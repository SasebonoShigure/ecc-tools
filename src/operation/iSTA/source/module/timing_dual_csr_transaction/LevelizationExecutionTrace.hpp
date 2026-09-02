// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "ActiveCsrEdgeView.hpp"
#include "CsrFifoLevelization.hpp"
#include "TimingEditTypes.hpp"

namespace ista::timing_incremental_levelization {
struct MixedLevelizationTelemetry;
}

namespace ista::timing_dual_csr {
class DualCsrStageFactoryAccess;

enum class LevelizationExecutionMode : std::uint8_t
{
  kFullStage5,
  kMixedStage6
};

// Deterministic projection of MixedLevelizationTelemetry. Run-envelope
// timings and the fresh-oracle-derived legal_noncompact observation are
// intentionally absent from committed state and recovery equality.
struct MixedLevelizationDeterministicTrace
{
  std::uint64_t n_last_nodes{0};
  std::uint64_t n_current_nodes{0};
  std::uint64_t n_new_nodes{0};
  std::uint64_t n_old_levels{0};
  std::uint64_t mixed_vertex_count{0};
  std::uint64_t actual_eligible_arc_visits{0};
  std::uint64_t old_old{0};
  std::uint64_t old_new{0};
  std::uint64_t new_old{0};
  std::uint64_t new_new{0};
  std::uint64_t mapped_actual_arc_candidates{0};
  std::uint64_t unique_actual_endpoint_pairs{0};
  std::uint64_t actual_parallel_duplicates_collapsed{0};
  std::uint64_t proxy_alias_duplicates_collapsed{0};
  std::uint64_t unique_precedence_edges{0};
  std::uint64_t virtual_chain_edges{0};
  std::uint64_t base_out{0};
  std::uint64_t aux_out{0};
  std::uint64_t kahn_processed_count{0};
  std::uint64_t max_frontier{0};
  std::uint64_t reconstructed_level_count{0};
  std::uint64_t actual_cycle_classifier_runs{0};
  std::uint64_t actual_cycle_classifier_arc_visits{0};
  std::uint64_t fallback_count{0};

  friend bool operator==(const MixedLevelizationDeterministicTrace&,
                         const MixedLevelizationDeterministicTrace&)
      = default;
};

class LevelizationExecutionTrace final
{
 public:
  LevelizationExecutionTrace(const LevelizationExecutionTrace&) = default;
  LevelizationExecutionTrace& operator=(const LevelizationExecutionTrace&)
      = default;
  LevelizationExecutionTrace(LevelizationExecutionTrace&&) noexcept
      = default;
  LevelizationExecutionTrace& operator=(LevelizationExecutionTrace&&)
      noexcept = default;

  [[nodiscard]] timing_edit::TransactionGeneration generation() const
      noexcept
  {
    return _generation;
  }

  [[nodiscard]] LevelizationExecutionMode mode() const noexcept
  {
    return _mode;
  }

  [[nodiscard]] const csr::CsrFifoLevelization& levels() const noexcept
  {
    return _levels;
  }

  [[nodiscard]] const csr::CsrTraversalTelemetry& traversal() const
      noexcept
  {
    return _traversal;
  }

  [[nodiscard]] std::uint64_t fullLevelizerEntryDelta() const noexcept
  {
    return _full_levelizer_entry_delta;
  }

  [[nodiscard]] std::uint64_t fallbackCount() const noexcept
  {
    return _fallback_count;
  }

  [[nodiscard]] bool suppliedLevelsValidated() const noexcept
  {
    return _supplied_levels_validated;
  }

  [[nodiscard]] const std::optional<MixedLevelizationDeterministicTrace>&
  mixed() const noexcept
  {
    return _mixed;
  }

  friend bool operator==(const LevelizationExecutionTrace&,
                         const LevelizationExecutionTrace&) = default;

 private:
  LevelizationExecutionTrace(
      timing_edit::TransactionGeneration generation,
      LevelizationExecutionMode mode, csr::CsrFifoLevelization levels,
      csr::CsrTraversalTelemetry traversal,
      csr::CsrFullLevelizerEntryDelta&& full_levelizer_entry_delta,
      std::uint64_t fallback_count, bool supplied_levels_validated,
      std::optional<MixedLevelizationDeterministicTrace> mixed);

  timing_edit::TransactionGeneration _generation;
  LevelizationExecutionMode _mode{LevelizationExecutionMode::kFullStage5};
  csr::CsrFifoLevelization _levels;
  csr::CsrTraversalTelemetry _traversal;
  std::uint64_t _full_levelizer_entry_delta{0};
  std::uint64_t _fallback_count{0};
  bool _supplied_levels_validated{false};
  std::optional<MixedLevelizationDeterministicTrace> _mixed;

  friend class DualCsrStageFactoryAccess;
};

static_assert(!std::is_aggregate_v<LevelizationExecutionTrace>);
static_assert(!std::is_assignable_v<
              decltype(std::declval<LevelizationExecutionTrace&>().levels()),
              csr::CsrFifoLevelization>);

[[nodiscard]] MixedLevelizationDeterministicTrace
projectDeterministicMixedTrace(
    const timing_incremental_levelization::MixedLevelizationTelemetry&
        telemetry) noexcept;

[[nodiscard]] bool validateLevelizationExecutionTraceShape(
    const LevelizationExecutionTrace& trace) noexcept;

}  // namespace ista::timing_dual_csr
