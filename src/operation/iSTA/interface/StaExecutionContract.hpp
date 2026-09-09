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
#pragma once

#include <optional>
#include <string>

namespace ista {

enum class StaBackend
{
  kCpu,
  kCuda
};
enum class StaUpdateStrategy
{
  kFull,
  kIncremental
};
enum class StaFallbackPolicy
{
  kError,
  kFullWithRecord
};
enum class StaExecutionKind
{
  kBaselineOnly,
  kFullUpdate,
  kIncrementalUpdate,
  kFallbackFull
};
enum class StaPropagationScope
{
  kNone,
  kFullGraph,
  kAffectedCone
};
enum class StaFallbackReason
{
  kNotApplicableNoMutation,
  kSameBatchInsertRemoveUnsupported,
  kCellMasterReplacementUnsupported,
  kUnsupportedEdit,
  kReverseSlotNotReplaceable,
  kNodeDeletionUnsupported,
  kCapacityFallback,
  kClockChanged,
  kConstraintChanged,
  kLibraryChanged,
  kCornerChanged,
  kParasiticChanged,
  kSequentialEditUnsupported,
  kBackendUnavailable,
  kContractViolation,
  kOldLevelOrderConflict,
};
enum class StaCapabilityReason
{
  kBackendUnavailable,
  kUnsupportedAnalysisFeatures
};
enum class StaSessionStatusCode
{
  kUnsupported,
  kInvalidState,
  kOwnerConflict,
  kInitFailed,
  kRunFailed,
  kCleanupFailed,
  kQuarantined,
};
enum class StaSessionState
{
  kCreated,
  kInitialized,
  kBaselineExecuted,
  kPoisoned,
  kClosed
};

struct StaExecutionRequest
{
  StaBackend backend = StaBackend::kCpu;
  StaUpdateStrategy update_strategy = StaUpdateStrategy::kFull;
  StaFallbackPolicy fallback_policy = StaFallbackPolicy::kError;

  bool operator==(const StaExecutionRequest&) const = default;
};

struct StaExecutionRecord
{
  StaBackend requested_backend;
  StaUpdateStrategy requested_update_strategy;
  StaFallbackPolicy fallback_policy;
  StaBackend executed_backend;
  StaUpdateStrategy executed_update_strategy;
  StaExecutionKind execution_kind;
  StaPropagationScope propagation_scope;
  bool incremental_batch_executed;
  bool fallback_to_full;
  std::optional<StaFallbackReason> fallback_reason;

  bool operator==(const StaExecutionRecord&) const = default;
};

struct StaStatus
{
  std::optional<StaSessionStatusCode> code;
  StaSessionState state = StaSessionState::kCreated;
  std::optional<StaCapabilityReason> capability_reason;
  std::string message;

  [[nodiscard]] bool ok() const { return !code.has_value(); }
  static StaStatus success(StaSessionState state);
  static StaStatus failure(StaSessionStatusCode code, StaSessionState state, std::string message,
                           std::optional<StaCapabilityReason> capability_reason = std::nullopt);
};

struct StaExecutionResult
{
  StaStatus status;
  std::optional<StaExecutionRecord> record;

  [[nodiscard]] bool ok() const { return status.ok() && record.has_value(); }
  static StaExecutionResult success(StaExecutionRecord record, StaSessionState state);
  static StaExecutionResult failure(StaStatus status);
};

StaBackend parseStaBackend(const std::string& value);
StaUpdateStrategy parseStaUpdateStrategy(const std::string& value);
StaFallbackPolicy parseStaFallbackPolicy(const std::string& value);

std::string toString(StaBackend value);
std::string toString(StaUpdateStrategy value);
std::string toString(StaFallbackPolicy value);
std::string toString(StaExecutionKind value);
std::string toString(StaPropagationScope value);
std::string toString(StaFallbackReason value);
std::string toString(StaCapabilityReason value);
std::string toString(StaSessionStatusCode value);
std::string toString(StaSessionState value);

std::optional<std::string> validateStaExecutionRecord(const StaExecutionRecord& record);
StaExecutionResult resolveStage1StaExecution(const StaExecutionRequest& request);

}  // namespace ista
