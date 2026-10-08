// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTransactionCoordinator.hpp"
#include "AllocationFailureSupport.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
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
#include "DualCsrDescriptorValidation.hpp"
#include "DualCsrUpdaterInternal.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using structured_support::StructuredFixture;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
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
  DualCsrTimingResult timing{std::move(*values)};
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
      .traversal_telemetry = fixture.graph.telemetry.traversal,
      .baseline = std::make_shared<const timing_edit::BaselineSemanticIdentity>(fixture.plan.baseline)};
  const auto violations = validateCommittedDualCsrAnalysisState(state);
  require(violations.empty(),
          "initial aggregate was invalid"
              + (violations.empty()
                     ? std::string{}
                     : ": " + violations.front().field));
  return state;
}

void baselineOwnershipContract(const StructuredFixture& fixture)
{
  auto initial = initialAggregate(fixture);
  auto supplied = std::make_shared<timing_edit::BaselineSemanticIdentity>(*initial.baseline);
  initial.baseline = supplied;
  DualCsrTransactionCoordinator coordinator(std::move(initial));
  supplied->canonical_pre_state.nodes.clear();
  require(*coordinator.committed().baseline == fixture.plan.baseline,
          "coordinator retained an external mutable baseline alias");
  auto malformed = initialAggregate(fixture);
  auto wrong = std::make_shared<timing_edit::BaselineSemanticIdentity>(*malformed.baseline);
  wrong->fingerprints.topology += "-wrong";
  malformed.baseline = wrong;
  require(!validateCommittedDualCsrAnalysisState(malformed).empty(),
          "public aggregate validator accepted a mismatched generation-zero baseline");
}

VerifiedStructuredBatch captureOwnedBatch(timing_edit::test::GuardedTestSession& session,
                                           const StructuredFixture& fixture,
                                           DualCsrTransactionCoordinator& coordinator)
{
  const auto& owner = coordinator.committed();
  auto pending = coordinator.prepareBatch({fixture.plan.expected_generation, fixture.plan.edits});
  require(pending.preparedBatch().baselineOwner() == owner.baseline,
          "controlled preparation did not retain its immutable session baseline");
  auto capture = structured_support::captureCandidateForContractTest(session, fixture, pending);
  return TimingEditDualCsrDescriptorBuilder::build(std::move(pending), std::move(capture));
}

DualCsrVerifiedStage verifiedStage(
    timing_edit::test::GuardedTestSession& session,
    const StructuredFixture& fixture,
    DualCsrTransactionCoordinator& coordinator,
    NarrowCaptureAudit* audit = nullptr)
{
  auto verified = captureOwnedBatch(session, fixture, coordinator);
  if (audit != nullptr)
    *audit = verified.captureAudit();
  auto attempt = makeStagedDualCsrAnalysisState(
      coordinator, std::move(verified));
  require(attempt.success(),
          "verified structured batch did not stage"
              + (attempt.violations.empty()
                     ? std::string{}
                     : ": " + attempt.violations.front().field + " "
                           + attempt.violations.front().message));
  const auto& trace = attempt.staged->batch().execution;
  require(trace.mode()
                     == LevelizationExecutionMode::kFullStage5
              && trace.fullLevelizerEntryDelta() == 1
              && trace.fallbackCount() == 0
              && !trace.suppliedLevelsValidated()
              && !trace.mixed().has_value(),
          "Stage 5 full factory did not seal a real full execution record");
  return std::move(*attempt.staged);
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
      DualCsrReceiptPhase::kPublish};
  evidence.capture_audits = {std::move(capture)};
  evidence.incremental_batch_executed = true;
  return evidence;
}

bool sameCommittedValues(const CommittedDualCsrAnalysisState& actual, const CommittedDualCsrAnalysisState& expected)
{
  // Failed preparation may retain reserved capacity; logical state must match.
  auto normalized = actual;
  normalized.graph.base_out.offsets_extent = expected.graph.base_out.offsets_extent;
  normalized.graph.base_out.edge_extent = expected.graph.base_out.edge_extent;
  normalized.graph.base_in.offsets_extent = expected.graph.base_in.offsets_extent;
  normalized.graph.base_in.edge_extent = expected.graph.base_in.edge_extent;
  normalized.graph.aux_out.rows_extent = expected.graph.aux_out.rows_extent;
  normalized.graph.aux_out.source_extent = expected.graph.aux_out.source_extent;
  normalized.graph.aux_out.mapping_extent = expected.graph.aux_out.mapping_extent;
  return normalized == expected;
}

void historyAllocationFailure()
{
  timing_edit::test::installNativeTransactionBaseline();
  timing_edit::test::GuardedTestSession session;
  const auto fixture = structured_support::makeStructuredFixture(STADM.getDatabase(), session.token());
  baselineOwnershipContract(fixture);
  const auto initial = initialAggregate(fixture);
  DualCsrTransactionCoordinator coordinator(initial);
  auto verified = captureOwnedBatch(session, fixture, coordinator);
  bool failed = false;
  auto* history = const_cast<CommittedDualCsrAnalysisState*>(&coordinator.committed());
  ista_test_allocation::armFailureWhen([](void* context) noexcept {
    const auto& state = *static_cast<const CommittedDualCsrAnalysisState*>(context);
    return state.semantic_trace.capacity() != 0 && state.execution_trace.capacity() == 0;
  }, history);
  try {
    static_cast<void>(makeStagedDualCsrAnalysisState(coordinator, std::move(verified)));
  } catch (const std::bad_alloc&) {
    failed = true;
  }
  static_cast<void>(ista_test_allocation::disable());
  require(failed && history->semantic_trace.capacity() != 0 && history->execution_trace.capacity() == 0
              && sameCommittedValues(coordinator.committed(), initial) && !coordinator.poisoned(),
          "history allocation failure changed committed state");
  require(coordinator.publish(verifiedStage(session, fixture, coordinator)).ok(), "history allocation failure prevented retry");
  require(session.closeAndRelease().ok(), "history allocation test close failed");
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
        verifiedStage(session, fixture, coordinator),
        DualCsrPublishFailpoint::kBeforeSwap);
    require(status.code == DualCsrTransactionCode::kPreSwapFault
                && !status.published && !status.poisoned
                && sameCommittedValues(coordinator.committed(), before),
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
        verifiedStage(session, fixture, coordinator),
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
    DualCsrTransactionCoordinator coordinator(initial);
    auto foreign_batch = captureOwnedBatch(session, fixture, coordinator);
    DualCsrTransactionCoordinator foreign_owner(initial);
    const auto foreign_attempt = makeStagedDualCsrAnalysisState(foreign_owner, std::move(foreign_batch));
    require(!foreign_attempt.success() && foreign_attempt.failure_origin == DualCsrStageFailureOrigin::kCapability
                && sameCommittedValues(foreign_owner.committed(), initial) && sameCommittedValues(coordinator.committed(), initial),
            "same-valued foreign owner accepted another graph's prepared batch");
    auto stale_batch = captureOwnedBatch(session, fixture, coordinator);
    auto concurrent_batch = captureOwnedBatch(session, fixture, coordinator);
    auto pending = verifiedStage(session, fixture, coordinator);
    bool query_rejected = false;
    try { static_cast<void>(coordinator.committed()); }
    catch (const std::logic_error&) { query_rejected = true; }
    require(query_rejected, "pending owner exposed uncommitted state");
    auto concurrent = makeStagedDualCsrAnalysisState(coordinator, std::move(concurrent_batch));
    require(!concurrent.success() && concurrent.failure_origin == DualCsrStageFailureOrigin::kCapability,
            "owner accepted a second pending transaction");
    DualCsrTransactionCoordinator other(initial);
    require(coordinator.canPublish(pending) && !other.canPublish(pending),
            "publication precheck lost the owning coordinator");
    require(other.publish(std::move(pending)).code == DualCsrTransactionCode::kTraceMismatch
                && sameCommittedValues(other.committed(), initial)
                && sameCommittedValues(coordinator.committed(), initial),
            "foreign publish changed either owner's logical state");
    auto committed_stage = verifiedStage(session, fixture, coordinator);
    ista_test_allocation::armFailure(1);
    const bool ready = coordinator.canPublish(committed_stage);
    const auto published = coordinator.publish(std::move(committed_stage));
    const bool consumed = !coordinator.canPublish(committed_stage);
    const auto commit_allocations = ista_test_allocation::disable();
    require(ready && consumed && published.ok() && commit_allocations == 0, "prepared precheck/commit allocated or reused a stage");
    const auto committed = coordinator.committed();
    auto stale = makeStagedDualCsrAnalysisState(coordinator, std::move(stale_batch));
    require(!stale.success() && sameCommittedValues(coordinator.committed(), committed),
            "stale generation changed the current aggregate");
    std::optional<DualCsrVerifiedStage> orphan;
    {
      DualCsrTransactionCoordinator temporary(initial);
      orphan.emplace(verifiedStage(session, fixture, temporary));
    }
    orphan.reset();
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
    DualCsrTransactionCoordinator coordinator(initial);
    auto verified = captureOwnedBatch(session, fixture, coordinator);
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        coordinator, std::move(verified));
    auto* staged = std::get_if<DualCsrVerifiedStage>(&attempt);
    require(staged != nullptr,
            "valid structured batch did not produce a mixed verified stage");
    const auto& trace = staged->batch().execution;
    require(trace.mode() == LevelizationExecutionMode::kMixedStage6
                && trace.fullLevelizerEntryDelta() == 0
                && trace.fallbackCount() == 0
                && trace.suppliedLevelsValidated()
                && trace.mixed().has_value(),
            "mixed stage execution record has the wrong mode boundary");
    const auto& mixed = *trace.mixed();
    require(mixed.n_last_nodes == initial.graph.nodes.size()
                && mixed.n_current_nodes == staged->batch().descriptor->after.node_slots
                && mixed.n_new_nodes > 0
                && mixed.kahn_processed_count
                       == mixed.mixed_vertex_count
                && mixed.reconstructed_level_count
                       == trace.levels().level_offsets.size() - 1,
            "mixed execution record lost its real generation or Kahn boundary");
    const auto before = initial;
    const auto rejected = coordinator.publish(
        std::move(*staged), DualCsrPublishFailpoint::kBeforeSwap);
    require(rejected.code == DualCsrTransactionCode::kPreSwapFault
                && !rejected.published && !rejected.poisoned
                && sameCommittedValues(coordinator.committed(), before),
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
    auto malformed = initial;
    malformed.generation = timing_edit::TransactionGeneration{1};
    const auto before = malformed;
    bool rejected = false;
    try { DualCsrTransactionCoordinator invalid(malformed); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && malformed == before, "malformed generation bypassed the owner boundary");
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
      DualCsrTransactionCoordinator source_commit(source_initial);
      auto advance = captureOwnedBatch(source_session, source_fixture, source_commit);
      auto advance_attempt = makeMixedLevelizedDualCsrAnalysisState(
          source_commit, std::move(advance));
      auto* advanced
          = std::get_if<DualCsrVerifiedStage>(&advance_attempt);
      require(advanced != nullptr,
              "stale exact-before fixture did not produce generation one");
      require(source_commit.publish(std::move(*advanced)).ok(), "source generation publication failed");
      generation_one = source_commit.committed();
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
    DualCsrTransactionCoordinator rebound_owner(std::move(*rebound.rebound));
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        rebound_owner, std::move(stale));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    const DualCsrViolations expected{{
        DualCsrViolationCode::kGeneration,
        "stage.capability",
        "verified batch and exact committed state do not form the next generation"}};
    require(failure != nullptr
                && failure->origin
                       == DualCsrStageFailureOrigin::kCapability
                && failure->violations == expected
                && sameCommittedValues(rebound_owner.committed(), before)
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
    DualCsrTransactionCoordinator coordinator(initial);
    auto verified = captureOwnedBatch(session, fixture, coordinator);
    auto injected_descriptor = verified.descriptor();
    DualCsrStageFactoryFaultAdapter::corruptIndexDelta(
        injected_descriptor);
    const auto expected_attempt = validateDualCsrArcGroupIndexDelta(initial.graph, initial.arc_group_index, injected_descriptor);
    require(!expected_attempt.empty(), "wrong-index input did not fail the real updater/index boundary");
    DualCsrStageFactoryFaultAdapter::corruptIndexDelta(verified);
    const auto before = initial;
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        coordinator, std::move(verified));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    require(
        failure != nullptr && failure->origin == DualCsrStageFailureOrigin::kUpdater && failure->violations == expected_attempt && sameCommittedValues(coordinator.committed(), before),
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
    DualCsrTransactionCoordinator coordinator(initial);
    auto verified = captureOwnedBatch(session, fixture, coordinator);
    auto injected_descriptor = verified.descriptor();
    require(DualCsrStageFactoryFaultAdapter::corruptUpdaterDescriptor(
                injected_descriptor),
            "updater-conflict fixture has no appended node");
    require(validateDualCsrArcGroupIndexDelta(
                initial.graph, initial.arc_group_index,
                injected_descriptor)
                .empty(),
            "updater-conflict input failed before the updater boundary");
    const auto expected_attempt = validateDualCsrDescriptor(initial.graph, injected_descriptor);
    require(!expected_attempt.empty(),
            "updater-conflict input did not fail the real updater");
    require(DualCsrStageFactoryFaultAdapter::corruptUpdaterDescriptor(
                verified),
            "verified updater-conflict fixture has no appended node");
    const auto before = initial;
    auto attempt = makeMixedLevelizedDualCsrAnalysisState(
        coordinator, std::move(verified));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    require(failure != nullptr
                && failure->origin == DualCsrStageFailureOrigin::kUpdater
                && failure->violations == expected_attempt
                && sameCommittedValues(coordinator.committed(), before),
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
    DualCsrTransactionCoordinator coordinator(initial);
    auto verified = captureOwnedBatch(session, fixture, coordinator);
    require(DualCsrStageFactoryFaultAdapter::corruptIntrinsicTimingInput(
                verified, initial.graph),
            "intrinsic fixture lacks an unconstrained output endpoint");
    require(validateDualCsrArcGroupIndexDelta(
                initial.graph, initial.arc_group_index,
                verified.descriptor())
                .empty(),
            "intrinsic input failed before the updater boundary");
    auto expected_graph = initial.graph;
    require(validateDualCsrDescriptor(expected_graph, verified.descriptor()).empty(),
            "finite intrinsic input did not pass the descriptor boundary");
    detail::PreparedDualCsrGraphPatch patch(expected_graph, verified.descriptor());
    patch.apply();
    DualCsrEdgeViewImpl edges(expected_graph);
    DualCsrTimingPayloadViewImpl payloads(expected_graph);
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
        coordinator, std::move(verified));
    const auto* failure = std::get_if<DualCsrStageFailure>(&attempt);
    require(failure != nullptr
                && failure->origin
                       == DualCsrStageFailureOrigin::kTimingIntrinsic
                && failure->violations == expected && sameCommittedValues(coordinator.committed(), before),
            "intrinsic failure was erased, changed, or mutated committed");
    require(session.closeAndRelease().ok(),
            "intrinsic timing session close failed");
  }
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
      verifiedStage(session, fixture, coordinator, &capture));
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
  premature.phase_trace.pop_back();
  require(!finalizeDualCsrExactReceipt(
               coordinator.committed(), *observation.observation,
               fixture.baseline_reference_input,
               std::move(premature))
               .success(),
          "transaction receipt finalized without a committed publish phase");
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
    ista::timing_dual_csr::test::historyAllocationFailure();
    ista::timing_dual_csr::test::verifiedCapabilityAndPublishContracts();
    ista::timing_dual_csr::test::mixedFactoryAndAtomicityContracts();
    ista::timing_dual_csr::test::mixedFactoryFailureInputContracts();
    ista::timing_dual_csr::test::splitReceiptContracts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
