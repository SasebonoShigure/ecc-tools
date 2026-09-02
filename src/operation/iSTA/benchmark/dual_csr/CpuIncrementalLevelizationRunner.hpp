// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <optional>

#include "DualCsrTransactionCoordinator.hpp"

namespace ista::timing_dual_csr {

struct CpuIncrementalLevelizationRunResult
{
  MixedDualCsrStageAttempt attempt;
  double stage_attempt_ms{0.0};
  std::optional<DualCsrStageTimings> phase_timings;

  [[nodiscard]] bool success() const noexcept
  {
    return std::holds_alternative<DualCsrVerifiedStage>(attempt);
  }

  [[nodiscard]] DualCsrVerifiedStage* staged() noexcept
  {
    return std::get_if<DualCsrVerifiedStage>(&attempt);
  }
  [[nodiscard]] const DualCsrStageFailure* dualFailure() const noexcept
  {
    return std::get_if<DualCsrStageFailure>(&attempt);
  }
  [[nodiscard]] const
      timing_incremental_levelization::MixedLevelizationFailure*
  mixedFailure() const noexcept
  {
    return std::get_if<
        timing_incremental_levelization::MixedLevelizationFailure>(
            &attempt);
  }
};

class CpuIncrementalLevelizationRunner final
{
 public:
  [[nodiscard]] static CpuIncrementalLevelizationRunResult run(
      const CommittedDualCsrAnalysisState& committed,
      VerifiedStructuredBatch&& verified_batch);
};

}  // namespace ista::timing_dual_csr
