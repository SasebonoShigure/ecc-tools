// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CudaTargetedExecution.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include <stdexcept>

namespace ista::csr {
namespace {
void require(bool valid, const char* message)
{
  if (!valid) throw std::runtime_error(message);
}
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
