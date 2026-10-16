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
#include "StaExecutionContract.hpp"

#include "IncreGpuStaBuildConfig.hpp"
#if ISTA_ENABLE_CUDA
#include "CsrCudaBackend.hpp"
#endif

#include <stdexcept>
#include <utility>

namespace ista {
namespace {

template <typename Enum>
Enum invalidToken(const char* field, const std::string& value)
{
  throw std::invalid_argument(std::string("invalid ") + field + " token: '" + value + "'");
}

bool sameBackend(const StaExecutionRecord& record)
{
  return record.requested_backend == record.executed_backend;
}

bool commonFullGraph(const StaExecutionRecord& record)
{
  return record.propagation_scope == StaPropagationScope::kFullGraph && !record.incremental_batch_executed;
}

}  // namespace

StaStatus StaStatus::success(StaSessionState state)
{
  return {.code = std::nullopt, .state = state, .capability_reason = std::nullopt, .message = {}};
}

StaStatus StaStatus::failure(StaSessionStatusCode code, StaSessionState state, std::string message, std::optional<StaCapabilityReason> capability_reason)
{
  return {.code = code, .state = state, .capability_reason = capability_reason, .message = std::move(message)};
}

StaExecutionResult StaExecutionResult::success(StaExecutionRecord record, StaSessionState state)
{
  return {.status = StaStatus::success(state), .record = std::move(record)};
}

StaExecutionResult StaExecutionResult::failure(StaStatus status)
{
  return {.status = std::move(status), .record = std::nullopt};
}

StaBackend parseStaBackend(const std::string& value)
{
  if (value == "cpu")
    return StaBackend::kCpu;
  if (value == "cuda")
    return StaBackend::kCuda;
  return invalidToken<StaBackend>("STA backend", value);
}

StaUpdateStrategy parseStaUpdateStrategy(const std::string& value)
{
  if (value == "full")
    return StaUpdateStrategy::kFull;
  if (value == "incremental")
    return StaUpdateStrategy::kIncremental;
  return invalidToken<StaUpdateStrategy>("STA update strategy", value);
}

StaFallbackPolicy parseStaFallbackPolicy(const std::string& value)
{
  if (value == "error")
    return StaFallbackPolicy::kError;
  if (value == "full_with_record")
    return StaFallbackPolicy::kFullWithRecord;
  return invalidToken<StaFallbackPolicy>("STA fallback policy", value);
}

std::string toString(StaBackend value)
{
  switch (value) {
    case StaBackend::kCpu:
      return "cpu";
    case StaBackend::kCuda:
      return "cuda";
  }
  throw std::logic_error("unknown StaBackend value");
}

std::string toString(StaUpdateStrategy value)
{
  switch (value) {
    case StaUpdateStrategy::kFull:
      return "full";
    case StaUpdateStrategy::kIncremental:
      return "incremental";
  }
  throw std::logic_error("unknown StaUpdateStrategy value");
}

std::string toString(StaFallbackPolicy value)
{
  switch (value) {
    case StaFallbackPolicy::kError:
      return "error";
    case StaFallbackPolicy::kFullWithRecord:
      return "full_with_record";
  }
  throw std::logic_error("unknown StaFallbackPolicy value");
}

std::string toString(StaExecutionKind value)
{
  switch (value) {
    case StaExecutionKind::kBaselineOnly:
      return "baseline_only";
    case StaExecutionKind::kFullUpdate:
      return "full_update";
    case StaExecutionKind::kIncrementalUpdate:
      return "incremental_update";
    case StaExecutionKind::kFallbackFull:
      return "fallback_full";
  }
  throw std::logic_error("unknown StaExecutionKind value");
}

std::string toString(StaPropagationScope value)
{
  switch (value) {
    case StaPropagationScope::kNone:
      return "none";
    case StaPropagationScope::kFullGraph:
      return "full_graph";
    case StaPropagationScope::kAffectedCone:
      return "affected_cone";
  }
  throw std::logic_error("unknown StaPropagationScope value");
}

std::string toString(StaFallbackReason value)
{
  switch (value) {
    case StaFallbackReason::kNotApplicableNoMutation:
      return "not_applicable_no_mutation";
    case StaFallbackReason::kSameBatchInsertRemoveUnsupported:
      return "same_batch_insert_remove_unsupported";
    case StaFallbackReason::kCellMasterReplacementUnsupported:
      return "cell_master_replacement_unsupported";
    case StaFallbackReason::kUnsupportedEdit:
      return "unsupported_edit";
    case StaFallbackReason::kReverseSlotNotReplaceable:
      return "reverse_slot_not_replaceable";
    case StaFallbackReason::kNodeDeletionUnsupported:
      return "node_deletion_unsupported";
    case StaFallbackReason::kCapacityFallback:
      return "capacity_fallback";
    case StaFallbackReason::kClockChanged:
      return "clock_changed";
    case StaFallbackReason::kConstraintChanged:
      return "constraint_changed";
    case StaFallbackReason::kLibraryChanged:
      return "library_changed";
    case StaFallbackReason::kCornerChanged:
      return "corner_changed";
    case StaFallbackReason::kParasiticChanged:
      return "parasitic_changed";
    case StaFallbackReason::kSequentialEditUnsupported:
      return "sequential_edit_unsupported";
    case StaFallbackReason::kBackendUnavailable:
      return "backend_unavailable";
    case StaFallbackReason::kOldLevelOrderConflict:
      return "old_level_order_conflict";
    case StaFallbackReason::kContractViolation:
      return "contract_violation";
  }
  throw std::logic_error("unknown StaFallbackReason value");
}

std::string toString(StaCapabilityReason value)
{
  switch (value) {
    case StaCapabilityReason::kBackendUnavailable:
      return "backend_unavailable";
    case StaCapabilityReason::kUnsupportedAnalysisFeatures:
      return "unsupported_analysis_features";
    case StaCapabilityReason::kUnsupportedExecutionMode:
      return "unsupported_execution_mode";
  }
  throw std::logic_error("unknown StaCapabilityReason value");
}

std::string toString(StaSessionStatusCode value)
{
  switch (value) {
    case StaSessionStatusCode::kUnsupported:
      return "unsupported";
    case StaSessionStatusCode::kInvalidState:
      return "invalid_state";
    case StaSessionStatusCode::kOwnerConflict:
      return "owner_conflict";
    case StaSessionStatusCode::kInitFailed:
      return "init_failed";
    case StaSessionStatusCode::kRunFailed:
      return "run_failed";
    case StaSessionStatusCode::kCleanupFailed:
      return "cleanup_failed";
    case StaSessionStatusCode::kQuarantined:
      return "quarantined";
  }
  throw std::logic_error("unknown StaSessionStatusCode value");
}

std::string toString(StaSessionState value)
{
  switch (value) {
    case StaSessionState::kCreated:
      return "created";
    case StaSessionState::kInitialized:
      return "initialized";
    case StaSessionState::kBaselineExecuted:
      return "baseline_executed";
    case StaSessionState::kPoisoned:
      return "poisoned";
    case StaSessionState::kClosed:
      return "closed";
  }
  throw std::logic_error("unknown StaSessionState value");
}

std::optional<std::string> validateStaExecutionRecord(const StaExecutionRecord& record)
{
  if (!sameBackend(record) && record.execution_kind != StaExecutionKind::kFallbackFull) {
    return "backend mismatch requires fallback_full";
  }
  switch (record.execution_kind) {
    case StaExecutionKind::kFullUpdate:
      if (record.requested_update_strategy != StaUpdateStrategy::kFull || record.executed_update_strategy != StaUpdateStrategy::kFull || !sameBackend(record)
          || !commonFullGraph(record) || record.fallback_to_full || record.fallback_reason) {
        return "invalid full_update execution record";
      }
      break;
    case StaExecutionKind::kBaselineOnly:
      if (record.requested_update_strategy != StaUpdateStrategy::kIncremental || record.executed_update_strategy != StaUpdateStrategy::kFull
          || !sameBackend(record) || !commonFullGraph(record) || record.fallback_to_full
          || record.fallback_reason != StaFallbackReason::kNotApplicableNoMutation) {
        return "invalid baseline_only execution record";
      }
      break;
    case StaExecutionKind::kIncrementalUpdate:
      if (record.requested_update_strategy != StaUpdateStrategy::kIncremental || record.executed_update_strategy != StaUpdateStrategy::kIncremental
          || !sameBackend(record) || record.propagation_scope != StaPropagationScope::kFullGraph || !record.incremental_batch_executed
          || record.fallback_to_full || record.fallback_reason) {
        return "invalid incremental_update execution record";
      }
      break;
    case StaExecutionKind::kFallbackFull:
      if (record.requested_update_strategy != StaUpdateStrategy::kIncremental || record.executed_update_strategy != StaUpdateStrategy::kFull
          || record.fallback_policy != StaFallbackPolicy::kFullWithRecord || !commonFullGraph(record) || !record.fallback_to_full || !record.fallback_reason) {
        return "invalid fallback_full execution record";
      }
      break;
  }
  return std::nullopt;
}

StaExecutionResult resolveStage1StaExecution(const StaExecutionRequest& request)
{
  if ((request.backend == StaBackend::kCpu && request.update_strategy != StaUpdateStrategy::kFull)
      || (request.backend == StaBackend::kCuda && request.update_strategy != StaUpdateStrategy::kIncremental)) {
    return StaExecutionResult::failure(StaStatus::failure(
        StaSessionStatusCode::kUnsupported, StaSessionState::kCreated,
        "STA supports only cpu/full and cuda/incremental requests", StaCapabilityReason::kUnsupportedExecutionMode));
  }
  if (request.backend == StaBackend::kCuda) {
#if ISTA_ENABLE_CUDA
    try {
      csr::CsrCudaBackend::requireDevice();
    } catch (const std::exception& error) {
      return StaExecutionResult::failure(
          StaStatus::failure(StaSessionStatusCode::kUnsupported, StaSessionState::kCreated, error.what(), StaCapabilityReason::kBackendUnavailable));
    }
#else
    return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kUnsupported, StaSessionState::kCreated, "CUDA STA backend was not built",
                                                          StaCapabilityReason::kBackendUnavailable));
#endif
  }
  const bool incremental = request.update_strategy == StaUpdateStrategy::kIncremental;
  StaExecutionRecord record{.requested_backend = request.backend,
                            .requested_update_strategy = request.update_strategy,
                            .fallback_policy = request.fallback_policy,
                            .executed_backend = request.backend,
                            .executed_update_strategy = StaUpdateStrategy::kFull,
                            .execution_kind = incremental ? StaExecutionKind::kBaselineOnly : StaExecutionKind::kFullUpdate,
                            .propagation_scope = StaPropagationScope::kFullGraph,
                            .incremental_batch_executed = false,
                            .fallback_to_full = false,
                            .fallback_reason = incremental ? std::optional{StaFallbackReason::kNotApplicableNoMutation} : std::nullopt};
  if (auto error = validateStaExecutionRecord(record)) {
    return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kRunFailed, StaSessionState::kCreated, *error));
  }
  return StaExecutionResult::success(std::move(record), StaSessionState::kCreated);
}

}  // namespace ista
