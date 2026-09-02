// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "LevelizationExecutionTrace.hpp"

#include <cstddef>
#include <limits>
#include <utility>

#include "MixedLevelizationTelemetry.hpp"

namespace ista::timing_dual_csr {
namespace {

bool checkedAdd(std::uint64_t lhs, std::uint64_t rhs,
                std::uint64_t& result) noexcept
{
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
    return false;
  result = lhs + rhs;
  return true;
}

bool checkedSum4(std::uint64_t first, std::uint64_t second,
                 std::uint64_t third, std::uint64_t fourth,
                 std::uint64_t& result) noexcept
{
  std::uint64_t left = 0;
  std::uint64_t right = 0;
  return checkedAdd(first, second, left)
         && checkedAdd(third, fourth, right)
         && checkedAdd(left, right, result);
}

bool validateMixed(const MixedLevelizationDeterministicTrace& trace)
    noexcept
{
  std::uint64_t total = 0;
  if (!checkedAdd(trace.n_last_nodes, trace.n_new_nodes, total)
      || total != trace.n_current_nodes)
    return false;
  if (!checkedAdd(trace.n_old_levels, trace.n_new_nodes, total)
      || total != trace.mixed_vertex_count)
    return false;
  if (!checkedSum4(trace.old_old, trace.old_new, trace.new_old,
                   trace.new_new, total)
      || total != trace.actual_eligible_arc_visits)
    return false;
  if (!checkedSum4(0, trace.old_new, trace.new_old, trace.new_new,
                   total)
      || total != trace.mapped_actual_arc_candidates)
    return false;
  if (trace.actual_parallel_duplicates_collapsed
          > trace.mapped_actual_arc_candidates
      || trace.unique_actual_endpoint_pairs
             != trace.mapped_actual_arc_candidates
                    - trace.actual_parallel_duplicates_collapsed)
    return false;
  if (trace.proxy_alias_duplicates_collapsed
          > trace.unique_actual_endpoint_pairs)
    return false;
  const std::uint64_t mapped_unique
      = trace.unique_actual_endpoint_pairs
        - trace.proxy_alias_duplicates_collapsed;
  if (!checkedAdd(trace.virtual_chain_edges, mapped_unique, total)
      || total != trace.unique_precedence_edges)
    return false;
  if (!checkedAdd(trace.base_out, trace.aux_out, total)
      || total != trace.actual_eligible_arc_visits)
    return false;
  const std::uint64_t expected_virtual_chain
      = trace.n_old_levels == 0 ? 0 : trace.n_old_levels - 1;
  if (trace.virtual_chain_edges != expected_virtual_chain
      || trace.max_frontier > trace.mixed_vertex_count
      || (trace.mixed_vertex_count != 0 && trace.max_frontier == 0)
      || trace.reconstructed_level_count > trace.n_current_nodes
      || (trace.n_current_nodes != 0
          && trace.reconstructed_level_count == 0))
    return false;
  return trace.kahn_processed_count == trace.mixed_vertex_count
         && trace.actual_cycle_classifier_runs == 0
         && trace.actual_cycle_classifier_arc_visits == 0
         && trace.fallback_count == 0;
}

}  // namespace

LevelizationExecutionTrace::LevelizationExecutionTrace(
    timing_edit::TransactionGeneration generation_value,
    LevelizationExecutionMode mode_value,
    csr::CsrFifoLevelization levels_value,
    csr::CsrTraversalTelemetry traversal_value,
    csr::CsrFullLevelizerEntryDelta&& entry_delta,
    std::uint64_t fallback_count_value,
    bool supplied_levels_validated_value,
    std::optional<MixedLevelizationDeterministicTrace> mixed_value)
    : _generation(generation_value),
      _mode(mode_value),
      _levels(std::move(levels_value)),
      _traversal(traversal_value),
      _full_levelizer_entry_delta(entry_delta.value()),
      _fallback_count(fallback_count_value),
      _supplied_levels_validated(supplied_levels_validated_value),
      _mixed(std::move(mixed_value))
{
}

MixedLevelizationDeterministicTrace projectDeterministicMixedTrace(
    const timing_incremental_levelization::MixedLevelizationTelemetry&
        value) noexcept
{
  return {
      .n_last_nodes = value.n_last_nodes,
      .n_current_nodes = value.n_current_nodes,
      .n_new_nodes = value.n_new_nodes,
      .n_old_levels = value.n_old_levels,
      .mixed_vertex_count = value.mixed_vertex_count,
      .actual_eligible_arc_visits = value.actual_eligible_arc_visits,
      .old_old = value.old_old,
      .old_new = value.old_new,
      .new_old = value.new_old,
      .new_new = value.new_new,
      .mapped_actual_arc_candidates = value.mapped_actual_arc_candidates,
      .unique_actual_endpoint_pairs = value.unique_actual_endpoint_pairs,
      .actual_parallel_duplicates_collapsed
          = value.actual_parallel_duplicates_collapsed,
      .proxy_alias_duplicates_collapsed
          = value.proxy_alias_duplicates_collapsed,
      .unique_precedence_edges = value.unique_precedence_edges,
      .virtual_chain_edges = value.virtual_chain_edges,
      .base_out = value.base_out,
      .aux_out = value.aux_out,
      .kahn_processed_count = value.kahn_processed_count,
      .max_frontier = value.max_frontier,
      .reconstructed_level_count = value.reconstructed_level_count,
      .actual_cycle_classifier_runs
          = value.actual_cycle_classifier_runs,
      .actual_cycle_classifier_arc_visits
          = value.actual_cycle_classifier_arc_visits,
      .fallback_count = value.fallback_count};
}

bool validateLevelizationExecutionTraceShape(
    const LevelizationExecutionTrace& trace) noexcept
{
  if (trace.generation().value() == 0 || !trace.levels().complete
      || trace.levels().view_error.has_value()
      || trace.levels().level_offsets.empty())
    return false;
  if (trace.mode() == LevelizationExecutionMode::kFullStage5)
    return !trace.mixed().has_value()
           && trace.fullLevelizerEntryDelta() == 1
           && trace.fallbackCount() == 0
           && !trace.suppliedLevelsValidated();
  if (trace.mode() != LevelizationExecutionMode::kMixedStage6
      || !trace.mixed().has_value()
      || trace.fullLevelizerEntryDelta() != 0
      || trace.fallbackCount() != 0
      || !trace.suppliedLevelsValidated()
      || trace.traversal().levelization
             != csr::CsrStageStorageVisits{}
      || !validateMixed(*trace.mixed()))
    return false;
  const auto& mixed = *trace.mixed();
  if (trace.levels().level_nodes.size() != mixed.n_current_nodes
      || trace.levels().node_to_level.size() != mixed.n_current_nodes
      || trace.levels().level_offsets.size() - 1
             != mixed.reconstructed_level_count)
    return false;
  return trace.fallbackCount() == mixed.fallback_count;
}

}  // namespace ista::timing_dual_csr
