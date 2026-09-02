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
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "DataManager.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrExactReceipt.hpp"
#include "DualCsrStructuredBatchTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
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
  return std::move(*attempt.staged);
}

CommittedDualCsrAnalysisState freshEquivalent(
    const CommittedDualCsrAnalysisState& committed,
    std::uint64_t session)
{
  CommittedDualCsrAnalysisState rebuilt = committed;
  rebuilt.graph.session_token
      = timing_edit::TimingEditSessionToken{session};
  rebuilt.arc_group_index.session_token
      = timing_edit::TimingEditSessionToken{session};
  rebuilt.graph.reference_payload.source_identity.session_token = session;
  rebuilt.timing.values.source_identity.session_token = session;
  ++rebuilt.graph.telemetry.full_rebuild_count;
  for (auto& descriptor : rebuilt.descriptor_trace) {
    descriptor.session_token
        = timing_edit::TimingEditSessionToken{session};
    auto& source = descriptor.reference_payload.source_identity;
    if (source) {
      source->expected.session_token = session;
      source->replacement.session_token = session;
    }
  }
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

  auto rebuilt = freshEquivalent(
      coordinator.committed(), session.token().value() + 3000);
  auto attempt = makeDualCsrVerifiedRecovery(
      coordinator.committed(), std::move(rebuilt));
  require(attempt.success(),
          "semantically equivalent recovery was rejected");
  const auto recovered = coordinator.recover(
      std::move(*attempt.recovery));
  require(recovered.ok() && recovered.published
              && !recovered.poisoned && !coordinator.poisoned(),
          "verified recovery did not clear poison");

  auto healthy_rebuild = freshEquivalent(
      coordinator.committed(), session.token().value() + 4000);
  auto healthy_attempt = makeDualCsrVerifiedRecovery(
      coordinator.committed(), std::move(healthy_rebuild));
  require(healthy_attempt.success(),
          "healthy recovery capability fixture was invalid");
  const auto before = coordinator.committed();
  const auto rejected = coordinator.recover(
      std::move(*healthy_attempt.recovery));
  require(rejected.code
              == DualCsrTransactionCode::kRecoveryNotPoisoned
              && coordinator.committed() == before,
          "healthy coordinator accepted recovery");
  require(session.closeAndRelease().ok(),
          "transaction recovery session close failed");
}

void splitReceiptContracts()
{
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
    ista::timing_dual_csr::test::recoveryContracts();
    ista::timing_dual_csr::test::splitReceiptContracts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
