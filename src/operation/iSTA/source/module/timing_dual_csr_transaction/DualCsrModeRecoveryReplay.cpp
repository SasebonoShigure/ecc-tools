// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrModeRecoveryReplay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
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
  if constexpr (incregpusta_debug_checks_enabled)
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

// Capacity retained after an abandoned transaction and allocation growth are
// resource observations, not replayable logical state.
bool sameRecoveredState(const CommittedDualCsrAnalysisState& lhs, const CommittedDualCsrAnalysisState& rhs)
{
  const auto graph = [](const DualCsrState& value) {
    return std::tie(value.session_token, value.generation, value.high_water, value.nodes, value.arc_slots,
                    value.base_out.row_offsets, value.base_out.physical_slots,
                    value.base_in.row_offsets, value.base_in.physical_slots,
                    value.aux_out.row_arcs, value.aux_out.row_sources, value.aux_out.source_to_aux_row,
                    value.reference_payload);
  };
  const auto execution = [](const DualCsrTelemetry& value) {
    return std::tie(value.generation_before, value.generation_after, value.logical_edges_before, value.logical_edges_after,
                    value.base_physical_edges, value.base_active_edges, value.base_tombstones, value.aux_rows, value.aux_edges,
                    value.tombstone_reuse_count, value.base_change_count, value.reverse_change_count, value.nodes_appended,
                    value.arcs_appended, value.payload_change_count, value.aux_rebuild_count, value.full_rebuild_count,
                    value.fallback_count, value.traversal);
  };
  const auto state = [](const CommittedDualCsrAnalysisState& value) {
    return std::tie(value.timing, value.resolver, value.arc_group_index, value.identity, value.generation,
                    value.descriptor_trace, value.semantic_trace, value.traversal_telemetry, value.execution_trace);
  };
  return graph(lhs.graph) == graph(rhs.graph) && execution(lhs.graph.telemetry) == execution(rhs.graph.telemetry)
         && state(lhs) == state(rhs) && bool(lhs.baseline) == bool(rhs.baseline)
         && (!lhs.baseline || *lhs.baseline == *rhs.baseline);
}

bool stageMatchesPersistedRecord(const DualCsrVerifiedStage& staged, const CommittedDualCsrAnalysisState& expected, std::size_t index)
{
  const auto& batch = staged.batch();
  return index < expected.semantic_trace.size() && index < expected.execution_trace.size() && index < expected.descriptor_trace.size()
         && batch.semantic == expected.semantic_trace[index] && batch.execution == expected.execution_trace[index]
         && batch.descriptor && *batch.descriptor == expected.descriptor_trace[index];
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
  if (!baseline_errors.empty() || !generation_zero.baseline || *generation_zero.baseline != *poisoned_committed.baseline
      || generation_zero.generation.value() != 0
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
  const std::size_t count = poisoned_committed.semantic_trace.size();
  for (std::size_t index = 0; index < count; ++index) {
    const auto replay_generation_before = coordinator.committed().generation.value();
    const auto& persisted_execution = poisoned_committed.execution_trace[index];
    std::optional<DualCsrBatchDescriptor> descriptor;
    if constexpr (incregpusta_debug_checks_enabled) {
      descriptor = poisoned_committed.descriptor_trace[index];
      descriptor->session_token = new_token;
      if (auto& identity = descriptor->reference_payload.source_identity; identity) {
        identity->expected.session_token = new_token.value();
        identity->replacement.session_token = new_token.value();
      }
    }
    DualCsrModeRecoveryBatchRequest request{.batch_index = index,
                                            .coordinator = coordinator,
                                            .diagnostic_descriptor = descriptor ? &*descriptor : nullptr,
                                            .persisted_semantic_batch = poisoned_committed.semantic_trace[index],
                                            .persisted_execution = persisted_execution};
    auto batch_attempt = source.rebuild(request);
    if (!batch_attempt.success())
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kBatchSourceRejected,
          replay_generation_before,
          batch_attempt.detail.empty()
              ? "recovery batch source did not mint a unique capability"
              : std::move(batch_attempt.detail),
          std::move(batch_attempt.violations));

    std::optional<DualCsrVerifiedStage> staged;
    if (persisted_execution.mode()
        == LevelizationExecutionMode::kFullStage5) {
      auto full_attempt = makeStagedDualCsrAnalysisState(
          coordinator,
          std::move(*batch_attempt.verified_batch));
      if (!full_attempt.success()) {
        DualCsrStageFailure failure{
            full_attempt.failure_origin.value_or(
                DualCsrStageFailureOrigin::kStateValidation),
            full_attempt.violations};
        return reject(
            evidence,
            DualCsrModeRecoveryReplayCode::kFullStageRejected,
            replay_generation_before,
            "persisted full mode failed its real Stage 5 factory",
            full_attempt.violations, std::move(failure));
      }
      staged.emplace(std::move(*full_attempt.staged));
      if constexpr (incregpusta_debug_checks_enabled)
        ++evidence.full_records_replayed;
    } else if (persisted_execution.mode()
               == LevelizationExecutionMode::kMixedStage6) {
      auto mixed_attempt = makeMixedLevelizedDualCsrAnalysisState(
          coordinator,
          std::move(*batch_attempt.verified_batch));
      if (auto* dual = std::get_if<DualCsrStageFailure>(
              &mixed_attempt))
        return reject(
            evidence,
            DualCsrModeRecoveryReplayCode::kMixedStageRejected,
            replay_generation_before,
            "persisted mixed mode failed at the Dual-CSR boundary",
            dual->violations, std::move(*dual));
      if (auto* mixed = std::get_if<
              timing_incremental_levelization::MixedLevelizationFailure>(
              &mixed_attempt))
        return reject(
            evidence,
            DualCsrModeRecoveryReplayCode::kMixedStageRejected,
            replay_generation_before,
            "persisted mixed mode failed its real Stage 6 factory", {},
            std::nullopt, std::move(*mixed));
      staged.emplace(std::move(
          std::get<DualCsrVerifiedStage>(mixed_attempt)));
      if constexpr (incregpusta_debug_checks_enabled)
        ++evidence.mixed_records_replayed;
    } else {
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kModeRecordMismatch,
          replay_generation_before,
          "persisted execution mode is unknown");
    }

    if (incregpusta_debug_checks_enabled && !stageMatchesPersistedRecord(*staged, *expected, index))
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kModeRecordMismatch,
          replay_generation_before,
          "real replay did not reproduce the persisted mode-specific record");
    const auto status = coordinator.publish(std::move(*staged));
    if (!status.ok() || !status.published || status.poisoned)
      return reject(
          evidence,
          DualCsrModeRecoveryReplayCode::kPublishRejected,
          replay_generation_before,
          "recovery replay publish rejected a verified stage");
    if constexpr (incregpusta_debug_checks_enabled)
      ++evidence.descriptors_replayed;
  }

  if constexpr (incregpusta_debug_checks_enabled) {
    evidence.generation_recovered = coordinator.committed().generation.value();
    evidence.recovery_replay_count = 1;
  }
  if (coordinator.committed().generation != poisoned_committed.generation || (incregpusta_debug_checks_enabled && !sameRecoveredState(coordinator.committed(), *expected)))
    return reject(
        evidence,
        DualCsrModeRecoveryReplayCode::kDeterministicMismatch,
        coordinator.committed().generation.value(),
        "mode-dispatched replay differs from the typed-rebound deterministic aggregate");

  DualCsrModeRecoveryReplayAttempt attempt;
  if constexpr (incregpusta_debug_checks_enabled)
    attempt.evidence = evidence;
  attempt.recovery.emplace(DualCsrVerifiedModeRecovery(poisoned_committed, std::move(*coordinator._committed)));
  return attempt;
}

DualCsrTransactionCoordinator::DualCsrTransactionCoordinator(DualCsrVerifiedModeRecovery recovered)
    : _committed(std::make_shared<CommittedDualCsrAnalysisState>(std::move(recovered._state)))
{
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::recover(
    DualCsrVerifiedModeRecovery verified)
{
  if (!_pending.expired())
    return reject(DualCsrTransactionCode::kPending);
  if (!_poisoned)
    return reject(DualCsrTransactionCode::kRecoveryNotPoisoned);
  if (!verified._expected_poisoned.matches(*_committed))
    return reject(DualCsrTransactionCode::kTraceMismatch);
  if (incregpusta_debug_checks_enabled && !validateCommittedDualCsrAnalysisState(verified._state).empty())
    return reject(DualCsrTransactionCode::kInvalidStagedState);
  if (verified._state.generation != _committed->generation || verified._state.graph.session_token.value() == 0
      || verified._state.graph.session_token == _committed->graph.session_token)
    return reject(DualCsrTransactionCode::kTraceMismatch);

  if constexpr (incregpusta_debug_checks_enabled) {
    auto rebound = rebindRecoveredAggregateSession(*_committed, _committed->graph.session_token, verified._state.graph.session_token);
    if (!rebound.success() || !sameRecoveredState(*rebound.rebound, verified._state))
      return reject(DualCsrTransactionCode::kTraceMismatch);
  }
  _committed->swap(verified._state);
  _poisoned = false;
  return {DualCsrTransactionCode::kOk, true, false};
}

}  // namespace ista::timing_dual_csr
