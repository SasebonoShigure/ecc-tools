// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "CsrSessionSnapshotAdapter.hpp"
#include "CsrSnapshotObservation.hpp"
#include "DataManager.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditCoordinator.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "oracle/IndependentOracleObservation.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::oracle;
using namespace ista::timing_edit::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireOk(TransactionStatus status, const std::string& operation)
{
  require(status.ok(), operation + " returned status "
                           + std::to_string(static_cast<std::uint32_t>(status.code)));
}

void advanceToExecuting(TimingEditCoordinator& coordinator,
                        const PreparedBatch& prepared)
{
  requireOk(coordinator.beginPrepare(prepared), "beginPrepare");
  requireOk(coordinator.markStaged(), "markStaged");
  requireOk(coordinator.markExecuting(), "markExecuting");
  require(coordinator.state() == TimingEditCoordinatorState::kExecuting,
          "coordinator did not reach Executing");
}

std::unique_ptr<TimingEditCoordinator> makeCoordinator(
    GuardedTestSession& session, const PreparedNativeTransaction& transaction,
    std::unique_ptr<csr::CsrCommittedSessionSnapshot> initial_host)
{
  return std::make_unique<TimingEditCoordinator>(
      session.token(),
      StaTimingEditSessionProbeAccess::stateSignal(session.session()),
      transaction.baseline, transaction.initial_identity, CumulativeEditTrace{},
      transaction.resolver, std::move(initial_host));
}

void requireReferenceInputEqual(const csr::CsrReferenceInput& left,
                                const csr::CsrReferenceInput& right)
{
#define REQUIRE_INPUT_FIELD(FIELD)                                                \
  require(left.FIELD == right.FIELD, "host CSR input differs: " #FIELD)
  REQUIRE_INPUT_FIELD(schema_version);
  REQUIRE_INPUT_FIELD(source_identity);
  REQUIRE_INPUT_FIELD(analysis_context);
  REQUIRE_INPUT_FIELD(analysis_order);
  REQUIRE_INPUT_FIELD(path_source_order);
  REQUIRE_INPUT_FIELD(transition_order);
  REQUIRE_INPUT_FIELD(nodes);
  REQUIRE_INPUT_FIELD(node_ids_by_key);
  REQUIRE_INPUT_FIELD(instances);
  REQUIRE_INPUT_FIELD(arcs);
  REQUIRE_INPUT_FIELD(arc_ids_by_key);
  REQUIRE_INPUT_FIELD(out_row_offsets);
  REQUIRE_INPUT_FIELD(out_arc_ids);
  REQUIRE_INPUT_FIELD(in_row_offsets);
  REQUIRE_INPUT_FIELD(in_arc_ids);
  REQUIRE_INPUT_FIELD(ordered_root_ids);
  REQUIRE_INPUT_FIELD(ordered_start_ids);
  REQUIRE_INPUT_FIELD(ordered_end_ids);
  REQUIRE_INPUT_FIELD(clock_tags);
  REQUIRE_INPUT_FIELD(clocks);
  REQUIRE_INPUT_FIELD(clock_owner_by_node);
  REQUIRE_INPUT_FIELD(port_constraints);
  REQUIRE_INPUT_FIELD(case_analysis_constraints);
  REQUIRE_INPUT_FIELD(timing_check_endpoints);
  REQUIRE_INPUT_FIELD(referenced_library_cells);
  REQUIRE_INPUT_FIELD(parasitics);
  REQUIRE_INPUT_FIELD(start_seeds);
  REQUIRE_INPUT_FIELD(endpoint_required);
  REQUIRE_INPUT_FIELD(cell_pin_capacitances);
  REQUIRE_INPUT_FIELD(unsupported_reasons);
#undef REQUIRE_INPUT_FIELD
}

void requireObservationStructureEqual(
    const std::unique_ptr<csr::CsrSnapshotObservation>& left,
    const std::unique_ptr<csr::CsrSnapshotObservation>& right,
    const std::string& field)
{
  require(static_cast<bool>(left) == static_cast<bool>(right),
          field + " presence differs");
  if (left == nullptr) {
    return;
  }
  require(left->schema_version == right->schema_version,
          field + ".schema_version differs");
  require(left->state == right->state, field + ".state differs");
  require(left->identity == right->identity, field + ".identity differs");
  require(left->counts == right->counts, field + ".counts differs");
  require(left->buffers == right->buffers, field + ".buffers differs");
  require(left->capacity == right->capacity, field + ".capacity differs");
  require(left->support == right->support, field + ".support differs");
  require(left->host_bytes == right->host_bytes,
          field + ".host_bytes differs");
  require(left->last_rejection == right->last_rejection,
          field + ".last_rejection differs");
  require(left->last_invalidation == right->last_invalidation,
          field + ".last_invalidation differs");
  require(left->timing_ms.reference_ms.has_value()
              == right->timing_ms.reference_ms.has_value(),
          field + ".timing_ms.reference presence differs");
  require(left->timing_ms.scatter_ms.has_value()
              == right->timing_ms.scatter_ms.has_value(),
          field + ".timing_ms.scatter presence differs");
  // Wall-clock durations are intentionally not semantic recovery state.
}

void requireHostSnapshotEqual(
    const csr::CsrCommittedSessionSnapshot& replayed,
    const csr::CsrCommittedSessionSnapshot& recovered)
{
  requireReferenceInputEqual(replayed.input, recovered.input);
  require(replayed.metadata.level_offsets == recovered.metadata.level_offsets,
          "host CSR metadata.level_offsets differs");
  require(replayed.metadata.level_nodes == recovered.metadata.level_nodes,
          "host CSR metadata.level_nodes differs");
  require(replayed.metadata.node_to_level == recovered.metadata.node_to_level,
          "host CSR metadata.node_to_level differs");
  requireObservationStructureEqual(replayed.active_observation,
                                   recovered.active_observation,
                                   "host CSR active_observation");
  requireObservationStructureEqual(replayed.released_observation,
                                   recovered.released_observation,
                                   "host CSR released_observation");
}

csr::CsrTimingResult currentCsrFull(TimingEditSessionToken session_token)
{
  auto snapshot = buildNativeHostSnapshot(STADM.getDatabase(), session_token);
  csr::CsrComputationResult computation
      = csr::computeCsrSessionReference(*snapshot);
  const auto* result = std::get_if<csr::CsrTimingResult>(&computation.outcome);
  require(result != nullptr,
          "current Database did not produce a supported CSR full result");
  return *result;
}

void requireCommittedRawClosure(const TimingEditCoordinator& coordinator,
                                TimingEditSessionToken session_token)
{
  require(coordinator.hostSnapshot() != nullptr,
          "committed coordinator has no host CSR snapshot");
  const csr::CsrReferenceInput& input = coordinator.hostSnapshot()->input;
  NativeTimingEditPreparer::requireExactResolverCoverage(
      STADM.getDatabase(), input, coordinator.resolverState());
  NativeTimingEditPreparer::requireExactCommittedTraceClosure(
      STADM.getDatabase(), input, coordinator.resolverState(),
      coordinator.baseline(), coordinator.cumulativeTrace().batches);
  const CommittedEditIdentity actual
      = NativeTimingEditPreparer::captureCurrentIdentity(
          STADM.getDatabase(), coordinator.committedIdentity().generation,
          editTestAnalysisContext(), session_token);
  require(actual == coordinator.committedIdentity(),
          "committed raw Database identity differs from coordinator");
}

void requireMaterialized(const TokenMaterialization& materialization)
{
  const Database& database = STADM.getDatabase();
  require(database.get_instance_map().contains(materialization.instance_name),
          "replayed buffer instance is absent");
  require(database.get_net_map().contains(materialization.output_net_name),
          "replayed buffer output net is absent");
  require(database.get_pin_map().contains(materialization.input_pin_name)
              && database.get_pin_map().contains(
                  materialization.output_pin_name),
          "replayed buffer pins are absent");
}

void testFreshRecoveryReplaysDurableSemanticTrace()
{
  installNativeTransactionBaseline();
  GuardedTestSession old_session;
  PreparedNativeTransaction original_transaction
      = prepareNativeTransaction(old_session.token());
  auto original_initial_host
      = buildNativeHostSnapshot(STADM.getDatabase(), old_session.token());
  auto original = makeCoordinator(old_session, original_transaction,
                                  std::move(original_initial_host));
  advanceToExecuting(*original, original_transaction.prepared);

  TransactionStatus poison_status;
  const StaStatus poisoned_guard
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          old_session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            require(token == old_session.token().value(),
                    "old guard supplied the wrong session token");
            ExecutedNativeTransaction executed = executeNativeTransaction(
                authority, context, original_transaction);
            requireOk(original->sealVerified(original_transaction.prepared,
                                             executed.post_identity),
                      "seal poisoned g+1");
            CumulativeEditTrace trace
                = nativeSuccessorTrace(original_transaction);
            VerifiedPublishBundle bundle(
                std::move(executed.installed),
                std::move(executed.host_snapshot),
                std::move(executed.csr_full_receipt),
                std::move(original_transaction.prepared), std::move(trace),
                std::move(executed.staged_resolver),
                CommitFaultLatch{CommitLatchOrdinal::kDeviceState});
            poison_status = original->publish(authority, std::move(bundle));
            return StaStatus::success(old_session.session().state());
          });
  require(poisoned_guard.ok(),
          "poisoned g+1 guard failed: " + poisoned_guard.message);
  require(poison_status.code == TransactionStatusCode::kPoisonedCommit
              && original->state() == TimingEditCoordinatorState::kPoisoned
              && old_session.session().state() == StaSessionState::kPoisoned
              && original->committedIdentity().generation
                     == TransactionGeneration{1}
              && original->cumulativeTrace().batches.size() == 1,
          "original coordinator did not retain a complete poisoned g+1");

  const BaselineSemanticIdentity durable_baseline = original->baseline();
  const CommittedEditIdentity durable_identity = original->committedIdentity();
  const CumulativeEditTrace durable_trace = original->cumulativeTrace();
  const TimingEditResolverState durable_resolver = original->resolverState();
  const RawDesignObservation durable_raw
      = captureRawDesign(STADM.getDatabase());

  require(old_session.closeAndRelease().ok()
              && old_session.session().state() == StaSessionState::kClosed,
          "old poisoned session did not close");
  requireOk(original->acknowledgePoisonedSessionClosed(old_session.token()),
            "acknowledge old poisoned session close");
  require(original->state() == TimingEditCoordinatorState::kRecovering,
          "original coordinator did not enter Recovering");

  // Recovery starts from an independently materialized native g0, not the
  // poisoned process's already-edited Database.
  installNativeTransactionBaseline();
  GuardedTestSession fresh_session;
  TransactionStatus recovery_status;
  bool compared_complete_recovery = false;
  const StaStatus recovery_guard
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          fresh_session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            require(token == fresh_session.token().value()
                        && authority.sessionToken() == fresh_session.token(),
                    "fresh guard supplied the wrong session authority");

            Database& database = STADM.getDatabase();
            TimingEditResolverState g0_resolver
                = TimingEditResolverState::bindBaseline(database);
            const CommittedEditIdentity g0_identity
                = NativeTimingEditPreparer::captureCurrentIdentity(
                    database, TransactionGeneration{0},
                    editTestAnalysisContext(), fresh_session.token());
            require(g0_identity.current_fingerprints
                            == durable_baseline.fingerprints
                        && g0_identity.canonical_pre_state
                               == durable_baseline.canonical_pre_state,
                    "fresh native baseline differs structurally from durable g0");

            DesignEditPlan semantic_plan = durable_trace.batches.front();
            const TimingEditPrepareContext prepare_context{
                fresh_session.token(), durable_baseline, g0_identity,
                editTestAnalysisContext()};
            PreparedBatch prepared = NativeTimingEditPreparer::prepare(
                database, semantic_plan, prepare_context, g0_resolver);
            auto replay_initial_host
                = buildNativeHostSnapshot(database, fresh_session.token());
            PreparedNativeTransaction replay_transaction{
                durable_baseline, g0_identity, std::move(g0_resolver),
                std::move(semantic_plan), std::move(prepared)};
            TimingEditCoordinator replay_coordinator(
                fresh_session.token(),
                StaTimingEditSessionProbeAccess::stateSignal(
                    fresh_session.session()),
                durable_baseline, g0_identity, CumulativeEditTrace{},
                replay_transaction.resolver, std::move(replay_initial_host));
            advanceToExecuting(replay_coordinator,
                               replay_transaction.prepared);
            ExecutedNativeTransaction replayed = executeNativeTransaction(
                authority, context, replay_transaction);
            requireOk(replay_coordinator.sealVerified(
                          replay_transaction.prepared,
                          replayed.post_identity),
                      "seal independently replayed g+1");
            CumulativeEditTrace replay_trace
                = nativeSuccessorTrace(replay_transaction);
            require(replay_trace == durable_trace,
                    "normalized independent replay trace differs from durable trace");
            const TokenMaterialization replay_materialization
                = replayed.materialization;
            VerifiedPublishBundle replay_bundle(
                std::move(replayed.installed),
                std::move(replayed.host_snapshot),
                std::move(replayed.csr_full_receipt),
                std::move(replay_transaction.prepared),
                CumulativeEditTrace{replay_trace},
                std::move(replayed.staged_resolver));
            requireOk(replay_coordinator.publish(authority,
                                                 std::move(replay_bundle)),
                      "publish independently replayed g+1");
            require(replay_coordinator.state()
                            == TimingEditCoordinatorState::kIdle
                        && replay_coordinator.committedIdentity()
                               == durable_identity
                        && replay_coordinator.cumulativeTrace()
                               == durable_trace
                        && replay_coordinator.resolverState()
                               == durable_resolver,
                    "independent replay did not reconstruct durable g+1");
            requireMaterialized(replay_materialization);
            requireCommittedRawClosure(replay_coordinator,
                                       fresh_session.token());

            const RawDesignObservation replay_raw
                = captureRawDesign(STADM.getDatabase());
            require(compareRawDesign(durable_raw, replay_raw).empty(),
                    "fresh replay raw Database differs from poisoned committed g+1");
            const csr::CsrTimingResult replay_timing
                = currentCsrFull(fresh_session.token());
            auto recovery_host = buildNativeHostSnapshot(
                STADM.getDatabase(), fresh_session.token());
            CsrExecutionAuthorization recovery_receipt = StaTimingEditSessionProbeAccess::proveCsrFullExact(authority, *recovery_host, STADM.getDatabase(),
                                                                                                            replay_coordinator.committedIdentity().generation);
            CommittedEditIdentity recovery_identity
                = replay_coordinator.committedIdentity();
            CumulativeEditTrace recovery_trace
                = replay_coordinator.cumulativeTrace();
            TimingEditResolverState recovery_resolver
                = replay_coordinator.resolverState();
            FreshRecoveryBundle recovery_bundle(
                fresh_session.token(), std::move(recovery_host),
                std::move(recovery_receipt), std::move(recovery_identity),
                std::move(recovery_trace), std::move(recovery_resolver),
                StaTimingEditSessionProbeAccess::stateSignal(
                    fresh_session.session()));
            recovery_status = original->installFreshRecovery(
                authority, std::move(recovery_bundle));

            require(recovery_status.ok()
                        && original->state()
                               == TimingEditCoordinatorState::kIdle
                        && original->sessionToken() == fresh_session.token()
                        && original->committedIdentity()
                               == replay_coordinator.committedIdentity()
                        && original->cumulativeTrace()
                               == replay_coordinator.cumulativeTrace()
                        && original->resolverState()
                               == replay_coordinator.resolverState()
                        && original->deviceState()
                               == replay_coordinator.deviceState()
                        && original->commitMarker()
                               == replay_coordinator.commitMarker(),
                    "recovered coordinator sidecars differ from independent replay");
            requireHostSnapshotEqual(*replay_coordinator.hostSnapshot(),
                                     *original->hostSnapshot());
            require(currentCsrFull(fresh_session.token()) == replay_timing,
                    "recovered full CSR result differs from independent replay");
            const RawDesignObservation recovered_raw
                = captureRawDesign(STADM.getDatabase());
            require(compareRawDesign(replay_raw, recovered_raw).empty(),
                    "fresh recovery changed the independently replayed raw Database");
            requireCommittedRawClosure(*original, fresh_session.token());
            requireCommittedRawClosure(replay_coordinator,
                                       fresh_session.token());
            compared_complete_recovery = true;
            return StaStatus::success(fresh_session.session().state());
          });

  require(recovery_guard.ok(),
          "fresh recovery guard failed: " + recovery_guard.message);
  require(recovery_status.ok() && compared_complete_recovery
              && fresh_session.session().state()
                     == StaSessionState::kBaselineExecuted,
          "fresh recovery did not complete all structural comparisons");
  require(fresh_session.closeAndRelease().ok(),
          "fresh recovery session close failed");
}

}  // namespace

int main()
{
  NativeTransactionRuntime runtime;
  testFreshRecoveryReplaysDurableSemanticTrace();
  return 0;
}
