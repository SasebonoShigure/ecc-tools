// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTransactionCoordinator.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "DataManager.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrExactReceipt.hpp"
#include "DualCsrStageFactoryFaultAdapter.hpp"
#include "DualCsrStructuredBatchTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrUpdater.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using structured_support::StructuredFixture;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<std::uint8_t> activeMask(const DualCsrState& graph)
{
  std::vector<std::uint8_t> result;
  result.reserve(graph.arc_slots.size());
  for (const auto& slot : graph.arc_slots)
    result.push_back(slot.active ? 1U : 0U);
  return result;
}

DualCsrTimingResult timingFor(const DualCsrState& graph)
{
  csr::CsrCpuReferenceBackend backend;
  DualCsrEdgeViewImpl edges(graph);
  DualCsrTimingPayloadViewImpl payloads(graph);
  auto output = backend.computeWithTelemetry(edges, payloads);
  auto* values = std::get_if<csr::CsrTimingResult>(
      &output.computation.outcome);
  require(values != nullptr,
          "transaction fixture full CPU timing failed");
  DualCsrTimingResult timing{std::move(*values), activeMask(graph)};
  const auto violations = validateDualCsrTimingResult(graph, timing);
  require(violations.empty(),
          "transaction fixture timing was invalid"
              + (violations.empty()
                     ? std::string{}
                     : ": " + violations.front().field));
  return timing;
}

CommittedDualCsrAnalysisState initialAggregate(
    const StructuredFixture& fixture)
{
  CommittedDualCsrAnalysisState state{
      .graph = fixture.graph,
      .timing = timingFor(fixture.graph),
      .resolver = fixture.resolver,
      .arc_group_index = fixture.arc_group_index,
      .identity = fixture.identity,
      .generation = timing_edit::TransactionGeneration{0},
      .descriptor_trace = {},
      .semantic_trace = {},
      .traversal_telemetry = fixture.graph.telemetry.traversal};
  const auto violations = validateCommittedDualCsrAnalysisState(state);
  require(violations.empty(),
          "initial aggregate was invalid"
              + (violations.empty()
                     ? std::string{}
                     : ": " + violations.front().field));
  return state;
}

DualCsrVerifiedStage verifiedStage(
    timing_edit::test::GuardedTestSession& session,
    const StructuredFixture& fixture,
    const CommittedDualCsrAnalysisState& committed,
    NarrowCaptureAudit* audit = nullptr)
{
  auto verified = structured_support::captureAndBuildVerifiedBatch(
      session, fixture);
  if (audit != nullptr)
    *audit = verified.captureAudit();
  auto attempt = makeStagedDualCsrAnalysisState(
      committed, std::move(verified));
  require(attempt.success(),
          "verified structured batch did not stage"
              + (attempt.violations.empty()
                     ? std::string{}
                     : ": " + attempt.violations.front().field + " "
                           + attempt.violations.front().message));
  const auto& trace = attempt.staged->state().execution_trace;
  require(trace.size() == 1
              && trace.front().mode()
                     == LevelizationExecutionMode::kFullStage5
              && trace.front().fullLevelizerEntryDelta() == 1
              && trace.front().fallbackCount() == 0
              && !trace.front().suppliedLevelsValidated()
              && !trace.front().mixed().has_value(),
          "Stage 5 full factory did not seal a real full execution record");
  return std::move(*attempt.staged);
}

CommittedDualCsrAnalysisState freshEquivalent(
    const CommittedDualCsrAnalysisState& committed,
    std::uint64_t session)
{
  auto rebind = rebindRecoveredAggregateSession(
      committed, committed.graph.session_token,
      timing_edit::TimingEditSessionToken{session});
  require(rebind.success(),
          "typed recovery aggregate rebind was rejected");
  CommittedDualCsrAnalysisState rebuilt
      = std::move(*rebind.rebound);
  ++rebuilt.graph.telemetry.full_rebuild_count;
  const auto violations = validateCommittedDualCsrAnalysisState(rebuilt);
  require(violations.empty(),
          "fresh equivalent aggregate was invalid"
              + (violations.empty()
                     ? std::string{}
                     : ": " + violations.front().field));
  return rebuilt;
}

DualCsrTransactionEvidence successfulEvidence(
    NarrowCaptureAudit capture)
{
  DualCsrTransactionEvidence evidence;
  evidence.phase_trace = {
      DualCsrReceiptPhase::kPreCandidatePrepare,
      DualCsrReceiptPhase::kCandidateApplyExecute,
      DualCsrReceiptPhase::kPostCandidateCapture,
      DualCsrReceiptPhase::kDualCsrUpdate,
      DualCsrReceiptPhase::kFullLevelization,
      DualCsrReceiptPhase::kFullPropagation,
      DualCsrReceiptPhase::kIntrinsicVerify,
      DualCsrReceiptPhase::kPublish,
      DualCsrReceiptPhase::kNativeFinalRollback};
  evidence.capture_audits = {std::move(capture)};
  evidence.operation_counters.candidate_creates = 1;
  evidence.operation_counters.candidate_applies = 1;
  evidence.operation_counters.candidate_executes = 1;
  evidence.operation_counters.candidate_installs = 1;
  evidence.operation_counters.candidate_rollbacks = 1;
  evidence.incremental_batch_executed = true;
  evidence.native_final_rollback_completed = true;
  return evidence;
}

void verifiedCapabilityAndPublishContracts()
{
  static_assert(!std::is_copy_constructible_v<VerifiedStructuredBatch>);
  static_assert(!std::is_copy_constructible_v<DualCsrVerifiedStage>);
  static_assert(std::is_nothrow_move_constructible_v<
                DualCsrVerifiedStage>);
  static_assert(!std::is_default_constructible_v<
                DualCsrVerifiedStage>);

  {
    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession session;
    const StructuredFixture fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), session.token());
    const auto initial = initialAggregate(fixture);
    DualCsrTransactionCoordinator coordinator(initial);
    const auto before = coordinator.committed();
    const auto status = coordinator.publish(
        verifiedStage(session, fixture, initial),
        DualCsrPublishFailpoint::kBeforeSwap);
    require(status.code == DualCsrTransactionCode::kPreSwapFault
                && !status.published && !status.poisoned
                && coordinator.committed() == before,
            "pre-swap failure changed the committed aggregate");
    require(session.closeAndRelease().ok(),
            "pre-swap session close failed");
  }
  {
    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession session;
    const StructuredFixture fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), session.token());
    const auto initial = initialAggregate(fixture);
    DualCsrTransactionCoordinator coordinator(initial);
    const auto status = coordinator.publish(
        verifiedStage(session, fixture, initial),
        DualCsrPublishFailpoint::kAfterSwap);
    require(status.code == DualCsrTransactionCode::kPostSwapPoisoned
                && status.published && status.poisoned
                && coordinator.committed().generation
                       == timing_edit::TransactionGeneration{1},
            "post-swap failure was not atomic-and-poisoned");
    require(session.closeAndRelease().ok(),
            "post-swap session close failed");
  }
  {
    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession session;
    const StructuredFixture fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), session.token());
    const auto initial = initialAggregate(fixture);
    auto stale = verifiedStage(session, fixture, initial);
    DualCsrTransactionCoordinator coordinator(stale.state());
    const auto committed = coordinator.committed();
    const auto rejected = coordinator.publish(std::move(stale));
    require(rejected.code
                       == DualCsrTransactionCode::kTraceMismatch
                && !rejected.published && !rejected.poisoned
                && coordinator.committed() == committed,
            "source-bound stage replay changed a newer aggregate");
    require(session.closeAndRelease().ok(),
            "replay session close failed");
  }
}

void mixedFactoryAndAtomicityContracts()
{
  {
    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession session;
    const StructuredFixture fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), session.token());
    const auto initial = initialAggregate(fixture);
    auto verified = structured_support::captureAndBuildVerifiedBatch(
        session, fixture);
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        initial, std::move(verified));
    auto* staged = std::get_if<DualCsrVerifiedStage>(&attempt);
    require(staged != nullptr,
            "valid structured batch did not produce a mixed verified stage");
    const auto& state = staged->state();
    require(state.execution_trace.size() == 1,
            "mixed stage did not append exactly one execution record");
    const auto& trace = state.execution_trace.front();
    require(trace.mode() == LevelizationExecutionMode::kMixedStage6
                && trace.fullLevelizerEntryDelta() == 0
                && trace.fallbackCount() == 0
                && trace.suppliedLevelsValidated()
                && trace.mixed().has_value(),
            "mixed stage execution record has the wrong mode boundary");
    const auto& mixed = *trace.mixed();
    require(mixed.n_last_nodes == initial.graph.nodes.size()
                && mixed.n_current_nodes == state.graph.nodes.size()
                && mixed.n_new_nodes > 0
                && mixed.kahn_processed_count
                       == mixed.mixed_vertex_count
                && mixed.reconstructed_level_count
                       == trace.levels().level_offsets.size() - 1,
            "mixed execution record lost its real generation or Kahn boundary");
    require(validateCommittedDualCsrAnalysisState(state).empty(),
            "mixed verified stage is not a valid detached aggregate");

    DualCsrTransactionCoordinator coordinator(initial);
    const auto before = coordinator.committed();
    const auto rejected = coordinator.publish(
        std::move(*staged), DualCsrPublishFailpoint::kBeforeSwap);
    require(rejected.code == DualCsrTransactionCode::kPreSwapFault
                && !rejected.published && !rejected.poisoned
                && coordinator.committed() == before,
            "pre-swap mixed failure changed the committed aggregate");
    require(session.closeAndRelease().ok(),
            "mixed pre-swap session close failed");
  }

  {
    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession session;
    const StructuredFixture fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), session.token());
    const auto initial = initialAggregate(fixture);
    auto verified = structured_support::captureAndBuildVerifiedBatch(
        session, fixture);
    auto malformed = initial;
    malformed.generation = timing_edit::TransactionGeneration{1};
    const auto before = malformed;
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        malformed, std::move(verified));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    require(failure != nullptr
                && failure->origin
                       == DualCsrStageFailureOrigin::kExactBefore
                && !failure->violations.empty()
                && malformed == before,
            "malformed exact-before was erased into a mixed failure or mutated");
    require(session.closeAndRelease().ok(),
            "mixed typed-failure session close failed");
  }
}

void mixedFactoryFailureInputContracts()
{
  {
    std::optional<CommittedDualCsrAnalysisState> generation_one;
    timing_edit::TimingEditSessionToken source_token;
    {
      timing_edit::test::installNativeTransactionBaseline();
      timing_edit::test::GuardedTestSession source_session;
      const StructuredFixture source_fixture
          = structured_support::makeStructuredFixture(
              STADM.getDatabase(), source_session.token());
      const auto source_initial = initialAggregate(source_fixture);
      auto advance = structured_support::captureAndBuildVerifiedBatch(
          source_session, source_fixture);
      auto advance_attempt = makeMixedLevelizedDualCsrAnalysisState(
          source_initial, std::move(advance));
      const auto* advanced
          = std::get_if<DualCsrVerifiedStage>(&advance_attempt);
      require(advanced != nullptr,
              "stale exact-before fixture did not produce generation one");
      generation_one = advanced->state();
      source_token = source_session.token();
      require(source_session.closeAndRelease().ok(),
              "stale source session close failed");
    }

    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession stale_session;
    const StructuredFixture stale_fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), stale_session.token());
    const auto stale_initial = initialAggregate(stale_fixture);
    auto stale = structured_support::captureAndBuildVerifiedBatch(
        stale_session, stale_fixture);
    auto rebound = rebindRecoveredAggregateSession(
        std::move(*generation_one), source_token,
        stale_session.token());
    require(rebound.success(),
            "generation-one fixture did not rebind to stale session");
    const auto before = *rebound.rebound;
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        *rebound.rebound, std::move(stale));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    const DualCsrViolations expected{{
        DualCsrViolationCode::kExpectedValue,
        "stage.exact_before",
        "verified capability is not bound to the exact committed aggregate"}};
    require(failure != nullptr
                && failure->origin
                       == DualCsrStageFailureOrigin::kExactBefore
                && failure->violations == expected
                && *rebound.rebound == before
                && validateCommittedDualCsrAnalysisState(stale_initial)
                       .empty(),
            "valid stale exact-before lost its typed failure or mutated input");
    require(stale_session.closeAndRelease().ok(),
            "stale exact-before session close failed");
  }

  {
    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession session;
    const StructuredFixture fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), session.token());
    const auto initial = initialAggregate(fixture);
    auto verified = structured_support::captureAndBuildVerifiedBatch(
        session, fixture);
    auto injected_descriptor = verified.descriptor();
    DualCsrStageFactoryFaultAdapter::corruptIndexDelta(
        injected_descriptor);
    const auto expected = validateDualCsrArcGroupIndexDelta(
        initial.graph, initial.arc_group_index, injected_descriptor);
    require(!expected.empty(),
            "wrong-index input did not fail the real delta validator");
    DualCsrStageFactoryFaultAdapter::corruptIndexDelta(verified);
    const auto before = initial;
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        initial, std::move(verified));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    require(failure != nullptr
                && failure->origin
                       == DualCsrStageFailureOrigin::kIndexDelta
                && failure->violations == expected && initial == before,
            "wrong-index failure was erased, changed, or mutated committed");
    require(session.closeAndRelease().ok(),
            "wrong-index session close failed");
  }

  {
    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession session;
    const StructuredFixture fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), session.token());
    const auto initial = initialAggregate(fixture);
    auto verified = structured_support::captureAndBuildVerifiedBatch(
        session, fixture);
    auto injected_descriptor = verified.descriptor();
    require(DualCsrStageFactoryFaultAdapter::corruptUpdaterDescriptor(
                injected_descriptor),
            "updater-conflict fixture has no appended node");
    require(validateDualCsrArcGroupIndexDelta(
                initial.graph, initial.arc_group_index,
                injected_descriptor)
                .empty(),
            "updater-conflict input failed before the updater boundary");
    const auto expected_attempt = DualCsrUpdater::stage(
        initial.graph, initial.arc_group_index, injected_descriptor);
    require(!expected_attempt.success()
                && !expected_attempt.violations.empty(),
            "updater-conflict input did not fail the real updater");
    require(DualCsrStageFactoryFaultAdapter::corruptUpdaterDescriptor(
                verified),
            "verified updater-conflict fixture has no appended node");
    const auto before = initial;
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        initial, std::move(verified));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    require(failure != nullptr
                && failure->origin == DualCsrStageFailureOrigin::kUpdater
                && failure->violations == expected_attempt.violations
                && initial == before,
            "updater failure was erased, changed, or mutated committed");
    require(session.closeAndRelease().ok(),
            "updater-conflict session close failed");
  }

  {
    timing_edit::test::installNativeTransactionBaseline();
    timing_edit::test::GuardedTestSession session;
    const StructuredFixture fixture
        = structured_support::makeStructuredFixture(
            STADM.getDatabase(), session.token());
    const auto initial = initialAggregate(fixture);
    auto verified = structured_support::captureAndBuildVerifiedBatch(
        session, fixture);
    require(DualCsrStageFactoryFaultAdapter::corruptIntrinsicTimingInput(
                verified),
            "intrinsic fixture lacks an unconstrained output endpoint");
    require(validateDualCsrArcGroupIndexDelta(
                initial.graph, initial.arc_group_index,
                verified.descriptor())
                .empty(),
            "intrinsic input failed before the updater boundary");
    const auto expected_update = DualCsrUpdater::stage(
        initial.graph, initial.arc_group_index,
        verified.descriptor());
    require(expected_update.success(),
            "finite intrinsic input did not pass the real updater"
                + (expected_update.violations.empty()
                       ? std::string{}
                       : ": " + expected_update.violations.front().field
                             + " " + expected_update.violations.front().message));
    DualCsrEdgeViewImpl edges(*expected_update.state);
    DualCsrTimingPayloadViewImpl payloads(*expected_update.state);
    csr::CsrCpuReferenceBackend backend;
    const auto backend_output = backend.computeWithTelemetry(edges, payloads);
    require(std::get_if<csr::CsrTimingResult>(
                &backend_output.computation.outcome)
                == nullptr,
            "finite intrinsic input did not fail the real CPU backend");
    const DualCsrViolations expected{{
        DualCsrViolationCode::kTimingResult,
        "stage.mixed_cpu",
        "external-level full CPU propagation did not produce timing"}};
    const auto before = initial;
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        initial, std::move(verified));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    require(failure != nullptr
                && failure->origin
                       == DualCsrStageFailureOrigin::kTimingIntrinsic
                && failure->violations == expected && initial == before,
            "intrinsic failure was erased, changed, or mutated committed");
    require(session.closeAndRelease().ok(),
            "intrinsic timing session close failed");
  }
}

void recoveryContracts()
{
  timing_edit::test::installNativeTransactionBaseline();
  timing_edit::test::GuardedTestSession session;
  const StructuredFixture fixture
      = structured_support::makeStructuredFixture(
          STADM.getDatabase(), session.token());
  const auto initial = initialAggregate(fixture);
  DualCsrTransactionCoordinator coordinator(initial);
  const auto poisoned = coordinator.publish(
      verifiedStage(session, fixture, initial),
      DualCsrPublishFailpoint::kAfterSwap);
  require(poisoned.poisoned, "recovery fixture did not poison");

  const auto poisoned_state = coordinator.committed();
  const auto clean_rebuilt = freshEquivalent(
      poisoned_state, session.token().value() + 1000);
  using TelemetryCounter = std::uint64_t DualCsrTelemetry::*;
  const std::array<TelemetryCounter, 17> non_rebuild_counters{
      &DualCsrTelemetry::generation_before,
      &DualCsrTelemetry::generation_after,
      &DualCsrTelemetry::logical_edges_before,
      &DualCsrTelemetry::logical_edges_after,
      &DualCsrTelemetry::base_physical_edges,
      &DualCsrTelemetry::base_active_edges,
      &DualCsrTelemetry::base_tombstones,
      &DualCsrTelemetry::aux_rows,
      &DualCsrTelemetry::aux_edges,
      &DualCsrTelemetry::tombstone_reuse_count,
      &DualCsrTelemetry::base_change_count,
      &DualCsrTelemetry::reverse_change_count,
      &DualCsrTelemetry::nodes_appended,
      &DualCsrTelemetry::arcs_appended,
      &DualCsrTelemetry::payload_change_count,
      &DualCsrTelemetry::aux_rebuild_count,
      &DualCsrTelemetry::fallback_count};
  for (const TelemetryCounter counter : non_rebuild_counters) {
    auto forged = clean_rebuilt;
    ++(forged.graph.telemetry.*counter);
    require(!makeDualCsrVerifiedRecovery(
                 poisoned_state, std::move(forged))
                 .success(),
            "recovery accepted a changed non-rebuild telemetry counter");
  }
  auto forged_capacity = clean_rebuilt;
  forged_capacity.graph.telemetry.capacity_grow_events.push_back({});
  require(!makeDualCsrVerifiedRecovery(
               poisoned_state, std::move(forged_capacity))
               .success(),
          "recovery accepted changed capacity-grow telemetry");
  auto forged_traversal = clean_rebuilt;
  ++forged_traversal.graph.telemetry.traversal.levelization.base_out;
  require(!makeDualCsrVerifiedRecovery(
               poisoned_state, std::move(forged_traversal))
               .success(),
          "recovery accepted changed graph traversal telemetry");
  auto double_rebuild = clean_rebuilt;
  ++double_rebuild.graph.telemetry.full_rebuild_count;
  require(!makeDualCsrVerifiedRecovery(
               poisoned_state, std::move(double_rebuild))
               .success(),
          "recovery accepted a full-rebuild increment other than one");
  auto overflow_poisoned = poisoned_state;
  overflow_poisoned.graph.telemetry.full_rebuild_count
      = std::numeric_limits<std::uint64_t>::max();
  auto wrapped_rebuilt = freshEquivalent(
      overflow_poisoned, session.token().value() + 2000);
  require(!makeDualCsrVerifiedRecovery(
               overflow_poisoned, std::move(wrapped_rebuilt))
               .success(),
          "recovery accepted a wrapped full-rebuild counter");

  auto clone_rebind_bypass = freshEquivalent(
      coordinator.committed(), session.token().value() + 3000);
  const auto bypass = makeDualCsrVerifiedRecovery(
      coordinator.committed(), std::move(clone_rebind_bypass));
  require(!bypass.success() && coordinator.poisoned(),
          "legacy clone/rebind recovery bypassed persisted mode replay");
  require(session.closeAndRelease().ok(),
          "transaction recovery session close failed");
}

void splitReceiptContracts()
{
  timing_incremental_levelization::MixedLevelizationTelemetry telemetry;
  const auto deterministic = projectDeterministicMixedTrace(telemetry);
  telemetry.mixed_build_ms = 1.0;
  telemetry.mixed_kahn_ms = 2.0;
  telemetry.reconstruct_ms = 3.0;
  telemetry.legal_noncompact = true;
  require(projectDeterministicMixedTrace(telemetry) == deterministic,
          "run observations leaked into deterministic recovery equality");
  ++telemetry.old_old;
  require(projectDeterministicMixedTrace(telemetry) != deterministic
              && !timing_incremental_levelization::validateMixedTelemetryIdentities(telemetry),
          "deterministic projection lost a counter or its validation");

  timing_edit::test::installNativeTransactionBaseline();
  timing_edit::test::GuardedTestSession session;
  const StructuredFixture fixture
      = structured_support::makeStructuredFixture(
          STADM.getDatabase(), session.token());
  const auto initial = initialAggregate(fixture);
  DualCsrTransactionCoordinator coordinator(initial);
  NarrowCaptureAudit capture;
  const auto published = coordinator.publish(
      verifiedStage(session, fixture, initial, &capture));
  require(published.ok() && published.published,
          "receipt fixture did not publish");

  auto observation = makeDualCsrPublishedObservation(
      coordinator.committed());
  require(observation.success(),
          "post-publish observation was rejected");
  auto structural_canary = observation.observation->structural;
  ++structural_canary.high_water.arc_slots;
  require(!validateDualCsrStructuralReceipt(
               coordinator.committed(), structural_canary)
               .empty(),
          "structural receipt accepted a forged high-water mark");

  auto evidence = successfulEvidence(capture);
  auto exact = finalizeDualCsrExactReceipt(
      coordinator.committed(), *observation.observation,
      fixture.baseline_reference_input, evidence);
  require(exact.success()
              && exact.receipt->published == *observation.observation
              && exact.receipt->transaction.generation
                     == timing_edit::TransactionGeneration{1},
          "valid finalized receipt was rejected");

  auto malformed_baseline = fixture.baseline_reference_input;
  require(!malformed_baseline.nodes.empty(),
          "receipt baseline fixture unexpectedly has no nodes");
  malformed_baseline.nodes.front().net_name += "_forged";
  require(!finalizeDualCsrExactReceipt(
               coordinator.committed(), *observation.observation,
               std::move(malformed_baseline), evidence)
               .success(),
          "transaction receipt accepted a malformed full typed baseline anchor");

  auto premature = evidence;
  premature.native_final_rollback_completed = false;
  require(!finalizeDualCsrExactReceipt(
               coordinator.committed(), *observation.observation,
               fixture.baseline_reference_input,
               std::move(premature))
               .success(),
          "transaction receipt finalized before run-final rollback");
  auto full_export = evidence;
  full_export.operation_counters.native_full_reference_exports = 1;
  require(!finalizeDualCsrExactReceipt(
               coordinator.committed(), *observation.observation,
               fixture.baseline_reference_input,
               std::move(full_export))
               .success(),
          "transaction receipt accepted a targeted full export");
  auto wrong_order = std::move(evidence);
  std::swap(wrong_order.phase_trace[0], wrong_order.phase_trace[1]);
  require(!finalizeDualCsrExactReceipt(
               coordinator.committed(), *observation.observation,
               fixture.baseline_reference_input,
               std::move(wrong_order))
               .success(),
          "transaction receipt accepted a wrong phase order");
  require(session.closeAndRelease().ok(),
          "transaction receipt session close failed");
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_edit::test::NativeTransactionRuntime runtime;
    ista::timing_dual_csr::test::verifiedCapabilityAndPublishContracts();
    ista::timing_dual_csr::test::mixedFactoryAndAtomicityContracts();
    ista::timing_dual_csr::test::mixedFactoryFailureInputContracts();
    ista::timing_dual_csr::test::recoveryContracts();
    ista::timing_dual_csr::test::splitReceiptContracts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
