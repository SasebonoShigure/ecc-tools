// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "LevelizationExecutionTrace.hpp"

#include <cstddef>
#include <utility>

#include "IncreGpuStaBuildConfig.hpp"
#include "MixedLevelizationTelemetry.hpp"

namespace ista::timing_dual_csr {
namespace {

bool validateMixed(const MixedLevelizationDeterministicTrace& trace) noexcept
{
  if (!timing_incremental_levelization::validateMixedTelemetryIdentities(trace)
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

LevelizationExecutionTrace::LevelizationExecutionTrace(timing_edit::TransactionGeneration generation_value, LevelizationExecutionMode mode_value,
                                                       const csr::CsrFifoLevelization& levels_value, csr::CsrTraversalTelemetry traversal_value,
                                                       csr::CsrFullLevelizerEntryDelta&& entry_delta, std::uint64_t fallback_count_value,
                                                       bool supplied_levels_validated_value, std::optional<MixedLevelizationDeterministicTrace> mixed_value)
    : _generation(generation_value),
      _mode(mode_value),
      _levels(incregpusta_debug_checks_enabled ? levels_value : csr::CsrFifoLevelization{}),
      _traversal(traversal_value),
      _full_levelizer_entry_delta(entry_delta.value()),
      _fallback_count(fallback_count_value),
      _supplied_levels_validated(supplied_levels_validated_value),
      _mixed(incregpusta_debug_checks_enabled ? std::move(mixed_value) : std::nullopt)
{
}

MixedLevelizationDeterministicTrace projectDeterministicMixedTrace(
    const timing_incremental_levelization::MixedLevelizationTelemetry&
        value) noexcept
{
  // Copy only deterministic counters, excluding timing and oracle observations.
  return value;
}

bool validateLevelizationExecutionTraceShape(
    const LevelizationExecutionTrace& trace) noexcept
{
  if constexpr (!incregpusta_debug_checks_enabled)
    return trace.generation().value() != 0
           && (trace.mode() == LevelizationExecutionMode::kFullStage5 || trace.mode() == LevelizationExecutionMode::kMixedStage6);
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
