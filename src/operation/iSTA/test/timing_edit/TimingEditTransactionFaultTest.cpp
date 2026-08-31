// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "DataManager.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditCoordinator.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct RollbackRecorder
{
  std::uint64_t calls{0};
};

bool failRollback(void* context) noexcept
{
  ++static_cast<RollbackRecorder*>(context)->calls;
  return false;
}

void testUnknownExecutionFailpointRejectedBeforeSwap()
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  const CommittedEditIdentity before
      = NativeTimingEditPreparer::captureCurrentIdentity(
          STADM.getDatabase(), TransactionGeneration{0},
          editTestAnalysisContext(), session.token());
  Database* const canonical_address = &STADM.getDatabase();
  bool rejected = false;
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t, StaCsrSessionContext&,
              const NativeExecutionAuthority& authority) {
            NativeDatabaseCandidate candidate
                = NativeDatabaseCandidate::create(STADM.getDatabase());
            try {
              InstalledNativeDatabase unexpected
                  = NativeCandidateExecutor::installAndExecute(
                      authority, std::move(candidate),
                      static_cast<NativeExecutionFailpoint>(255));
              (void) unexpected;
            } catch (const std::invalid_argument& error) {
              rejected
                  = std::string(error.what()).find("unknown execution failpoint")
                    != std::string::npos;
            }
            return StaStatus::success(session.session().state());
          });
  const CommittedEditIdentity after
      = NativeTimingEditPreparer::captureCurrentIdentity(
          STADM.getDatabase(), TransactionGeneration{0},
          editTestAnalysisContext(), session.token());
  require(guarded.ok() && rejected && &STADM.getDatabase() == canonical_address
              && after == before,
          "unknown execution failpoint was not rejected before canonical swap");
  require(session.closeAndRelease().ok(), "unknown-failpoint close failed");
}

void testRollbackErrorImmediatelyPoisonsCoordinatorAndSession()
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  PreparedNativeTransaction transaction
      = prepareNativeTransaction(session.token());
  auto initial_host
      = buildNativeHostSnapshot(STADM.getDatabase(), session.token());
  TimingEditCoordinator coordinator(
      session.token(),
      StaTimingEditSessionProbeAccess::stateSignal(session.session()),
      transaction.baseline, transaction.initial_identity, CumulativeEditTrace{},
      transaction.resolver, std::move(initial_host));
  require(coordinator.beginPrepare(transaction.prepared).ok()
              && coordinator.markStaged().ok()
              && coordinator.markExecuting().ok(),
          "rollback-error fixture did not reach Executing");

  bool typed_error_seen = false;
  TransactionStatus rollback_status;
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            requireRecursiveStaOperationRejected(token, context);
            requireReleasedSnapshotForNativeMutation(context,
                                                     authority.sessionToken());
            TimingEditResolverState staged
                = transaction.resolver.stage(transaction.prepared);
            NativeDatabaseCandidate candidate
                = translateNativeCandidate(transaction, staged);
            try {
              InstalledNativeDatabase unexpected
                  = NativeCandidateExecutor::installAndExecute(
                      authority, std::move(candidate),
                      NativeExecutionFailpoint::kAfterInstall,
                      NativeRollbackFailpoint::
                          kDelayRuntimeCleanupAfterDatabaseRestore);
              (void) unexpected;
            } catch (const NativeExecutionRollbackError& error) {
              const std::string message = error.what();
              require(error.executionFailpoint()
                              == NativeExecutionFailpoint::kAfterInstall
                          && error.rollbackFailure()
                                 == NativeRollbackFailure::
                                     kDelayRuntimeCleanup
                          && error.originalFailure() != nullptr
                          && message.find("after-install") != std::string::npos
                          && message.find("runtime cleanup failed")
                                 != std::string::npos,
                      "typed rollback error lost diagnostics");
              bool original_rethrown = false;
              try {
                error.rethrowOriginalFailure();
              } catch (const std::runtime_error& original) {
                original_rethrown
                    = std::string(original.what()).find("after-install")
                      != std::string::npos;
              }
              require(original_rethrown,
                      "typed rollback error lost original exception");
              typed_error_seen = true;
            }
            RollbackRecorder recorder;
            rollback_status = coordinator.rollbackPrecommit(
                PrecommitRollbackSignal{&recorder, &failRollback});
            require(recorder.calls == 1,
                    "coordinator did not consume rollback signal once");
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok() && typed_error_seen
              && rollback_status.code
                     == TransactionStatusCode::kRollbackFailed
              && coordinator.state() == TimingEditCoordinatorState::kPoisoned
              && coordinator.committedIdentity() == transaction.initial_identity
              && session.session().state() == StaSessionState::kPoisoned,
          "rollback failure did not poison coordinator/session at exact g0");
  require(session.closeAndRelease().ok(), "rollback-error close failed");
}

}  // namespace

int main()
{
  NativeTransactionRuntime runtime;
  testUnknownExecutionFailpointRejectedBeforeSwap();
  testRollbackErrorImmediatelyPoisonsCoordinatorAndSession();
  return 0;
}
