// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "DualCsrTransactionCoordinator.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista::timing_dual_csr {

enum class DualCsrModeRecoveryReplayCode : std::uint8_t
{
  kOk,
  kInvalidPoisonedState,
  kInvalidGenerationZero,
  kSessionTokenDidNotChange,
  kRecoveredAggregateRebindRejected,
  kBatchSourceRejected,
  kFullStageRejected,
  kMixedStageRejected,
  kModeRecordMismatch,
  kPublishRejected,
  kDeterministicMismatch
};

// This is a run envelope, not committed telemetry. In particular, it has no
// wall-clock fields and a recovery replay is not counted as a new incremental
// success in the recovered aggregate.
struct DualCsrModeRecoveryReplayEvidence
{
  std::uint64_t generation_zero{0};
  std::uint64_t generation_recovered{0};
  std::uint64_t descriptors_replayed{0};
  std::uint64_t full_records_replayed{0};
  std::uint64_t mixed_records_replayed{0};
  std::uint64_t recovery_replay_count{0};

  friend bool operator==(const DualCsrModeRecoveryReplayEvidence&,
                         const DualCsrModeRecoveryReplayEvidence&)
      = default;
};

struct DualCsrModeRecoveryBatchRequest
{
  std::size_t batch_index{0};
  const CommittedDualCsrAnalysisState& exact_recovered_before;
  const DualCsrBatchDescriptor& persisted_descriptor;
  const timing_edit::DesignEditPlan& persisted_semantic_batch;
  const LevelizationExecutionTrace& persisted_execution;
};

struct DualCsrModeRecoveryBatchAttempt
{
  std::optional<VerifiedStructuredBatch> verified_batch;
  DualCsrViolations violations;
  std::string detail;

  [[nodiscard]] bool success() const noexcept
  {
    return verified_batch.has_value() && violations.empty()
           && detail.empty();
  }
};

// Native replay adapters implement this boundary. Each invocation must create
// a fresh move-only capability from the current recovered state; retaining or
// copying a prior aggregate is not a valid implementation.
class DualCsrModeRecoveryBatchSource
{
 public:
  virtual ~DualCsrModeRecoveryBatchSource() = default;

  [[nodiscard]] virtual DualCsrModeRecoveryBatchAttempt rebuild(
      const DualCsrModeRecoveryBatchRequest& request)
      = 0;
};

struct DualCsrModeRecoveryReplayFailure
{
  DualCsrModeRecoveryReplayCode code{
      DualCsrModeRecoveryReplayCode::kInvalidPoisonedState};
  std::uint64_t generation{0};
  DualCsrViolations violations;
  std::optional<DualCsrStageFailure> dual_stage_failure;
  std::optional<
      timing_incremental_levelization::MixedLevelizationFailure>
      mixed_failure;
  std::string detail;
};

struct DualCsrModeRecoveryReplayAttempt;

class DualCsrVerifiedModeRecovery final
{
 public:
  DualCsrVerifiedModeRecovery(const DualCsrVerifiedModeRecovery&) = delete;
  DualCsrVerifiedModeRecovery& operator=(
      const DualCsrVerifiedModeRecovery&) = delete;
  DualCsrVerifiedModeRecovery(DualCsrVerifiedModeRecovery&&) noexcept
      = default;
  DualCsrVerifiedModeRecovery& operator=(
      DualCsrVerifiedModeRecovery&&) noexcept = default;

  [[nodiscard]] const CommittedDualCsrAnalysisState& state() const noexcept
  {
    return _state;
  }
  [[nodiscard]] const DualCsrModeRecoveryReplayEvidence& evidence()
      const noexcept
  {
    return _evidence;
  }

 private:
  DualCsrVerifiedModeRecovery(const CommittedDualCsrAnalysisState& expected_poisoned, CommittedDualCsrAnalysisState state,
                              DualCsrModeRecoveryReplayEvidence evidence)
      : _expected_poisoned(expected_poisoned), _state(std::move(state)), _evidence(evidence)
  {
  }

  DualCsrCommitBinding _expected_poisoned;
  CommittedDualCsrAnalysisState _state;
  DualCsrModeRecoveryReplayEvidence _evidence;

  friend DualCsrModeRecoveryReplayAttempt
  replayDualCsrModesForRecovery(
      const CommittedDualCsrAnalysisState&,
      CommittedDualCsrAnalysisState,
      DualCsrModeRecoveryBatchSource&);
  friend class DualCsrTransactionCoordinator;
};

struct DualCsrModeRecoveryReplayAttempt
{
  std::optional<DualCsrVerifiedModeRecovery> recovery;
  std::optional<DualCsrModeRecoveryReplayFailure> failure;
  DualCsrModeRecoveryReplayEvidence evidence;

  [[nodiscard]] bool success() const noexcept
  {
    return recovery.has_value() && !failure.has_value();
  }
};

// Replays exclusively through the public Stage 5 full or Stage 6 mixed
// factories selected by each persisted execution record. The poisoned value
// is typed-rebound only as an expected comparison value. The returned state is
// produced solely by generation-zero baseline + rebuilt capabilities.
[[nodiscard]] DualCsrModeRecoveryReplayAttempt
replayDualCsrModesForRecovery(
    const CommittedDualCsrAnalysisState& poisoned_committed,
    CommittedDualCsrAnalysisState generation_zero,
    DualCsrModeRecoveryBatchSource& source);

}  // namespace ista::timing_dual_csr
