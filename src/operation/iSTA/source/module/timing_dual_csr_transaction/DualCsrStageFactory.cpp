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
#include "MixedLevelizationInternal.hpp"
#include "CsrCpuReferenceBackendInternal.hpp"
#include "DualCsrArcGroupResolverValidation.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrStateValidation.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrTransactionCoordinator.hpp"
#include "DualCsrDescriptorValidation.hpp"
#include "DualCsrUpdaterInternal.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "NarrowCaptureAuditValidation.hpp"

namespace ista::timing_dual_csr {

struct DualCsrVerifiedStage::Data
{
  // Keep the owner alive until the two undo records have been destroyed.
  std::shared_ptr<CommittedDualCsrAnalysisState> owner;
  DualCsrCommitBinding expected;
  timing_edit::AppendOnlyHighWater before_high_water;
  std::uint64_t fallback_before;
  DualCsrBatchDescriptor descriptor;
  timing_edit::EditBatch semantic_batch;
  DualCsrTimingResult timing;
  std::optional<timing_edit::CommittedEditIdentity> identity;
  std::optional<DualCsrPendingBatch> batch;
  DualCsrStageTimings timings;
  timing_edit::PreparedTimingEditResolverPatch resolver_patch;
  detail::PreparedDualCsrGraphPatch graph_patch;
  detail::PreparedDualCsrIndexPatch index_patch;

  Data(std::shared_ptr<CommittedDualCsrAnalysisState> state,
       const timing_edit::PreparedBatch& prepared,
       timing_edit::TimingEditResolverDelta resolver_delta,
       DualCsrBatchDescriptor delta)
      : owner(std::move(state)), expected(*owner), before_high_water(owner->graph.high_water),
        fallback_before(owner->graph.telemetry.fallback_count),
        descriptor(std::move(delta)),
        semantic_batch{prepared.expectedGeneration(), prepared.normalizedEdits()},
        resolver_patch(owner->resolver, std::move(resolver_delta)),
        graph_patch(owner->graph, descriptor), index_patch(owner->graph, owner->arc_group_index, descriptor)
  {
  }
};

const DualCsrPendingBatch& DualCsrVerifiedStage::batch() const noexcept { return *_data->batch; }
const DualCsrStageTimings& DualCsrVerifiedStage::timings() const noexcept { return _data->timings; }

class DualCsrStageFactoryAccess final
{
 public:
  struct ConsumedBatch
  {
    const DualCsrState* source_graph;
    timing_edit::PreparedBatch prepared_batch;
    timing_edit::TimingEditResolverDelta resolver_delta;
    DualCsrBatchDescriptor descriptor;
    NarrowCaptureAudit capture_audit;
  };

  [[nodiscard]] static ConsumedBatch consume(
      VerifiedStructuredBatch&& batch)
  {
    return {batch._source_graph, std::move(batch._prepared_batch),
            std::move(batch._resolver_delta),
            std::move(batch._descriptor),
            std::move(batch._capture_audit)};
  }

  using StageData = DualCsrVerifiedStage::Data;

  [[nodiscard]] static bool available(const DualCsrTransactionCoordinator& coordinator)
  {
    return !coordinator._poisoned && coordinator._pending.expired();
  }

  [[nodiscard]] static std::shared_ptr<StageData> begin(
      DualCsrTransactionCoordinator& coordinator, ConsumedBatch batch)
  {
    coordinator.reserveHistory();
    auto stage = std::make_shared<StageData>(coordinator._committed, batch.prepared_batch,
                                            std::move(batch.resolver_delta), std::move(batch.descriptor));
    coordinator._pending = stage;
    stage->resolver_patch.apply();
    stage->graph_patch.apply();
    stage->index_patch.apply();
    return stage;
  }

  [[nodiscard]] static DualCsrVerifiedStage makeVerified(std::shared_ptr<StageData> stage)
  {
    return DualCsrVerifiedStage(std::move(stage));
  }

  [[nodiscard]] static LevelizationExecutionTrace makeExecutionTrace(timing_edit::TransactionGeneration generation, LevelizationExecutionMode mode,
                                                                     const csr::CsrTimingValues& levels, csr::CsrTraversalTelemetry traversal,
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
  return committed.baseline && (batch.baselineOwner() == committed.baseline
                                || batch.baseline() == *committed.baseline);
}

std::optional<std::uint64_t> counterDelta(
    std::uint64_t before, std::uint64_t after) noexcept
{
  if (after < before)
    return std::nullopt;
  return after - before;
}

using PreparedStage = std::shared_ptr<DualCsrStageFactoryAccess::StageData>;
using PreparedStageAttempt = std::variant<PreparedStage, DualCsrStageFailure>;

PreparedStageAttempt prepareStage(
    DualCsrTransactionCoordinator& coordinator,
    DualCsrStageFactoryAccess::ConsumedBatch batch)
{
  if (!DualCsrStageFactoryAccess::available(coordinator))
    return makeDualFailure(DualCsrStageFailureOrigin::kCapability, DualCsrViolationCode::kConflict,
                           "stage.owner", "owner is pending or poisoned");
  const auto& committed = coordinator.committed();
  DualCsrViolations violations;
  const std::uint64_t expected = committed.generation.value();
  if (expected == std::numeric_limits<std::uint64_t>::max()
      || batch.prepared_batch.sessionToken() != committed.graph.session_token || batch.prepared_batch.expectedGeneration() != committed.generation
      || batch.prepared_batch.targetGeneration() != timing_edit::TransactionGeneration{expected + 1}
      || batch.source_graph != &committed.graph
      || (incregpusta_debug_checks_enabled && batch.prepared_batch.expectedPreCommittedIdentity() != committed.identity)
      || batch.prepared_batch.policyReceipt().disposition != timing_edit::EditDisposition::kSupported || !baselineMatches(committed, batch.prepared_batch)
      || batch.descriptor.session_token != batch.prepared_batch.sessionToken()
      || batch.descriptor.expected_generation != batch.prepared_batch.expectedGeneration()
      || batch.descriptor.target_generation != batch.prepared_batch.targetGeneration() || batch.descriptor.before != batch.prepared_batch.slotPlan().before
      || batch.descriptor.after != batch.prepared_batch.slotPlan().after || batch.resolver_delta.highWater() != batch.descriptor.after
      || (incregpusta_debug_checks_enabled
          && !validateNarrowCaptureAudit(batch.capture_audit,
                                         NarrowCaptureAuditExpectation{batch.descriptor.after, batch.descriptor.logical_deactivations.size()})))
    return makeDualFailure(
        DualCsrStageFailureOrigin::kCapability,
        DualCsrViolationCode::kGeneration, "stage.capability",
        "verified batch and exact committed state do not form the next generation");

  const auto update_started = SteadyClock::now();
  violations = validateDualCsrDescriptor(committed.graph, batch.descriptor);
  if (violations.empty())
    violations = validateDualCsrArcGroupIndexDelta(committed.graph, committed.arc_group_index, batch.descriptor);
  if (!violations.empty())
    return makeDualFailure(DualCsrStageFailureOrigin::kUpdater, std::move(violations));
  auto stage = DualCsrStageFactoryAccess::begin(coordinator, std::move(batch));
  stage->timings.dual_csr_update_ms = elapsedMilliseconds(update_started);
  return stage;
}

using BuiltStateAttempt = std::variant<PreparedStage, DualCsrStageFailure>;

BuiltStateAttempt buildStagedState(
    PreparedStage stage, DualCsrTimingResult timing, LevelizationExecutionTrace execution)
{
  if constexpr (incregpusta_debug_checks_enabled)
    stage->identity = deriveDualCsrCommittedIdentity(stage->owner->graph, stage->owner->resolver);
  stage->timing = std::move(timing);
  stage->batch.emplace(DualCsrPendingBatch{std::move(stage->semantic_batch), std::move(execution),
                                          {}});
  if constexpr (incregpusta_debug_checks_enabled) {
    stage->batch->descriptor = std::move(stage->descriptor);
    auto violations = validatePendingDualCsrAnalysisState(*stage->owner, stage->timing, stage->owner->resolver,
                                                          stage->identity, *stage->batch, stage->before_high_water);
    if (!violations.empty())
      return makeDualFailure(DualCsrStageFailureOrigin::kStateValidation, std::move(violations));
  }
  return stage;
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
    DualCsrTransactionCoordinator& coordinator,
    VerifiedStructuredBatch&& verified_batch)
{
  DualCsrStageAttempt attempt;
  auto prepared_attempt = prepareStage(
      coordinator,
      DualCsrStageFactoryAccess::consume(std::move(verified_batch)));
  if (auto* failure
      = std::get_if<DualCsrStageFailure>(&prepared_attempt)) {
    assignFailure(attempt, std::move(*failure));
    return attempt;
  }
  PreparedStage stage
      = std::move(std::get<PreparedStage>(prepared_attempt));
  attempt.timings = stage->timings;

  DualCsrEdgeViewImpl edges(stage->owner->graph);
  DualCsrTimingPayloadViewImpl payloads(stage->owner->graph);
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

  const auto fallback_delta = counterDelta(
      stage->fallback_before,
      stage->owner->graph.telemetry.fallback_count);
  if (!fallback_delta) {
    assignFailure(
        attempt,
        makeDualFailure(DualCsrStageFailureOrigin::kStateValidation,
                        DualCsrViolationCode::kTimingResult,
                        "stage.fallback_count",
                        "fallback counter regressed"));
    return attempt;
  }

  stage->owner->graph.telemetry.traversal = computation.telemetry;
  LevelizationExecutionTrace execution
      = DualCsrStageFactoryAccess::makeExecutionTrace(
          stage->descriptor.target_generation,
          LevelizationExecutionMode::kFullStage5, *values,
          computation.telemetry, std::move(entry_delta),
          *fallback_delta, false, std::nullopt);
  DualCsrTimingResult timing{std::move(*values)};
  const auto verify_started = SteadyClock::now();
  auto built = buildStagedState(std::move(stage),
                                std::move(timing),
                                std::move(execution));
  attempt.timings.verify_ms = elapsedMilliseconds(verify_started);
  if (auto* failure = std::get_if<DualCsrStageFailure>(&built)) {
    assignFailure(attempt, std::move(*failure));
    return attempt;
  }
  auto ready = std::move(std::get<PreparedStage>(built));
  ready->timings = attempt.timings;
  attempt.staged.emplace(DualCsrStageFactoryAccess::makeVerified(std::move(ready)));
  return attempt;
}

MixedDualCsrStageAttempt makeMixedLevelizedDualCsrAnalysisState(
    DualCsrTransactionCoordinator& coordinator,
    VerifiedStructuredBatch&& verified_batch)
{
  auto prepared_attempt = prepareStage(
      coordinator,
      DualCsrStageFactoryAccess::consume(std::move(verified_batch)));
  if (auto* failure
      = std::get_if<DualCsrStageFailure>(&prepared_attempt))
    return std::move(*failure);
  PreparedStage stage
      = std::move(std::get<PreparedStage>(prepared_attempt));

  DualCsrEdgeViewImpl edges(stage->owner->graph);
  DualCsrTimingPayloadViewImpl payloads(stage->owner->graph);
  const auto& previous_timing = stage->owner->timing.values;
  const timing_incremental_levelization::detail::PreviousLevels previous{
      previous_timing.level_offsets, previous_timing.level_nodes, previous_timing.node_to_level, true};
  const std::uint64_t generation = stage->descriptor.target_generation.value();
  auto guard = csr::beginCsrFullLevelizerOperation();
  auto mixed_attempt
      = timing_incremental_levelization::detail::runOwnedCpuIncrementalLevelization(
          edges, previous,
          MixedLevelizationRequest{
              .n_last_nodes = static_cast<std::uint64_t>(
                  stage->before_high_water.node_slots),
              .generation_before
                  = stage->owner->generation.value(),
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
  stage->timings.mixed_build_ms = mixed.telemetry.mixed_build_ms;
  stage->timings.mixed_kahn_ms = mixed.telemetry.mixed_kahn_ms;
  stage->timings.reconstruct_ms = mixed.telemetry.reconstruct_ms;
  auto computation = csr::detail::propagateCsrCpuReferenceWithLevels(edges, payloads, std::move(mixed.levels));
  auto entry_delta = std::move(guard).finish();
  if (entry_delta.value() != 0)
    return forbiddenFullEntry(generation, entry_delta.value());
  stage->timings.full_propagation_ms
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
      stage->fallback_before,
      stage->owner->graph.telemetry.fallback_count);
  if (!fallback_delta
      || *fallback_delta != mixed.telemetry.fallback_count)
    return makeMixedFailure(
        MixedStageFailureCode::kInvariantFailure,
        MixedFailureOrigin::kPropagation, std::nullopt, generation,
        "mixed fallback telemetry differs from the real graph counter delta");

  stage->owner->graph.telemetry.traversal = computation.telemetry;
  LevelizationExecutionTrace execution
      = DualCsrStageFactoryAccess::makeExecutionTrace(
          stage->descriptor.target_generation,
          LevelizationExecutionMode::kMixedStage6, *values,
          computation.telemetry, std::move(entry_delta),
          *fallback_delta, true,
          projectDeterministicMixedTrace(mixed.telemetry));
  DualCsrTimingResult timing{std::move(*values)};
  const auto verify_started = SteadyClock::now();
  DualCsrStageTimings timings = stage->timings;
  auto built = buildStagedState(std::move(stage), std::move(timing),
                                std::move(execution));
  timings.verify_ms = elapsedMilliseconds(verify_started);
  if (auto* failure = std::get_if<DualCsrStageFailure>(&built))
    return std::move(*failure);
  auto ready = std::move(std::get<PreparedStage>(built));
  ready->timings = timings;
  return DualCsrStageFactoryAccess::makeVerified(std::move(ready));
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::publish(
    DualCsrVerifiedStage verified, DualCsrPublishFailpoint failpoint)
{
  const auto cancel = [&](DualCsrTransactionCode code) {
    // A by-value parameter may outlive this call until the caller's full
    // expression ends. Undo and release pending before reporting rejection.
    verified._data.reset();
    return reject(code);
  };
  if (_poisoned)
    return cancel(DualCsrTransactionCode::kPoisoned);
  if (failpoint != DualCsrPublishFailpoint::kNone
      && (!incregpusta_debug_checks_enabled
          || (failpoint != DualCsrPublishFailpoint::kBeforeSwap && failpoint != DualCsrPublishFailpoint::kAfterSwap)))
    return cancel(DualCsrTransactionCode::kUnknownFailpoint);
  const auto& stage = verified._data;
  if (!stage || stage->owner != _committed || _pending.lock() != stage || !stage->expected.matches(*_committed))
    return cancel(DualCsrTransactionCode::kTraceMismatch);
  auto& current = *_committed;
  if (!stage->batch || current.graph.generation.value() != current.generation.value() + 1
      || (stage->identity && stage->identity->generation != current.graph.generation)
      || (incregpusta_debug_checks_enabled && !stage->identity))
    return cancel(DualCsrTransactionCode::kStaleGeneration);
  auto& batch = *stage->batch;
  if (batch.semantic.expected_generation != current.generation || batch.execution.generation() != current.graph.generation
      || !current.baseline)
    return cancel(DualCsrTransactionCode::kTraceMismatch);
  if (failpoint == DualCsrPublishFailpoint::kBeforeSwap)
    return cancel(DualCsrTransactionCode::kPreSwapFault);

  // History, values and undo were fully prepared before apply.
  using std::swap;
  swap(current.timing, stage->timing);
  swap(current.identity, stage->identity);
  current.semantic_trace.push_back(std::move(batch.semantic));
  current.execution_trace.push_back(std::move(batch.execution));
  if constexpr (incregpusta_debug_checks_enabled)
    current.descriptor_trace.push_back(std::move(*batch.descriptor));
  current.generation = current.graph.generation;
  current.traversal_telemetry = current.graph.telemetry.traversal;
  stage->index_patch.commit();
  stage->graph_patch.commit();
  stage->resolver_patch.commit();
  _pending.reset();
  if (failpoint == DualCsrPublishFailpoint::kAfterSwap) {
    _poisoned = true;
    return {DualCsrTransactionCode::kPostSwapPoisoned, true, true};
  }
  return {DualCsrTransactionCode::kOk, true, false};
}

}  // namespace ista::timing_dual_csr
