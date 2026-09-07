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
#include "MixedLevelizationTelemetry.hpp"

namespace ista::timing_dual_csr {
class DualCsrStageFactoryAccess;

enum class LevelizationExecutionMode : std::uint8_t
{
  kFullStage5,
  kMixedStage6
};

using timing_incremental_levelization::MixedLevelizationDeterministicTrace;

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
  LevelizationExecutionTrace(timing_edit::TransactionGeneration generation, LevelizationExecutionMode mode, const csr::CsrFifoLevelization& levels,
                             csr::CsrTraversalTelemetry traversal, csr::CsrFullLevelizerEntryDelta&& full_levelizer_entry_delta, std::uint64_t fallback_count,
                             bool supplied_levels_validated, std::optional<MixedLevelizationDeterministicTrace> mixed);

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
