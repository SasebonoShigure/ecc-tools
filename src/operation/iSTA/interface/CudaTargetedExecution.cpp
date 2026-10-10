// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CudaTargetedExecution.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrParasiticTree.hpp"
#include "DataManager.hpp"
#include <stdexcept>

namespace ista::csr {
namespace {
void require(bool valid, const char* message)
{
  if (!valid) throw std::runtime_error(message);
}
}
CudaDualCsrPatch detail::makeCudaTargetedPatch(const timing_dual_csr::DualCsrState& before,
    const timing_dual_csr::DualCsrBatchDescriptor& descriptor, NativeLibraryModels& models)
{
  models.requireDatabase(STADM.getDatabase());
  require(before.session_token == descriptor.session_token && before.generation == descriptor.expected_generation
              && before.high_water == descriptor.before, "CUDA descriptor has stale before-state");
  CudaDualCsrPatch patch;
  patch.session_token = descriptor.session_token;
  patch.expected_generation = descriptor.expected_generation;
  patch.target_generation = descriptor.target_generation;
  patch.before = descriptor.before;
  patch.after = descriptor.after;
  patch.node_appends = descriptor.node_appends;
  patch.arc_appends = descriptor.arc_appends;
  patch.removes = descriptor.removes;
  patch.changes = descriptor.changes;
  patch.reverse_changes = descriptor.reverse_changes;
  patch.payload_changes = descriptor.payload_changes;
  patch.logical_deactivations = descriptor.logical_deactivations;
  patch.aux_rows = descriptor.aux_rows;
  patch.appended_base_out_rows = descriptor.appended_base_out_rows;
  for (const auto& row : descriptor.affected_base_in_rows)
    if (row.sink.value() >= descriptor.before.node_slots)
      patch.appended_base_in_rows.push_back({row.sink, row.replacement_arc_ids});

  std::map<NodeId, CsrParasiticBinding> old, next;
  const auto collect = [](auto& bindings, const CsrParasiticBinding& binding) {
    if (!binding.tree) return;
    const auto& tree = binding.tree;
    require(!tree->terminals.empty(), "CUDA descriptor RC tree lacks a driver");
    if (const auto found = bindings.find(tree->terminals.front()); found != bindings.end()) {
      require(found->second.tree == tree, "CUDA descriptor RC driver has conflicting owners");
      return;
    }
    for (std::size_t terminal = 0; terminal < tree->terminals.size(); ++terminal)
      require(bindings.emplace(tree->terminals[terminal], CsrParasiticBinding{tree, terminal}).second,
              "CUDA descriptor RC terminal belongs to multiple nets");
  };
  for (const auto& arc : descriptor.logical_deactivations)
    collect(old, before.arc_slots.at(arc.id.value()).payload.parasitic);
  for (const auto& arc : descriptor.payload_changes) {
    collect(old, arc.expected.parasitic);
    collect(next, arc.replacement.parasitic);
  }
  for (const auto& arc : descriptor.arc_appends) collect(next, arc.payload.parasitic);
  if (descriptor.reference_payload.start_seeds)
    for (const auto& seed : descriptor.reference_payload.start_seeds->changes) {
      collect(old, seed.expected.parasitic);
      collect(next, seed.replacement.parasitic);
    }
  for (const auto& [node, binding] : next) {
    const auto previous = old.find(node);
    patch.rc_bindings.push_back({node, previous == old.end() ? CsrParasiticBinding{} : previous->second, binding});
  }
  for (const auto& node : descriptor.node_appends)
    if (!node.payload.is_port && next.contains(node.id))
      patch.rc_load_cells.try_emplace(node.payload.instance_cell, std::cref(models.cell(node.payload.instance_cell)));
  return patch;
}
CudaDualCsrSession detail::stageCudaTargetedBatch(CudaDualCsrSession& owner, const CudaDualCsrPatch& patch, StaUpdateStrategy strategy,
                                                  StaFallbackPolicy fallback, StaExecutionRecord& execution, std::optional<CudaDualCsrTelemetry>& rejected_mixed_attempt)
{
  rejected_mixed_attempt.reset();
  require(strategy == StaUpdateStrategy::kFull || strategy == StaUpdateStrategy::kIncremental, "unknown update strategy");
  require(fallback == StaFallbackPolicy::kError || fallback == StaFallbackPolicy::kFullWithRecord, "unknown fallback policy");
  const bool incremental = strategy == StaUpdateStrategy::kIncremental;
  execution = {StaBackend::kCuda,
                     strategy,
                     fallback,
                     StaBackend::kCuda,
                     strategy,
                     incremental ? StaExecutionKind::kIncrementalUpdate : StaExecutionKind::kFullUpdate,
                     StaPropagationScope::kFullGraph,
                     incremental,
                     false,
                     std::nullopt};
  std::optional<CudaDualCsrSession> candidate(owner.stage(patch, incremental ? CudaLevelizationMode::kMixed : CudaLevelizationMode::kFull));
  if (candidate->computationCode() && candidate->telemetry().mixed_failure == timing_incremental_levelization::MixedStageFailureCode::kOldLevelOrderConflict
      && fallback == StaFallbackPolicy::kFullWithRecord) {
    if constexpr (incregpusta_debug_checks_enabled)
      rejected_mixed_attempt = candidate->telemetry();
    // Retry the same uncommitted edit against the healthy origin. Never
    // clear a candidate status or use fallback for cycles/CUDA failures.
    candidate.reset();
    candidate.emplace(owner.stage(patch, CudaLevelizationMode::kFull));
    execution.executed_update_strategy = StaUpdateStrategy::kFull;
    execution.execution_kind = StaExecutionKind::kFallbackFull;
    execution.incremental_batch_executed = false;
    execution.fallback_to_full = true;
    execution.fallback_reason = StaFallbackReason::kOldLevelOrderConflict;
  }
  require(!candidate->computationCode(), "targeted GPU update/propagation failed");
  require(!validateStaExecutionRecord(execution), "inconsistent actual CUDA execution record");
  return std::move(*candidate);
}

}  // namespace ista::csr
