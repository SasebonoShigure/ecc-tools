// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../../data_manager/adapter/csr/DualCsrSemanticIdentity.hpp"
#include "../../data_manager/adapter/csr/TimingEditDualCsrDescriptorBuilder.hpp"
#include "CpuIncrementalLevelizationPipeline.hpp"
#include "CsrCpuReferenceBackendInternal.hpp"
#include "DualCsrArcGroupResolverValidation.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrStateValidation.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrTransactionCoordinator.hpp"
#include "DualCsrUpdater.hpp"
#include "NarrowCaptureAuditValidation.hpp"

namespace ista::timing_dual_csr {

class DualCsrStageFactoryAccess final
{
 public:
  struct ConsumedBatch
  {
    timing_edit::PreparedBatch prepared_batch;
    timing_edit::TimingEditResolverState staged_resolver;
    DualCsrBatchDescriptor descriptor;
    ExpectedBeforeBinding expected_before;
    NarrowCaptureAudit capture_audit;
  };

  [[nodiscard]] static ConsumedBatch consume(
      VerifiedStructuredBatch&& batch)
  {
    return {std::move(batch._prepared_batch),
            std::move(batch._staged_resolver),
            std::move(batch._descriptor),
            std::move(batch._expected_before),
            std::move(batch._capture_audit)};
  }

  [[nodiscard]] static DualCsrVerifiedStage makeVerified(
      const CommittedDualCsrAnalysisState& expected,
      CommittedDualCsrAnalysisState staged,
      DualCsrStageTimings timings)
  {
    return DualCsrVerifiedStage(expected, std::move(staged), timings);
  }

  [[nodiscard]] static LevelizationExecutionTrace makeExecutionTrace(timing_edit::TransactionGeneration generation, LevelizationExecutionMode mode,
                                                                     const csr::CsrFifoLevelization& levels, csr::CsrTraversalTelemetry traversal,
                                                                     csr::CsrFullLevelizerEntryDelta&& entry_delta, std::uint64_t fallback_count,
                                                                     bool supplied_levels_validated, std::optional<MixedLevelizationDeterministicTrace> mixed)
  {
    return LevelizationExecutionTrace(generation, mode, levels, traversal, std::move(entry_delta), fallback_count, supplied_levels_validated, std::move(mixed));
  }
};

namespace {

using SteadyClock = std::chrono::steady_clock;
using timing_incremental_levelization::MixedFailureOrigin;
using timing_incremental_levelization::MixedLevelizationFailure;
using timing_incremental_levelization::MixedLevelizationRequest;
using timing_incremental_levelization::MixedLevelizationSuccess;
using timing_incremental_levelization::MixedStageFailureCode;

double elapsedMilliseconds(SteadyClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(
             SteadyClock::now() - start)
      .count();
}

DualCsrStageFailure makeDualFailure(
    DualCsrStageFailureOrigin origin, DualCsrViolations violations)
{
  return {origin, std::move(violations)};
}

DualCsrStageFailure makeDualFailure(
    DualCsrStageFailureOrigin origin, DualCsrViolationCode code,
    std::string field, std::string message)
{
  DualCsrViolations violations;
  violations.push_back(
      {code, std::move(field), std::move(message)});
  return makeDualFailure(origin, std::move(violations));
}

MixedLevelizationFailure makeMixedFailure(
    MixedStageFailureCode code, MixedFailureOrigin origin,
    std::optional<csr::CsrViewError> view_error,
    std::uint64_t generation, std::string detail)
{
  return {code, origin, view_error, generation, std::move(detail)};
}

bool baselineMatches(const CommittedDualCsrAnalysisState& committed,
                     const timing_edit::PreparedBatch& batch)
{
  if (!committed.semantic_trace.empty())
    return batch.baseline() == committed.semantic_trace.front().baseline;
  return committed.generation.value() == 0
         && committed.identity.generation
                == timing_edit::TransactionGeneration{0}
         && committed.identity.current_fingerprints
                == batch.baseline().fingerprints
         && committed.identity.canonical_pre_state
                == batch.baseline().canonical_pre_state;
}

timing_edit::DesignEditPlan semanticPlan(
    const timing_edit::PreparedBatch& batch)
{
  return {.schema_version = timing_edit::kDesignEditPlanSchema,
          .baseline = batch.baseline(),
          .expected_generation = batch.expectedGeneration(),
          .edits = batch.normalizedEdits()};
}

std::vector<std::uint8_t> activeArcMask(const DualCsrState& graph)
{
  std::vector<std::uint8_t> mask;
  mask.reserve(graph.arc_slots.size());
  for (const auto& slot : graph.arc_slots)
    mask.push_back(slot.active ? 1U : 0U);
  return mask;
}

csr::CsrFifoLevelization levelizationFromTiming(
    const csr::CsrTimingResult& timing)
{
  return {true, timing.level_offsets, timing.level_nodes,
          timing.node_to_level};
}

std::optional<std::uint64_t> counterDelta(
    std::uint64_t before, std::uint64_t after) noexcept
{
  if (after < before)
    return std::nullopt;
  return after - before;
}

struct PreparedStage
{
  timing_edit::PreparedBatch prepared_batch;
  timing_edit::TimingEditResolverState staged_resolver;
  DualCsrBatchDescriptor descriptor;
  DualCsrState graph;
  DualCsrArcGroupIndex arc_group_index;
  timing_edit::DesignEditPlan semantic_batch;
  DualCsrStageTimings timings;
};

using PreparedStageAttempt
    = std::variant<PreparedStage, DualCsrStageFailure>;

PreparedStageAttempt prepareStage(
    const CommittedDualCsrAnalysisState& committed,
    DualCsrStageFactoryAccess::ConsumedBatch batch)
{
  DualCsrViolations violations;
  if (incregpusta_debug_checks_enabled
      && (batch.expected_before.graph() != committed.graph || batch.expected_before.resolver() != committed.resolver
          || batch.expected_before.arcGroupIndex() != committed.arc_group_index || batch.expected_before.identity() != committed.identity))
    return makeDualFailure(
        DualCsrStageFailureOrigin::kExactBefore,
        DualCsrViolationCode::kExpectedValue,
        "stage.exact_before",
        "verified capability is not bound to the exact committed aggregate");

  const std::uint64_t expected = committed.generation.value();
  if (expected == std::numeric_limits<std::uint64_t>::max() || batch.prepared_batch.sealed() || batch.prepared_batch.targetPostCommittedIdentity()
      || batch.prepared_batch.sessionToken() != committed.graph.session_token || batch.prepared_batch.expectedGeneration() != committed.generation
      || batch.prepared_batch.targetGeneration() != timing_edit::TransactionGeneration{expected + 1}
      || batch.prepared_batch.expectedPreCommittedIdentity() != committed.identity
      || batch.prepared_batch.policyReceipt().disposition != timing_edit::EditDisposition::kSupported || !baselineMatches(committed, batch.prepared_batch)
      || batch.descriptor.session_token != batch.prepared_batch.sessionToken()
      || batch.descriptor.expected_generation != batch.prepared_batch.expectedGeneration()
      || batch.descriptor.target_generation != batch.prepared_batch.targetGeneration() || batch.descriptor.before != batch.prepared_batch.slotPlan().before
      || batch.descriptor.after != batch.prepared_batch.slotPlan().after || batch.staged_resolver.highWater() != batch.descriptor.after
      || (incregpusta_debug_checks_enabled
          && !validateNarrowCaptureAudit(batch.capture_audit,
                                         NarrowCaptureAuditExpectation{batch.descriptor.after, batch.descriptor.logical_deactivations.size()})))
    return makeDualFailure(
        DualCsrStageFailureOrigin::kCapability,
        DualCsrViolationCode::kGeneration, "stage.capability",
        "verified batch and exact committed state do not form the next generation");

  try {
    if (incregpusta_debug_checks_enabled && committed.resolver.stage(batch.prepared_batch) != batch.staged_resolver)
      return makeDualFailure(
          DualCsrStageFailureOrigin::kCapability,
          DualCsrViolationCode::kConflict, "stage.resolver",
          "verified staged resolver is not the exact batch transition");
  } catch (const std::invalid_argument& error) {
    return makeDualFailure(DualCsrStageFailureOrigin::kCapability,
                           DualCsrViolationCode::kConflict,
                           "stage.resolver", error.what());
  }

  const auto update_started = SteadyClock::now();
  DualCsrIndexedUpdateAttempt graph_attempt = DualCsrUpdater::stage(
      committed.graph, committed.arc_group_index, batch.descriptor);
  const double update_ms = elapsedMilliseconds(update_started);
  if (!graph_attempt.success())
    return makeDualFailure(DualCsrStageFailureOrigin::kUpdater,
                           std::move(graph_attempt.violations));

  DualCsrState graph = std::move(*graph_attempt.state);
  DualCsrArcGroupIndex arc_group_index
      = std::move(*graph_attempt.arc_group_index);
  double invariant_ms = 0.0;
  if constexpr (incregpusta_debug_checks_enabled) {
    const auto invariant_started = SteadyClock::now();
    violations = validateDualCsrArcGroupResolverTransition(committed.arc_group_index, arc_group_index, batch.descriptor, batch.staged_resolver);
    invariant_ms = elapsedMilliseconds(invariant_started);
  }
  if (!violations.empty())
    return makeDualFailure(DualCsrStageFailureOrigin::kStateValidation,
                           std::move(violations));

  timing_edit::DesignEditPlan semantic_batch
      = semanticPlan(batch.prepared_batch);
  return PreparedStage{
      .prepared_batch = std::move(batch.prepared_batch),
      .staged_resolver = std::move(batch.staged_resolver),
      .descriptor = std::move(batch.descriptor),
      .graph = std::move(graph),
      .arc_group_index = std::move(arc_group_index),
      .semantic_batch = std::move(semantic_batch),
      .timings = {.dual_csr_update_ms = update_ms,
                  .invariant_ms = invariant_ms}};
}

using BuiltStateAttempt
    = std::variant<CommittedDualCsrAnalysisState, DualCsrStageFailure>;

BuiltStateAttempt buildStagedState(
    const CommittedDualCsrAnalysisState& committed,
    PreparedStage stage, DualCsrTimingResult timing,
    LevelizationExecutionTrace execution)
{
  DualCsrViolations violations;
  timing_edit::CommittedEditIdentity identity
      = deriveDualCsrCommittedIdentity(stage.graph,
                                       stage.staged_resolver);
  CommittedDualCsrAnalysisState staged{
      .graph = std::move(stage.graph),
      .timing = std::move(timing),
      .resolver = std::move(stage.staged_resolver),
      .arc_group_index = std::move(stage.arc_group_index),
      .identity = std::move(identity),
      .generation = stage.descriptor.target_generation,
      .descriptor_trace = committed.descriptor_trace,
      .semantic_trace = committed.semantic_trace,
      .traversal_telemetry = execution.traversal(),
      .execution_trace = committed.execution_trace};
  staged.descriptor_trace.push_back(std::move(stage.descriptor));
  staged.semantic_trace.push_back(std::move(stage.semantic_batch));
  staged.execution_trace.push_back(std::move(execution));
  if constexpr (incregpusta_debug_checks_enabled)
    violations = validateCommittedDualCsrAnalysisState(staged);
  if (!violations.empty())
    return makeDualFailure(DualCsrStageFailureOrigin::kStateValidation,
                           std::move(violations));
  return staged;
}

void assignFailure(DualCsrStageAttempt& attempt,
                   DualCsrStageFailure failure)
{
  attempt.failure_origin = failure.origin;
  attempt.violations = std::move(failure.violations);
}

MixedLevelizationFailure forbiddenFullEntry(
    std::uint64_t generation, std::uint64_t delta)
{
  return makeMixedFailure(
      MixedStageFailureCode::kInvariantFailure,
      MixedFailureOrigin::kPropagation, std::nullopt, generation,
      "mixed operation entered the full levelizer "
          + std::to_string(delta) + " time(s)");
}

}  // namespace

DualCsrStageAttempt makeStagedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& committed,
    VerifiedStructuredBatch&& verified_batch)
{
  DualCsrStageAttempt attempt;
  auto prepared_attempt = prepareStage(
      committed,
      DualCsrStageFactoryAccess::consume(std::move(verified_batch)));
  if (auto* failure
      = std::get_if<DualCsrStageFailure>(&prepared_attempt)) {
    assignFailure(attempt, std::move(*failure));
    return attempt;
  }
  PreparedStage stage
      = std::move(std::get<PreparedStage>(prepared_attempt));
  attempt.timings = stage.timings;

  DualCsrEdgeViewImpl edges(stage.graph);
  DualCsrTimingPayloadViewImpl payloads(stage.graph);
  auto guard = csr::beginCsrFullLevelizerOperation();
  auto computation = csr::detail::computeCsrCpuReferenceForStage(edges, payloads);
  auto entry_delta = std::move(guard).finish();
  attempt.timings.full_levelization_ms
      = computation.phase_timings.levelization_ms;
  attempt.timings.full_propagation_ms
      = computation.phase_timings.propagation_ms;
  auto* values
      = std::get_if<csr::CsrTimingResult>(&computation.computation.outcome);
  if (values == nullptr || (incregpusta_debug_checks_enabled && entry_delta.value() != 1)) {
    assignFailure(
        attempt,
        makeDualFailure(
            DualCsrStageFailureOrigin::kTimingIntrinsic,
            DualCsrViolationCode::kTimingResult, "stage.full_cpu",
            "direct-view full CPU computation did not produce timing with exactly one full-levelizer entry"));
    return attempt;
  }

  csr::CsrFifoLevelization levels;
  if constexpr (incregpusta_debug_checks_enabled)
    levels = levelizationFromTiming(*values);
  const auto fallback_delta = counterDelta(
      committed.graph.telemetry.fallback_count,
      stage.graph.telemetry.fallback_count);
  if (!fallback_delta) {
    assignFailure(
        attempt,
        makeDualFailure(DualCsrStageFailureOrigin::kStateValidation,
                        DualCsrViolationCode::kTimingResult,
                        "stage.fallback_count",
                        "fallback counter regressed"));
    return attempt;
  }

  stage.graph.telemetry.traversal = computation.telemetry;
  LevelizationExecutionTrace execution
      = DualCsrStageFactoryAccess::makeExecutionTrace(
          stage.descriptor.target_generation,
          LevelizationExecutionMode::kFullStage5, std::move(levels),
          computation.telemetry, std::move(entry_delta),
          *fallback_delta, false, std::nullopt);
  DualCsrTimingResult timing{std::move(*values),
                             activeArcMask(stage.graph)};
  const auto verify_started = SteadyClock::now();
  auto built = buildStagedState(committed, std::move(stage),
                                std::move(timing),
                                std::move(execution));
  attempt.timings.verify_ms = elapsedMilliseconds(verify_started);
  if (auto* failure = std::get_if<DualCsrStageFailure>(&built)) {
    assignFailure(attempt, std::move(*failure));
    return attempt;
  }
  attempt.staged.emplace(DualCsrStageFactoryAccess::makeVerified(
      committed,
      std::move(std::get<CommittedDualCsrAnalysisState>(built)),
      attempt.timings));
  return attempt;
}

MixedDualCsrStageAttempt makeMixedLevelizedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& exact_committed_before,
    VerifiedStructuredBatch&& verified_batch)
{
  auto prepared_attempt = prepareStage(
      exact_committed_before,
      DualCsrStageFactoryAccess::consume(std::move(verified_batch)));
  if (auto* failure
      = std::get_if<DualCsrStageFailure>(&prepared_attempt))
    return std::move(*failure);
  PreparedStage stage
      = std::move(std::get<PreparedStage>(prepared_attempt));

  DualCsrEdgeViewImpl edges(stage.graph);
  DualCsrTimingPayloadViewImpl payloads(stage.graph);
  const csr::CsrFifoLevelization previous
      = levelizationFromTiming(exact_committed_before.timing.values);
  const std::uint64_t generation = stage.descriptor.target_generation.value();
  auto guard = csr::beginCsrFullLevelizerOperation();
  auto mixed_attempt
      = timing_incremental_levelization::runCpuIncrementalLevelization(
          edges, previous,
          MixedLevelizationRequest{
              .n_last_nodes = static_cast<std::uint64_t>(
                  exact_committed_before.graph.nodes.size()),
              .generation_before
                  = exact_committed_before.generation.value(),
              .generation_after = generation});
  if (auto* failure
      = std::get_if<MixedLevelizationFailure>(&mixed_attempt)) {
    auto entry_delta = std::move(guard).finish();
    if (entry_delta.value() != 0)
      return forbiddenFullEntry(generation, entry_delta.value());
    return std::move(*failure);
  }
  MixedLevelizationSuccess mixed
      = std::move(std::get<MixedLevelizationSuccess>(mixed_attempt));
  stage.timings.mixed_build_ms = mixed.telemetry.mixed_build_ms;
  stage.timings.mixed_kahn_ms = mixed.telemetry.mixed_kahn_ms;
  stage.timings.reconstruct_ms = mixed.telemetry.reconstruct_ms;
  auto computation = csr::detail::propagateCsrCpuReferenceWithLevels(edges, payloads, mixed.levels);
  auto entry_delta = std::move(guard).finish();
  if (entry_delta.value() != 0)
    return forbiddenFullEntry(generation, entry_delta.value());
  stage.timings.full_propagation_ms
      = computation.phase_timings.propagation_ms;
  auto* values
      = std::get_if<csr::CsrTimingResult>(&computation.computation.outcome);
  if (values == nullptr)
    return makeDualFailure(
        DualCsrStageFailureOrigin::kTimingIntrinsic,
        DualCsrViolationCode::kTimingResult,
        "stage.mixed_cpu",
        "external-level full CPU propagation did not produce timing");

  const auto fallback_delta = counterDelta(
      exact_committed_before.graph.telemetry.fallback_count,
      stage.graph.telemetry.fallback_count);
  if (!fallback_delta
      || *fallback_delta != mixed.telemetry.fallback_count)
    return makeMixedFailure(
        MixedStageFailureCode::kInvariantFailure,
        MixedFailureOrigin::kPropagation, std::nullopt, generation,
        "mixed fallback telemetry differs from the real graph counter delta");

  stage.graph.telemetry.traversal = computation.telemetry;
  LevelizationExecutionTrace execution
      = DualCsrStageFactoryAccess::makeExecutionTrace(
          stage.descriptor.target_generation,
          LevelizationExecutionMode::kMixedStage6, mixed.levels,
          computation.telemetry, std::move(entry_delta),
          *fallback_delta, true,
          projectDeterministicMixedTrace(mixed.telemetry));
  DualCsrTimingResult timing{std::move(*values),
                             activeArcMask(stage.graph)};
  const auto verify_started = SteadyClock::now();
  DualCsrStageTimings timings = stage.timings;
  auto built = buildStagedState(exact_committed_before,
                                std::move(stage), std::move(timing),
                                std::move(execution));
  timings.verify_ms = elapsedMilliseconds(verify_started);
  if (auto* failure = std::get_if<DualCsrStageFailure>(&built))
    return std::move(*failure);
  return DualCsrStageFactoryAccess::makeVerified(
      exact_committed_before,
      std::move(std::get<CommittedDualCsrAnalysisState>(built)),
      timings);
}

}  // namespace ista::timing_dual_csr
