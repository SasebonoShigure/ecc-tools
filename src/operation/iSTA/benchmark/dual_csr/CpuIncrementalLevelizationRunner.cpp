// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CpuIncrementalLevelizationRunner.hpp"

#include <chrono>
#include <utility>

#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista::timing_dual_csr {
namespace {

using SteadyClock = std::chrono::steady_clock;

double elapsedMilliseconds(SteadyClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(
             SteadyClock::now() - start)
      .count();
}

}  // namespace

CpuIncrementalLevelizationRunResult
CpuIncrementalLevelizationRunner::run(
    const CommittedDualCsrAnalysisState& committed,
    VerifiedStructuredBatch&& verified_batch)
{
  const auto started = SteadyClock::now();
  auto attempt = makeMixedLevelizedDualCsrAnalysisState(
      committed, std::move(verified_batch));
  std::optional<DualCsrStageTimings> timings;
  if (const auto* staged = std::get_if<DualCsrVerifiedStage>(&attempt))
    timings = staged->timings();
  return {std::move(attempt), elapsedMilliseconds(started), timings};
}

}  // namespace ista::timing_dual_csr
