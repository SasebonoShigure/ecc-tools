// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrModeRecoveryReplay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace ista::timing_dual_csr {
namespace {

DualCsrModeRecoveryReplayAttempt reject(
    DualCsrModeRecoveryReplayEvidence evidence,
    DualCsrModeRecoveryReplayCode code, std::uint64_t generation,
    std::string detail, DualCsrViolations violations = {},
    std::optional<DualCsrStageFailure> dual_failure = std::nullopt,
    std::optional<
        timing_incremental_levelization::MixedLevelizationFailure>
        mixed_failure = std::nullopt)
{
  DualCsrModeRecoveryReplayAttempt attempt;
  attempt.evidence = evidence;
  attempt.failure = DualCsrModeRecoveryReplayFailure{
      .code = code,
      .generation = generation,
      .violations = std::move(violations),
      .dual_stage_failure = std::move(dual_failure),
      .mixed_failure = std::move(mixed_failure),
      .detail = std::move(detail)};
  return attempt;
}

bool checkedIncrement(std::uint64_t& value) noexcept
{
  if (value == std::numeric_limits<std::uint64_t>::max())
    return false;
  ++value;
  return true;
}

template <typename T>
bool exactPrefix(const std::vector<T>& produced,
                 const std::vector<T>& expected,
                 std::size_t length)
{
  return produced.size() == length && expected.size() >= length
         && std::equal(produced.begin(), produced.end(),
                       expected.begin());
}

bool stageMatchesPersistedPrefix(
    const CommittedDualCsrAnalysisState& staged,
    const CommittedDualCsrAnalysisState& expected,
    std::size_t length)
{
  if (!exactPrefix(staged.descriptor_trace,
                   expected.descriptor_trace, length)
      || !exactPrefix(staged.semantic_trace,
                      expected.semantic_trace, length)
      || !exactPrefix(staged.execution_trace,
                      expected.execution_trace, length))
    return false;
  const auto& record = staged.execution_trace.back();
  return validateLevelizationExecutionTraceShape(record)
         && record.generation()
                == timing_edit::TransactionGeneration{length};
}

bool validReplayEvidence(
    const DualCsrModeRecoveryReplayEvidence& evidence) noexcept
{
  std::uint64_t mode_records = 0;
  return evidence.recovery_replay_count == 1
         && evidence.generation_zero == 0
         && evidence.descriptors_replayed
                == evidence.generation_recovered
         && evidence.full_records_replayed
                <= std::numeric_limits<std::uint64_t>::max()
                   - evidence.mixed_records_replayed
         && (mode_records = evidence.full_records_replayed
                            + evidence.mixed_records_replayed,
             mode_records == evidence.descriptors_replayed);
}

}  // namespace

DualCsrModeRecoveryReplayAttempt replayDualCsrModesForRecovery(
    const CommittedDualCsrAnalysisState& poisoned_committed,
    CommittedDualCsrAnalysisState generation_zero,
    DualCsrModeRecoveryBatchSource& source)
{
  DualCsrModeRecoveryReplayEvidence evidence;
  const DualCsrViolations poisoned_errors
      = validateCommittedDualCsrAnalysisState(poisoned_committed);
  if (!poisoned_errors.empty()
      || poisoned_committed.generation.value() == 0)
    return reject(evidence,
                  DualCsrModeRecoveryReplayCode::kInvalidPoisonedState,
                  poisoned_committed.generation.value(),
                  "poisoned aggregate is not a valid nonzero generation",
                  poisoned_errors);

  const DualCsrViolations baseline_errors
      = validateCommittedDualCsrAnalysisState(generation_zero);
  if (!baseline_errors.empty() || generation_zero.generation.value() != 0
      || !generation_zero.descriptor_trace.empty()
      || !generation_zero.semantic_trace.empty()
      || !generation_zero.execution_trace.empty())
    return reject(evidence,
                  DualCsrModeRecoveryReplayCode::kInvalidGenerationZero,
                  generation_zero.generation.value(),
                  "recovery must start from a valid trace-empty generation zero",
                  baseline_errors);

  const auto old_token = poisoned_committed.graph.session_token;
  const auto new_token = generation_zero.graph.session_token;
  if (old_token == new_token || new_token.value() == 0)
    return reject(evidence,
                  DualCsrModeRecoveryReplayCode::kSessionTokenDidNotChange,
                  0, "recovery session token must change and remain nonzero");

  // An independently rebound aggregate is only an equivalence oracle. The
  // actual recovery below always rebuilds each batch through its recorded mode.
  std::optional<CommittedDualCsrAnalysisState> expected;
  if constexpr (incregpusta_debug_checks_enabled) {
    auto expected_attempt = rebindRecoveredAggregateSession(poisoned_committed, old_token, new_token);
    if (!expected_attempt.success())
      return reject(evidence, DualCsrModeRecoveryReplayCode::kRecoveredAggregateRebindRejected, 0, "complete typed recovery rebind was rejected",
                    std::move(expected_attempt.violations));
    expected = std::move(expected_attempt.rebound);
  }

  DualCsrTransactionCoordinator coordinator(std::move(generation_zero));
  const std::size_t count = poisoned_committed.descriptor_trace.size();
  for (std::size_t index = 0; index < count; ++index) {
    const auto& persisted_execution = poisoned_committed.execution_trace[index];
    // Only the current descriptor needs a new session binding for the source.
    auto descriptor = poisoned_committed.descriptor_trace[index];
    descriptor.session_token = new_token;
    if (auto& identity = descriptor.reference_payload.source_identity; identity) {
      identity->expected.session_token = new_token.value();
      identity->replacement.session_token = new_token.value();
    }
    DualCsrModeRecoveryBatchRequest request{.batch_index = index,
                                            .exact_recovered_before = coordinator.committed(),
                                            .persisted_descriptor = descriptor,
                                            .persisted_semantic_batch = poisoned_committed.semantic_trace[index],
                                            .persisted_execution = persisted_execution};
    auto batch_attempt = source.rebuild(request);
    if (!batch_attempt.success())
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kBatchSourceRejected,
          coordinator.committed().generation.value(),
          batch_attempt.detail.empty()
              ? "recovery batch source did not mint a unique capability"
              : std::move(batch_attempt.detail),
          std::move(batch_attempt.violations));

    std::optional<DualCsrVerifiedStage> staged;
    if (persisted_execution.mode()
        == LevelizationExecutionMode::kFullStage5) {
      auto full_attempt = makeStagedDualCsrAnalysisState(
          coordinator.committed(),
          std::move(*batch_attempt.verified_batch));
      if (!full_attempt.success()) {
        DualCsrStageFailure failure{
            full_attempt.failure_origin.value_or(
                DualCsrStageFailureOrigin::kStateValidation),
            full_attempt.violations};
        return reject(
            evidence,
            DualCsrModeRecoveryReplayCode::kFullStageRejected,
            coordinator.committed().generation.value(),
            "persisted full mode failed its real Stage 5 factory",
            full_attempt.violations, std::move(failure));
      }
      staged.emplace(std::move(*full_attempt.staged));
      if (!checkedIncrement(evidence.full_records_replayed))
        return reject(
            evidence,
            DualCsrModeRecoveryReplayCode::kModeRecordMismatch,
            coordinator.committed().generation.value(),
            "full replay record counter overflowed");
    } else if (persisted_execution.mode()
               == LevelizationExecutionMode::kMixedStage6) {
      auto mixed_attempt = makeMixedLevelizedDualCsrAnalysisState(
          coordinator.committed(),
          std::move(*batch_attempt.verified_batch));
      if (auto* dual = std::get_if<DualCsrStageFailure>(
              &mixed_attempt))
        return reject(
            evidence,
            DualCsrModeRecoveryReplayCode::kMixedStageRejected,
            coordinator.committed().generation.value(),
            "persisted mixed mode failed at the Dual-CSR boundary",
            dual->violations, std::move(*dual));
      if (auto* mixed = std::get_if<
              timing_incremental_levelization::MixedLevelizationFailure>(
              &mixed_attempt))
        return reject(
            evidence,
            DualCsrModeRecoveryReplayCode::kMixedStageRejected,
            coordinator.committed().generation.value(),
            "persisted mixed mode failed its real Stage 6 factory", {},
            std::nullopt, std::move(*mixed));
      staged.emplace(std::move(
          std::get<DualCsrVerifiedStage>(mixed_attempt)));
      if (!checkedIncrement(evidence.mixed_records_replayed))
        return reject(
            evidence,
            DualCsrModeRecoveryReplayCode::kModeRecordMismatch,
            coordinator.committed().generation.value(),
            "mixed replay record counter overflowed");
    } else {
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kModeRecordMismatch,
          coordinator.committed().generation.value(),
          "persisted execution mode is unknown");
    }

    const std::size_t prefix = index + 1;
    if (incregpusta_debug_checks_enabled && !stageMatchesPersistedPrefix(staged->state(), *expected, prefix))
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kModeRecordMismatch,
          coordinator.committed().generation.value(),
          "real replay did not reproduce the persisted mode-specific record");
    const auto status = coordinator.publish(std::move(*staged));
    if (!status.ok() || !status.published || status.poisoned)
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kPublishRejected,
          coordinator.committed().generation.value(),
          "recovery replay publish rejected a verified stage");
    if (!checkedIncrement(evidence.descriptors_replayed))
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kModeRecordMismatch,
          coordinator.committed().generation.value(),
          "descriptor replay counter overflowed");
  }

  evidence.generation_recovered
      = coordinator.committed().generation.value();
  evidence.recovery_replay_count = 1;
  if (!validReplayEvidence(evidence) || (incregpusta_debug_checks_enabled && !(coordinator.committed() == *expected)))
    return reject(
        evidence,
        DualCsrModeRecoveryReplayCode::kDeterministicMismatch,
        coordinator.committed().generation.value(),
        "mode-dispatched replay differs from the typed-rebound deterministic aggregate");

  DualCsrModeRecoveryReplayAttempt attempt;
  attempt.evidence = evidence;
  attempt.recovery.emplace(DualCsrVerifiedModeRecovery(
      poisoned_committed, coordinator.committed(), evidence));
  return attempt;
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::recover(
    DualCsrVerifiedModeRecovery verified)
{
  if (!_poisoned)
    return reject(DualCsrTransactionCode::kRecoveryNotPoisoned);
  if (!(verified._expected_poisoned.matches(_committed)) || !validReplayEvidence(verified._evidence))
    return reject(DualCsrTransactionCode::kTraceMismatch);
  if (incregpusta_debug_checks_enabled && !validateCommittedDualCsrAnalysisState(verified._state).empty())
    return reject(DualCsrTransactionCode::kInvalidStagedState);
  if (verified._state.graph.session_token
      == _committed.graph.session_token)
    return reject(DualCsrTransactionCode::kTraceMismatch);

  if constexpr (incregpusta_debug_checks_enabled) {
    auto rebound = rebindRecoveredAggregateSession(_committed, _committed.graph.session_token, verified._state.graph.session_token);
    if (!rebound.success() || !(*rebound.rebound == verified._state))
      return reject(DualCsrTransactionCode::kTraceMismatch);
  }
  _committed.swap(verified._state);
  _poisoned = false;
  return {DualCsrTransactionCode::kOk, true, false};
}

}  // namespace ista::timing_dual_csr
