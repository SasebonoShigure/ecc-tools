// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "DataManager.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "STAInterface.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditCoordinator.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingEditValidation.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

static_assert(!std::is_constructible_v<NativeExecutionAuthority,
                                       TimingEditSessionToken>);
static_assert(!std::is_copy_constructible_v<NativeExecutionAuthority>);
static_assert(!std::is_move_constructible_v<NativeExecutionAuthority>);

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

csr::NodeKey port(std::string name)
{
  return csr::NodeKey{csr::NodeKind::kPort, std::move(name), {}, {}};
}

csr::NodeKey pin(std::string instance, std::string name)
{
  return csr::NodeKey{csr::NodeKind::kInstancePin, instance + ":" + name,
                      std::move(instance), std::move(name)};
}

csr::LibraryArcSignature signature()
{
  csr::LibraryArcSignature value;
  value.library_name = "fixture.lib";
  value.arc_index = 0;
  value.sense = csr::TimingSense::kPositiveUnate;
  value.trigger_transition = csr::Transition::kNone;
  value.check_transition = csr::Transition::kNone;
  return value;
}

LibraryCellRef bufferCell()
{
  return LibraryCellRef{"fixture.lib", "BUF_X1", "A", "Y", {signature()}};
}

ComponentFingerprints fingerprints(char value)
{
  const std::string digest(64, value);
  return ComponentFingerprints{csr::kCsrSchemaVersion, digest, digest, digest,
                               digest, digest, digest};
}

struct ContractFixture
{
  csr::NodeKey driver{port("in")};
  csr::NodeKey sink{pin("u1", "A")};
  csr::ArcKey old_arc;
  BaselineSemanticIdentity baseline;
  CreationToken creation_token;
  BufferInsertionMutation edit;

  ContractFixture()
  {
    old_arc.base.source = driver;
    old_arc.base.sink = sink;
    old_arc.base.kind = csr::ArcKind::kNet;
    old_arc.base.owner_name = "n0";
    baseline.design_name = "transaction_fixture";
    baseline.fingerprints = fingerprints('a');
    baseline.canonical_pre_state.instances = {"u1"};
    baseline.canonical_pre_state.nets = {"n0"};
    baseline.canonical_pre_state.nodes
        = csr::sortedUniqueNodeKeys({driver, sink});
    baseline.canonical_pre_state.arcs = {old_arc};
    creation_token = CreationToken{1, "case", "profile", 42, 1, 0};
    edit = BufferInsertionMutation{ExistingNetRef{"n0"},
                                   {ExistingNodeRef{sink}},
                                   bufferCell(),
                                   UnplacedInputs{},
                                   NoParasiticRcInputs{},
                                   creation_token};
  }
};

TimingGraphDelta normalized(TimingGraphDelta value)
{
  TimingGraphDeltaNormalization result
      = normalizeTimingGraphDelta(std::move(value));
  if (const auto* errors = std::get_if<TimingEditContractViolations>(&result)) {
    throw std::logic_error("transaction fixture normalization failed: "
                           + errors->front().message);
  }
  return std::get<TimingGraphDelta>(std::move(result));
}

PreparedBatch makePreparedBatch(const ContractFixture& fixture,
                                TimingEditSessionToken session_token)
{
  const StableNodeRef sink_ref{ExistingNodeRef{fixture.sink}};
  const StableNetRef old_net{ExistingNetRef{"n0"}};
  const StableNetRef new_net{
      CreatedNetRef{fixture.creation_token, CreatedNetRole::kOutputNet}};
  const StableArcRef old_arc{ExistingArcRef{fixture.old_arc}};
  const StableArcRef new_arc{CreatedNewNetArcRef{
      fixture.creation_token,
      StableNodeRef{
          CreatedNodeRef{fixture.creation_token, CreatedNodeRole::kOutput}},
      sink_ref,
      0}};

  TimingGraphDelta forward;
  forward.operations = {ReconnectSink{sink_ref, old_net, new_net},
                        RemoveNetArc{old_arc}, AddNetArc{new_arc}};
  TimingGraphDelta reverse;
  reverse.operations = {ReconnectSink{sink_ref, new_net, old_net},
                        RemoveNetArc{new_arc}, AddNetArc{old_arc}};

  PreparedBatchData data;
  data.session_token = session_token;
  data.expected_generation = TransactionGeneration{0};
  data.target_generation = TransactionGeneration{1};
  data.baseline = fixture.baseline;
  data.expected_pre_committed_identity = CommittedEditIdentity{
      TransactionGeneration{0}, fixture.baseline.fingerprints,
      fixture.baseline.canonical_pre_state};
  data.normalized_edits = {fixture.edit};
  data.normalized_forward = normalized(std::move(forward));
  data.normalized_reverse = normalized(std::move(reverse));
  data.token_materializations = {
      defaultTokenMaterialization(fixture.creation_token,
                                  fixture.edit.library_cell)};
  data.policy_receipt = PolicyReceipt{EditDisposition::kSupported, {}};
  return PreparedBatch::create(std::move(data));
}

CommittedEditIdentity initialIdentity(const ContractFixture& fixture)
{
  return CommittedEditIdentity{TransactionGeneration{0},
                               fixture.baseline.fingerprints,
                               fixture.baseline.canonical_pre_state};
}

struct SessionSignalRecorder
{
  SessionTransactionState state{SessionTransactionState::kBaselineExecuted};
  std::uint64_t writes{0};
};

void writeRecordedSessionState(void* context,
                               SessionTransactionState state) noexcept
{
  auto& recorder = *static_cast<SessionSignalRecorder*>(context);
  recorder.state = state;
  ++recorder.writes;
}

SessionStateSignal signalFor(SessionSignalRecorder& recorder)
{
  return SessionStateSignal{&recorder, &writeRecordedSessionState};
}

struct RollbackRecorder
{
  bool result{true};
  std::uint64_t calls{0};
};

bool invokeRollback(void* context) noexcept
{
  auto& recorder = *static_cast<RollbackRecorder*>(context);
  ++recorder.calls;
  return recorder.result;
}

void advanceToStaged(TimingEditCoordinator& coordinator,
                     const PreparedBatch& batch)
{
  require(coordinator.beginPrepare(batch).ok(),
          "coordinator did not enter Prepared");
  require(coordinator.state() == TimingEditCoordinatorState::kPrepared,
          "Prepared state was not visible");
  require(coordinator.markStaged().ok()
              && coordinator.state() == TimingEditCoordinatorState::kStaged,
          "coordinator did not enter Staged");
}

void advanceToExecuting(TimingEditCoordinator& coordinator,
                        const PreparedBatch& batch)
{
  advanceToStaged(coordinator, batch);
  require(coordinator.markExecuting().ok()
              && coordinator.state() == TimingEditCoordinatorState::kExecuting,
          "coordinator did not enter Executing");
}

void testStateIdentityAndPrecommitRollbackSignals()
{
  const ContractFixture fixture;
  constexpr TimingEditSessionToken kToken{41};
  for (const bool rollback_result : {true, false}) {
    SessionSignalRecorder session;
    PreparedBatch batch = makePreparedBatch(fixture, kToken);
    TimingEditCoordinator coordinator(kToken, signalFor(session), fixture.baseline,
                                      initialIdentity(fixture), {}, {});
    require(coordinator.state() == TimingEditCoordinatorState::kIdle
                && coordinator.committedIdentity() == initialIdentity(fixture)
                && coordinator.deviceState().generation
                       == TransactionGeneration{0}
                && coordinator.commitMarker().generation
                       == TransactionGeneration{0},
            "coordinator initial g state is inconsistent");
    advanceToStaged(coordinator, batch);

    RollbackRecorder rollback{rollback_result, 0};
    const TransactionStatus status = coordinator.rollbackPrecommit(
        PrecommitRollbackSignal{&rollback, &invokeRollback});
    require(rollback.calls == 1,
            "precommit rollback signal was not called exactly once");
    require(coordinator.committedIdentity() == initialIdentity(fixture)
                && coordinator.cumulativeTrace().batches.empty()
                && coordinator.deviceState().generation
                       == TransactionGeneration{0},
            "precommit rollback changed committed g sidecars");
    if (rollback_result) {
      require(status.ok()
                  && coordinator.state() == TimingEditCoordinatorState::kIdle
                  && session.state
                         == SessionTransactionState::kBaselineExecuted
                  && session.writes == 0,
              "successful precommit rollback did not restore exact Idle(g)");
    } else {
      require(status.code == TransactionStatusCode::kRollbackFailed
                  && coordinator.state()
                         == TimingEditCoordinatorState::kPoisoned
                  && session.state == SessionTransactionState::kPoisoned
                  && session.writes == 1,
              "failed precommit rollback did not poison coordinator/session");
    }
  }
}

std::unique_ptr<TimingEditCoordinator> makeNativeCoordinator(
    GuardedTestSession& session, const PreparedNativeTransaction& transaction,
    std::unique_ptr<csr::CsrCommittedSessionSnapshot> initial_host)
{
  return std::make_unique<TimingEditCoordinator>(
      session.token(),
      StaTimingEditSessionProbeAccess::stateSignal(session.session()),
      transaction.baseline, transaction.initial_identity, CumulativeEditTrace{},
      transaction.resolver, std::move(initial_host));
}

void requireMaterialization(const TokenMaterialization& materialization,
                            bool expected)
{
  const Database& database = STADM.getDatabase();
  require(database.get_instance_map().contains(materialization.instance_name)
              == expected,
          "materialized instance membership is wrong");
  require(database.get_net_map().contains(materialization.output_net_name)
              == expected,
          "materialized output net membership is wrong");
  require(database.get_pin_map().contains(materialization.input_pin_name)
              == expected
              && database.get_pin_map().contains(
                     materialization.output_pin_name)
                     == expected,
          "materialized pin membership is wrong");
}

void requireExactNativeG0(const TimingEditCoordinator& coordinator,
                          const PreparedNativeTransaction& transaction,
                          const csr::CsrCommittedSessionSnapshot* initial_host,
                          const TokenMaterialization& materialization)
{
  const CommittedEditIdentity current
      = NativeTimingEditPreparer::captureCurrentIdentity(
          STADM.getDatabase(), TransactionGeneration{0},
          editTestAnalysisContext(), transaction.prepared.sessionToken());
  require(coordinator.state() == TimingEditCoordinatorState::kIdle
              && coordinator.committedIdentity() == transaction.initial_identity
              && coordinator.cumulativeTrace().batches.empty()
              && coordinator.resolverState() == transaction.resolver
              && coordinator.hostSnapshot() == initial_host
              && coordinator.deviceState().generation
                     == TransactionGeneration{0}
              && coordinator.commitMarker().generation
                     == TransactionGeneration{0}
              && current == transaction.initial_identity,
          "rejected native transaction did not restore exact g");
  requireMaterialization(materialization, false);
}

struct ObserverOutcome
{
  StaStatus status;
  bool observation_present{false};
};

void testPublishBeforeCommitRollsBackExactG()
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  PreparedNativeTransaction transaction
      = prepareNativeTransaction(session.token());
  const TimingEditResolverState staged
      = transaction.resolver.stage(transaction.prepared);
  const TokenMaterialization materialization = staged.materialization(
      transaction.plan.edits.front().creation_token);
  auto initial_host
      = buildNativeHostSnapshot(STADM.getDatabase(), session.token());
  const csr::CsrCommittedSessionSnapshot* initial_host_address
      = initial_host.get();
  auto coordinator
      = makeNativeCoordinator(session, transaction, std::move(initial_host));
  advanceToExecuting(*coordinator, transaction.prepared);
  StaCsrReferenceAttempt reference_attempt
      = session.session().computeCsrReferenceForTesting();
  const auto* computation
      = std::get_if<csr::CsrComputationResult>(&reference_attempt.value);
  require(computation != nullptr,
          "baseline session did not compute a CSR reference result");
  const auto* old_result
      = std::get_if<csr::CsrTimingResult>(&computation->outcome);
  require(old_result != nullptr,
          "baseline session CSR reference was not a timing result");

  std::promise<void> observer_started_signal;
  std::future<void> observer_started
      = observer_started_signal.get_future();
  std::promise<ObserverOutcome> observer_done_signal;
  std::future<ObserverOutcome> observer_done
      = observer_done_signal.get_future();
  std::thread observer;
  bool observer_blocked_until_guard_release = false;
  StaCsrScatterAttempt stale_scatter;
  bool stale_scatter_checked = false;

  TransactionStatus publish_status;
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            require(token == session.token().value()
                        && authority.sessionToken() == session.token(),
                    "guard supplied the wrong native authority");
            requireRecursiveStaOperationRejected(token, context);
            ExecutedNativeTransaction executed
                = executeNativeTransaction(authority, context, transaction);
            StaCsrSessionContext* const context_address = &context;
            observer = std::thread(
                [&, context_address, token] {
                  observer_started_signal.set_value();
                  std::unique_ptr<csr::CsrSnapshotObservation> observation;
                  const StaStatus status
                      = STAInterface::getInst()
                            .copySessionCsrSnapshotObservation(
                                token, context_address, &observation);
                  observer_done_signal.set_value(
                      ObserverOutcome{status, observation != nullptr});
                });
            observer_started.wait();
            observer_blocked_until_guard_release
                = observer_done.wait_for(std::chrono::milliseconds(100))
                  == std::future_status::timeout;
            stale_scatter = StaTimingEditSessionProbeAccess::scatterForTesting(
                context, STADM.getDatabase(), *old_result);
            stale_scatter_checked = true;
            require(coordinator
                        ->sealVerified(transaction.prepared,
                                       executed.post_identity)
                        .ok(),
                    "real batch did not seal before precommit fault");
            CumulativeEditTrace trace = nativeSuccessorTrace(transaction);
            VerifiedPublishBundle bundle(
                std::move(executed.installed),
                std::move(executed.host_snapshot),
                std::move(executed.csr_full_receipt),
                std::move(transaction.prepared), std::move(trace),
                std::move(executed.staged_resolver));
            publish_status = coordinator->publish(
                authority, std::move(bundle),
                PrecommitFailpoint::kPublishBeforeCommit);
            return StaStatus::success(session.session().state());
          });
  if (observer.joinable()) {
    observer.join();
  }
  const ObserverOutcome observed = observer_done.get();
  require(guarded.ok()
              && publish_status.code
                     == TransactionStatusCode::kPrecommitFault,
          "real publish-before-commit fault did not reject under one guard: "
              + guarded.message);
  require(observer_blocked_until_guard_release
              && observed.status.code == StaSessionStatusCode::kInvalidState
              && !observed.observation_present,
          "concurrent observer did not block until guard release or observed candidate state");
  require(stale_scatter_checked
              && stale_scatter.value.scatter_result.code
                     == csr::CsrScatterCode::kInvalidState
              && stale_scatter.value.scatter_result.writes_applied == 0,
          "released pre-mutation CSR snapshot remained scatterable");
  requireExactNativeG0(*coordinator, transaction, initial_host_address,
                       materialization);
  require(session.session().state() == StaSessionState::kBaselineExecuted,
          "successful native rollback changed the session state");
  require(session.closeAndRelease().ok(),
          "precommit rollback session close failed");
}

enum class PublishMismatch : std::uint8_t
{
  kOldSnapshot,
  kForgedPostIdentity,
  kUnstagedResolver
};

TransactionStatusCode expectedCode(PublishMismatch mismatch)
{
  switch (mismatch) {
    case PublishMismatch::kOldSnapshot:
      return TransactionStatusCode::kHostSnapshotMismatch;
    case PublishMismatch::kForgedPostIdentity:
      return TransactionStatusCode::kInvalidPostIdentity;
    case PublishMismatch::kUnstagedResolver:
      return TransactionStatusCode::kResolverMismatch;
  }
  throw std::logic_error("unknown publish mismatch");
}

void runPublishMismatch(PublishMismatch mismatch)
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  PreparedNativeTransaction transaction
      = prepareNativeTransaction(session.token());
  const TimingEditResolverState expected_staged
      = transaction.resolver.stage(transaction.prepared);
  const TokenMaterialization materialization = expected_staged.materialization(
      transaction.plan.edits.front().creation_token);
  auto initial_host
      = buildNativeHostSnapshot(STADM.getDatabase(), session.token());
  auto old_candidate_host
      = buildNativeHostSnapshot(STADM.getDatabase(), session.token());
  const csr::CsrCommittedSessionSnapshot* initial_host_address
      = initial_host.get();
  auto coordinator
      = makeNativeCoordinator(session, transaction, std::move(initial_host));
  advanceToExecuting(*coordinator, transaction.prepared);

  TransactionStatus publish_status;
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            requireRecursiveStaOperationRejected(token, context);
            ExecutedNativeTransaction executed
                = executeNativeTransaction(authority, context, transaction);
            CommittedEditIdentity sealed_identity = executed.post_identity;
            if (mismatch == PublishMismatch::kForgedPostIdentity) {
              char& first = sealed_identity.current_fingerprints.topology.front();
              first = first == '0' ? '1' : '0';
            }
            require(coordinator
                        ->sealVerified(transaction.prepared,
                                       std::move(sealed_identity))
                        .ok(),
                    "mismatch fixture did not reach Verified");

            auto host = mismatch == PublishMismatch::kOldSnapshot
                            ? std::move(old_candidate_host)
                            : std::move(executed.host_snapshot);
            TimingEditResolverState resolver
                = mismatch == PublishMismatch::kUnstagedResolver
                      ? transaction.resolver
                      : std::move(executed.staged_resolver);
            CumulativeEditTrace trace = nativeSuccessorTrace(transaction);
            VerifiedPublishBundle bundle(
                std::move(executed.installed), std::move(host),
                std::move(executed.csr_full_receipt),
                std::move(transaction.prepared), std::move(trace),
                std::move(resolver));
            publish_status
                = coordinator->publish(authority, std::move(bundle));
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok() && publish_status.code == expectedCode(mismatch),
          "publish integrity mismatch returned the wrong typed status: "
              + guarded.message);
  requireExactNativeG0(*coordinator, transaction, initial_host_address,
                       materialization);
  require(session.closeAndRelease().ok(),
          "publish mismatch session close failed");
}

void testPublishIntegrityMismatchesRollbackExactG()
{
  for (const PublishMismatch mismatch : {
           PublishMismatch::kOldSnapshot,
           PublishMismatch::kForgedPostIdentity,
           PublishMismatch::kUnstagedResolver}) {
    runPublishMismatch(mismatch);
  }
}

void requireCommittedG1(const TimingEditCoordinator& coordinator,
                        const CommittedEditIdentity& post_identity,
                        const CumulativeEditTrace& trace,
                        const TimingEditResolverState& resolver,
                        const csr::CsrCommittedSessionSnapshot* host,
                        const TokenMaterialization& materialization)
{
  require(coordinator.state() == TimingEditCoordinatorState::kPoisoned
              && coordinator.committedIdentity() == post_identity
              && coordinator.cumulativeTrace() == trace
              && coordinator.resolverState() == resolver
              && coordinator.hostSnapshot() != nullptr
              && coordinator.hostSnapshot() != host
              && coordinator.deviceState()
                     == DeviceCsrState{
                         TransactionGeneration{1},
                         DeviceCsrMaterializationState::kNotMaterialized,
                         DeviceCsrUnavailableReason::kStageNotImplemented}
              && coordinator.commitMarker()
                     == CommitMarker{TransactionGeneration{1},
                                     CommitMarkerState::kComplete},
          "commit fault did not finish a complete poisoned g+1");
  requireMaterialization(materialization, true);
}

void runPoisonedCommit(CommitFaultLatch latch,
                       bool publish_after_commit_fault)
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  PreparedNativeTransaction transaction
      = prepareNativeTransaction(session.token());
  auto initial_host
      = buildNativeHostSnapshot(STADM.getDatabase(), session.token());
  auto coordinator
      = makeNativeCoordinator(session, transaction, std::move(initial_host));
  advanceToExecuting(*coordinator, transaction.prepared);

  TransactionStatus publish_status;
  CommittedEditIdentity expected_post;
  CumulativeEditTrace expected_trace;
  TimingEditResolverState expected_resolver;
  TokenMaterialization materialization;
  const csr::CsrCommittedSessionSnapshot* expected_host{nullptr};
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            requireRecursiveStaOperationRejected(token, context);
            ExecutedNativeTransaction executed
                = executeNativeTransaction(authority, context, transaction);
            expected_post = executed.post_identity;
            expected_trace = nativeSuccessorTrace(transaction);
            expected_resolver = executed.staged_resolver;
            materialization = executed.materialization;
            expected_host = executed.host_snapshot.get();
            require(coordinator
                        ->sealVerified(transaction.prepared, expected_post)
                        .ok(),
                    "commit fixture did not reach Verified");
            VerifiedPublishBundle bundle(
                std::move(executed.installed),
                std::move(executed.host_snapshot),
                std::move(executed.csr_full_receipt),
                std::move(transaction.prepared),
                CumulativeEditTrace{expected_trace},
                TimingEditResolverState{expected_resolver}, latch,
                publish_after_commit_fault);
            publish_status
                = coordinator->publish(authority, std::move(bundle));
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok()
              && publish_status.code
                     == TransactionStatusCode::kPoisonedCommit
              && session.session().state() == StaSessionState::kPoisoned,
          "poisoned commit did not finish inside one guard: "
              + guarded.message);
  requireCommittedG1(*coordinator, expected_post, expected_trace,
                     expected_resolver, expected_host, materialization);

  require(session.closeAndRelease().ok()
              && session.session().state() == StaSessionState::kClosed,
          "poisoned old session did not close");
}

void testAllCommitLatchesFinishGPlusOneThenPoison()
{
  constexpr std::array<CommitLatchOrdinal, 4> kOrdinals{
      CommitLatchOrdinal::kDatabaseCanonical,
      CommitLatchOrdinal::kHostSnapshot,
      CommitLatchOrdinal::kTransactionSidecars,
      CommitLatchOrdinal::kDeviceState};
  for (std::size_t index = 0; index < kOrdinals.size(); ++index) {
    runPoisonedCommit(CommitFaultLatch{kOrdinals[index]},
                      false);
  }
  runPoisonedCommit({}, true);
}


}  // namespace

namespace ista::timing_edit::test {
void testTimingEditRuntimeContract();
}

int main()
{
  NativeTransactionRuntime runtime;
  ista::timing_edit::test::testTimingEditRuntimeContract();
  testStateIdentityAndPrecommitRollbackSignals();
  if constexpr (!ista::incregpusta_debug_checks_enabled)
    return 0;
  testPublishBeforeCommitRollsBackExactG();
  testPublishIntegrityMismatchesRollbackExactG();
  testAllCommitLatchesFinishGPlusOneThenPoison();


  return 0;
}
