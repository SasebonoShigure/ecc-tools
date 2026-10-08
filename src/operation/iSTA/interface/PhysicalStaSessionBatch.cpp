// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaSessionState.hpp"
#include "PhysicalStaBatchJson.hpp"
#include "StaCsrSessionContext.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DataManager.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"
#if ISTA_ENABLE_CUDA
#include "StaCudaSessionState.hpp"
#include "CudaTargetedExecution.hpp"
#include "CudaFullExecution.hpp"
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
  const bool incremental = request.update_strategy == StaUpdateStrategy::kIncremental;
  if ((!cuda && incremental && (!cpu || !cpu_native)) || (!incremental && !full))
    throw std::invalid_argument("physical batch backend is not enabled");
  const TimingEditResolverState* resolver = full ? &full->resolver() : cpu_native ? &cpu_native->resolver : nullptr;
#if ISTA_ENABLE_CUDA
  if (cuda) {
    if (!context._cuda || context._cuda->_poisoned || context._cuda->device.poisoned()) {
      authority.poison();
      throw std::runtime_error("physical CUDA owner is absent or poisoned");
    }
    if (incremental && context._cuda->native) resolver = &context._cuda->native->resolver;
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
       incremental ? StaExecutionKind::kIncrementalUpdate : StaExecutionKind::kFullUpdate,
       StaPropagationScope::kFullGraph, incremental, false, std::nullopt}};
  const auto levelization = incremental ? "mixed" : cuda ? "full" : "native_full";
  const auto bytes = recovery ? std::string{} : encodePhysicalStaRecoveryBatch(
      _baseline_id, edits, locations, result.execution, levelization, technology.dbu_per_um);
  std::string fallback_bytes;
  if (!recovery && incremental && request.fallback_policy == StaFallbackPolicy::kFullWithRecord) {
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
    std::optional<timing_dual_csr::PendingStructuredBatch> cpu_pending;
    if (!cuda && incremental) {
      if (cpu->poisoned() || cpu_native->generation != before)
        throw std::logic_error("physical native/CPU origin differs");
      cpu_pending.emplace(cpu->prepareBatch(edits));
    }
    if (!physical.apply()) throw std::runtime_error("physical batch apply failed");
    std::optional<NativeFullEditBatch> native_full;
    std::optional<NativeTargetedEditBatch> native_targeted;
    std::optional<timing_dual_csr::DualCsrVerifiedStage> cpu_candidate;
#if ISTA_ENABLE_CUDA
    std::optional<csr::CudaDualCsrSession> candidate;
    std::vector<std::string> names;
    std::vector<std::optional<std::size_t>> ranks;
#endif
    if (!cuda && incremental) {
      using namespace timing_dual_csr;
      native_targeted.emplace(NativeTargetedEditPreparer::prepare(
          database, authority, edits, *cpu_native, std::move(trees)));
      copyCreatedPhysicalPinCoordinates(physical, database);
      const auto capture = [&]() {
        const auto& committed = cpu->committed();
        auto captured = NarrowTimingRecordReader{context.libraryModels(database), &*native_targeted}.read(
            database, DualSemanticReadView{committed.graph}, committed.arc_group_index,
            cpu_pending->stagedResolver(), cpu_pending->preparedBatch(), cpu_pending->capturePlan());
        if (!captured.success())
          throw std::runtime_error("physical CPU narrow capture failed: " + std::to_string(static_cast<unsigned>(*captured.error())));
        return TimingEditDualCsrDescriptorBuilder::build(std::move(*cpu_pending), std::move(captured));
      };
      auto attempt = makeMixedLevelizedDualCsrAnalysisState(*cpu, capture());
      if (auto* ready = std::get_if<DualCsrVerifiedStage>(&attempt)) cpu_candidate.emplace(std::move(*ready));
      else {
        const auto* mixed = std::get_if<timing_incremental_levelization::MixedLevelizationFailure>(&attempt);
        if (!mixed || mixed->code != timing_incremental_levelization::MixedStageFailureCode::kOldLevelOrderConflict
            || request.fallback_policy != StaFallbackPolicy::kFullWithRecord)
          throw std::runtime_error("physical CPU incremental computation failed");
        // The failed stage has already undone graph/index changes. Native/IDB
        // edits remain in their local transaction for this explicitly allowed retry.
        cpu_pending.emplace(cpu->prepareBatch(edits));
        auto fallback = makeStagedDualCsrAnalysisState(*cpu, capture());
        if (!fallback.success()) throw std::runtime_error("physical CPU full fallback failed");
        cpu_candidate.emplace(std::move(*fallback.staged));
        result.execution.executed_update_strategy = StaUpdateStrategy::kFull;
        result.execution.execution_kind = StaExecutionKind::kFallbackFull;
        result.execution.incremental_batch_executed = false;
        result.execution.fallback_to_full = true;
        result.execution.fallback_reason = StaFallbackReason::kOldLevelOrderConflict;
      }
    } else
#if ISTA_ENABLE_CUDA
    if (cuda && incremental) {
      auto& owned = *context._cuda;
      if (!owned.native || owned.native->generation != before || owned.device.generation() != before)
        throw std::logic_error("physical native/device origin differs");
      native_targeted.emplace(NativeTargetedEditPreparer::prepare(database, authority, edits, *owned.native, std::move(trees)));
      copyCreatedPhysicalPinCoordinates(physical, database);
      std::optional<csr::CudaDualCsrTelemetry> rejected;
      candidate.emplace(csr::detail::stageCudaTargetedBatch(owned.device, native_targeted->patch,
          request.update_strategy, request.fallback_policy, result.execution, rejected));
    } else
#endif
    {
      native_full.emplace(NativeFullEditPreparer::prepare(database, authority, edits, *full, std::move(trees)));
      copyCreatedPhysicalPinCoordinates(physical, database);
      if (!cuda) NativeCandidateExecutor::executeFull(native_full->transaction());
#if ISTA_ENABLE_CUDA
      else {
        auto staged = csr::detail::stageCudaFullBatch(context, *native_full, context._cuda->device);
        names = std::move(staged.node_names);
        ranks = std::move(staged.arc_ranks);
        candidate.emplace(std::move(staged.candidate));
      }
#endif
      if (!native_full->canCommit() || native_full->targetGeneration().value() != result.generation)
        throw std::logic_error("physical native full qualification failed");
    }
    if (native_targeted && (!native_targeted->transaction.active()
        || native_targeted->staged.generation.value() != result.generation))
      throw std::logic_error("physical native targeted qualification failed");
    if (!cuda && incremental && (!cpu_candidate || !cpu->canPublish(*cpu_candidate)))
      throw std::logic_error("physical CPU publication qualification failed");
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
    if (cpu_candidate) {
      const auto status = cpu->publish(std::move(*cpu_candidate));
      if (!status.ok() || !status.published || status.poisoned)
        throw std::logic_error("physical CPU publication failed after the commit marker");
    }
    if (native_targeted) native_targeted->transaction.commit();
    if (native_full && !native_full->commit())
      throw std::logic_error("physical native publication failed after the commit marker");
    physical.commit();
    placement.commit();
#if ISTA_ENABLE_CUDA
    if (cuda) {
      if (incremental) context._cuda->committed();
      else context._cuda->committedFull(std::move(names), std::move(ranks));
    }
#endif
    if (!cuda && incremental) {
      _timing_generation.reset();
      _report_generation.reset();
    } else if (!cuda) {
      _timing_generation = TransactionGeneration{result.generation};
      _report_generation = TransactionGeneration{result.generation};
    }
  } catch (...) {
    // Undo runs while the authority and operation guard are live.
    bool damaged = marker_published || !_journal->healthy() || (cpu && cpu->poisoned());
#if ISTA_ENABLE_CUDA
    damaged |= cuda && context._cuda && (context._cuda->_poisoned || context._cuda->device.poisoned());
#endif
    if (damaged) authority.poison();
    throw;
  }
  return result;
}
}  // namespace ista
