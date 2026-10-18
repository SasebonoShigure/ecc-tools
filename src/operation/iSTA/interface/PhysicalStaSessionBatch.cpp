// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaSessionState.hpp"
#include "PhysicalStaBatchJson.hpp"
#include "StaCsrSessionContext.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DataManager.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#if ISTA_ENABLE_CUDA
#include "TimingEditDualCsrDescriptorBuilder.hpp"
#include "StaCudaSessionState.hpp"
#include "CudaTargetedExecution.hpp"
#endif

namespace ista {
PhysicalStaBatchResult PhysicalStaSessionState::applyBatch(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority, const timing_edit::EditBatch& edits,
    std::span<const PhysicalBufferLocation> locations)
{
  return applyBatchImpl(database, context, authority, edits, locations, false);
}
PhysicalStaBatchResult PhysicalStaSessionState::applyBatchImpl(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority, const timing_edit::EditBatch& edits,
    std::span<const PhysicalBufferLocation> locations, bool recovery)
{
  using namespace timing_edit;
  const bool cuda = request.backend == StaBackend::kCuda;
  if (!cuda && !full)
    throw std::invalid_argument("physical batch backend is not enabled");
  const TimingEditResolverState* resolver = full ? &full->resolver() : nullptr;
#if ISTA_ENABLE_CUDA
  if (cuda) {
    if (!context._cuda || context._cuda->_poisoned || context._cuda->device.poisoned()) {
      authority.poison();
      throw std::runtime_error("physical CUDA owner is absent or poisoned");
    }
    if (cuda_graph) resolver = &cuda_graph->resolver;
  }
#else
  if (cuda) throw std::invalid_argument("CUDA physical STA was not built");
#endif
  const auto before = generation(context);
  if (!authority.readyForQuery() || authority.sessionToken().value() != context.token() || &database != &STADM.getDatabase()
      || !resolver || !_journal || !_journal->healthy()
      || (recovery ? _journal->generation() <= before.value() : _journal->generation() != before.value())
      || edits.expected_generation != before)
    throw std::invalid_argument("physical batch owner, journal or generation is invalid");
  PhysicalStaBatchResult result{before.value() + 1,
      {request.backend, request.update_strategy, request.fallback_policy, request.backend, request.update_strategy,
       cuda ? StaExecutionKind::kIncrementalUpdate : StaExecutionKind::kFullUpdate,
       StaPropagationScope::kFullGraph, cuda, false, std::nullopt}};
  const auto levelization = cuda ? "mixed" : "native_full";
  const auto bytes = recovery ? std::string{} : encodePhysicalStaRecoveryBatch(
      _baseline_id, edits, locations, result.execution, levelization, technology.dbu_per_um);
  std::string fallback_bytes;
  if (!recovery && cuda && request.fallback_policy == StaFallbackPolicy::kFullWithRecord) {
    auto fallback = result.execution;
    fallback.executed_update_strategy = StaUpdateStrategy::kFull;
    fallback.execution_kind = StaExecutionKind::kFallbackFull;
    fallback.incremental_batch_executed = false;
    fallback.fallback_to_full = true;
    fallback.fallback_reason = StaFallbackReason::kOldLevelOrderConflict;
    fallback_bytes = encodePhysicalStaRecoveryBatch(_baseline_id, edits, locations, fallback, "full", technology.dbu_per_um);
  }
  bool marker_published = recovery;
  try {
    auto physical = preparePhysicalTimingEdit(design, edits, *resolver, locations, technology.dbu_per_um);
    auto placement = placementRows().prepare(physical);
    auto trees = physical.prepareRc(technology);
#if ISTA_ENABLE_CUDA
    std::optional<timing_dual_csr::PendingStructuredBatch> pending;
    if (cuda) {
      if (!cuda_graph || cuda_graph->graph.generation != before || context._cuda->device.generation() != before)
        throw std::logic_error("physical CSR/device origin differs");
      pending.emplace(cuda_graph->prepareBatch(edits));
    }
#endif
    if (!physical.apply()) throw std::runtime_error("physical batch apply failed");
    std::optional<NativeFullEditBatch> native_full;
#if ISTA_ENABLE_CUDA
    std::optional<CudaCsrEditState::Patch> host_patch;
    std::optional<csr::CudaDualCsrSession> candidate;
#endif
#if ISTA_ENABLE_CUDA
    if (cuda) {
      std::vector<csr::ParasiticNetModel> rc;
      rc.reserve(trees.size());
      for (const auto& [name, tree] : trees)
        rc.push_back(csr::detail::projectCanonicalParasiticNet(name, tree));
      using namespace timing_dual_csr;
      auto& host = *cuda_graph;
      auto direct = TimingEditDualCsrDescriptorBuilder::buildDirect(
          std::move(*pending), authority, context.libraryModels(database), rc);
      // Preserve before-state until the device patch and all local undo are prepared.
      auto patch = csr::detail::makeCudaTargetedPatch(host.graph, direct.descriptor(), context.libraryModels(database));
      host_patch.emplace(host, std::move(direct));
      host_patch->apply();
      std::optional<csr::CudaDualCsrTelemetry> rejected;
      candidate.emplace(csr::detail::stageCudaTargetedBatch(context._cuda->device, patch,
          request.update_strategy, request.fallback_policy, result.execution, rejected));
    } else
#endif
    {
      native_full.emplace(NativeFullEditPreparer::prepare(database, authority, edits, *full, std::move(trees)));
      copyCreatedPhysicalPinCoordinates(physical, database);
      NativeCandidateExecutor::executeFullTiming(native_full->transaction());
      if (!native_full->canCommit() || native_full->targetGeneration().value() != result.generation)
        throw std::logic_error("physical native full qualification failed");
    }
    if (!physical.applied() || (!recovery && _journal->generation() != before.value()))
      throw std::logic_error("physical common publication qualification failed");
#if ISTA_ENABLE_CUDA
    if (cuda && (!candidate || !context._cuda->device.canPublish(*candidate)
                 || candidate->generation().value() != result.generation))
      throw std::logic_error("physical device publication qualification failed");
#endif
    // Actual fallback is known now. All recovery serialization was prepared
    // before apply; output failure here still unwinds every local transaction.
    std::unique_ptr<PhysicalStaJournal::Prepared> record;
    if (!recovery) record.reset(new PhysicalStaJournal::Prepared(
        _journal->prepare(result.generation, result.execution.fallback_to_full ? fallback_bytes : bytes)));
    if (record) record->publish();
    marker_published = true;
#if ISTA_ENABLE_CUDA
    if (cuda && !context._cuda->device.publish(std::move(*candidate)))
      throw std::logic_error("physical device publication failed after the commit marker");
#endif
    if (native_full && !native_full->commit())
      throw std::logic_error("physical native publication failed after the commit marker");
    physical.commit();
    placement.commit();
#if ISTA_ENABLE_CUDA
    if (cuda) {
      host_patch->commit();
      context._cuda->committed();
    }
#endif
    _report_generation.reset();
    _csr_report_generation.reset();
    if (cuda)
      _timing_generation.reset();
    else
      _timing_generation = TransactionGeneration{result.generation};
  } catch (...) {
    // Undo runs while the authority and operation guard are live.
    bool damaged = marker_published || !_journal->healthy();
#if ISTA_ENABLE_CUDA
    damaged |= cuda && context._cuda && (context._cuda->_poisoned || context._cuda->device.poisoned());
#endif
    if (damaged) authority.poison();
    throw;
  }
  return result;
}
}  // namespace ista
