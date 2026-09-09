// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include <array>
#include <stdexcept>
#include <string>

#include "IncreGpuStaBuildConfig.hpp"
#include "StaExecutionContract.hpp"

namespace {

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Callable>
void requireInvalid(Callable callable)
{
  try {
    callable();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("expected invalid_argument");
}

}  // namespace

int main()
{
  using namespace ista;
  require(parseStaBackend("cpu") == StaBackend::kCpu, "cpu parse");
  require(parseStaBackend("cuda") == StaBackend::kCuda, "cuda parse");
  require(parseStaUpdateStrategy("full") == StaUpdateStrategy::kFull, "full parse");
  require(parseStaUpdateStrategy("incremental") == StaUpdateStrategy::kIncremental, "incremental parse");
  require(parseStaFallbackPolicy("error") == StaFallbackPolicy::kError, "error parse");
  require(parseStaFallbackPolicy("full_with_record") == StaFallbackPolicy::kFullWithRecord, "fallback parse");
  requireInvalid([] { parseStaBackend(""); });
  requireInvalid([] { parseStaBackend("CPU"); });
  requireInvalid([] { parseStaUpdateStrategy("increment"); });
  requireInvalid([] { parseStaFallbackPolicy("full"); });

  for (StaBackend backend : {StaBackend::kCpu, StaBackend::kCuda}) {
    for (StaUpdateStrategy strategy : {StaUpdateStrategy::kFull, StaUpdateStrategy::kIncremental}) {
      for (StaFallbackPolicy policy : {StaFallbackPolicy::kError, StaFallbackPolicy::kFullWithRecord}) {
        StaExecutionRequest request{backend, strategy, policy};
        StaExecutionResult result = resolveStage1StaExecution(request);
        if (backend == StaBackend::kCuda && !incregpusta_cuda_enabled) {
          require(!result.ok() && !result.record, "CUDA must be unsupported without a record");
          require(result.status.code == StaSessionStatusCode::kUnsupported, "CUDA status code");
          require(result.status.capability_reason == StaCapabilityReason::kBackendUnavailable, "CUDA capability reason");
          continue;
        }
        require(result.ok(), "available backend request must resolve");
        require(result.record->executed_backend == backend, "resolved backend must match request");
        require(!validateStaExecutionRecord(*result.record), "resolved record must validate");
        require(result.record->fallback_policy == policy, "fallback policy must be recorded exactly");
        if (strategy == StaUpdateStrategy::kFull) {
          require(result.record->execution_kind == StaExecutionKind::kFullUpdate, "full execution kind");
          require(!result.record->fallback_reason, "full fallback reason must be null");
        } else {
          require(result.record->execution_kind == StaExecutionKind::kBaselineOnly, "incremental baseline kind");
          require(result.record->fallback_reason == StaFallbackReason::kNotApplicableNoMutation, "baseline reason");
        }
      }
    }
  }

  StaExecutionRecord incremental{.requested_backend = StaBackend::kCuda,
                                 .requested_update_strategy = StaUpdateStrategy::kIncremental,
                                 .fallback_policy = StaFallbackPolicy::kError,
                                 .executed_backend = StaBackend::kCuda,
                                 .executed_update_strategy = StaUpdateStrategy::kIncremental,
                                 .execution_kind = StaExecutionKind::kIncrementalUpdate,
                                 .propagation_scope = StaPropagationScope::kFullGraph,
                                 .incremental_batch_executed = true,
                                 .fallback_to_full = false,
                                 .fallback_reason = std::nullopt};
  require(!validateStaExecutionRecord(incremental), "incremental_update validator");

  StaExecutionRecord fallback{.requested_backend = StaBackend::kCuda,
                              .requested_update_strategy = StaUpdateStrategy::kIncremental,
                              .fallback_policy = StaFallbackPolicy::kFullWithRecord,
                              .executed_backend = StaBackend::kCpu,
                              .executed_update_strategy = StaUpdateStrategy::kFull,
                              .execution_kind = StaExecutionKind::kFallbackFull,
                              .propagation_scope = StaPropagationScope::kFullGraph,
                              .incremental_batch_executed = false,
                              .fallback_to_full = true,
                              .fallback_reason = StaFallbackReason::kBackendUnavailable};
  require(!validateStaExecutionRecord(fallback), "fallback_full validator");
  fallback.fallback_to_full = false;
  require(validateStaExecutionRecord(fallback).has_value(), "fallback_full negative validator");
  incremental.executed_backend = StaBackend::kCpu;
  require(validateStaExecutionRecord(incremental).has_value(), "backend mismatch negative validator");

  constexpr std::array fallback_reasons{StaFallbackReason::kNotApplicableNoMutation,
                                        StaFallbackReason::kSameBatchInsertRemoveUnsupported,
                                        StaFallbackReason::kCellMasterReplacementUnsupported,
                                        StaFallbackReason::kUnsupportedEdit,
                                        StaFallbackReason::kReverseSlotNotReplaceable,
                                        StaFallbackReason::kNodeDeletionUnsupported,
                                        StaFallbackReason::kCapacityFallback,
                                        StaFallbackReason::kClockChanged,
                                        StaFallbackReason::kConstraintChanged,
                                        StaFallbackReason::kLibraryChanged,
                                        StaFallbackReason::kCornerChanged,
                                        StaFallbackReason::kParasiticChanged,
                                        StaFallbackReason::kSequentialEditUnsupported,
                                        StaFallbackReason::kBackendUnavailable,
                                        StaFallbackReason::kContractViolation};
  for (StaFallbackReason reason : fallback_reasons)
    require(!toString(reason).empty(), "fallback reason string");
  return 0;
}
