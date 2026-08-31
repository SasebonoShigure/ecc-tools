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

#include "DataManager.hpp"
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

void advanceToExecuting(TimingEditCoordinator& coordinator,
                        const PreparedBatch& batch)
{
  require(coordinator.beginPrepare(batch).ok(), "did not enter Prepared");
  require(coordinator.markStaged().ok(), "did not enter Staged");
  require(coordinator.markExecuting().ok(), "did not enter Executing");
}

std::unique_ptr<csr::CsrCommittedSessionSnapshot> buildAlternateContextHost(
    const Database& database, TimingEditSessionToken session_token)
{
  csr::StaAnalysisContext context = editTestAnalysisContext();
  context.temperature = "26";
  csr::CsrSessionSnapshotBuildResult result = csr::buildCsrSessionSnapshot(
      database, context, session_token.value());
  auto* snapshot
      = std::get_if<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>(&result);
  if (snapshot == nullptr || *snapshot == nullptr) {
    throw std::runtime_error("alternate-context CSR snapshot build failed");
  }
  return std::move(*snapshot);
}

void requireExactG0(const TimingEditCoordinator& coordinator,
                    const PreparedNativeTransaction& transaction,
                    const csr::CsrCommittedSessionSnapshot* initial_host,
                    const TokenMaterialization& materialization)
{
  const Database& database = STADM.getDatabase();
  const CommittedEditIdentity native_identity
      = NativeTimingEditPreparer::captureCurrentIdentity(
          database, TransactionGeneration{0}, editTestAnalysisContext(),
          transaction.prepared.sessionToken());
  require(coordinator.state() == TimingEditCoordinatorState::kIdle
              && coordinator.committedIdentity() == transaction.initial_identity
              && coordinator.cumulativeTrace().batches.empty()
              && coordinator.resolverState() == transaction.resolver
              && coordinator.hostSnapshot() == initial_host
              && coordinator.deviceState().generation
                     == TransactionGeneration{0}
              && coordinator.commitMarker().generation
                     == TransactionGeneration{0}
              && native_identity == transaction.initial_identity,
          "receipt rejection did not restore exact g0 state");
  require(!database.get_instance_map().contains(materialization.instance_name)
              && !database.get_net_map().contains(
                  materialization.output_net_name)
              && !database.get_pin_map().contains(
                  materialization.input_pin_name)
              && !database.get_pin_map().contains(
                  materialization.output_pin_name),
          "receipt rejection retained candidate materialization");
}

enum class ReceiptMismatch : std::uint8_t
{
  kMovedFrom,
  kOldDatabase,
  kWrongGeneration
};

void runReceiptMismatch(ReceiptMismatch mismatch)
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
  const csr::CsrCommittedSessionSnapshot* const initial_host_address
      = initial_host.get();
  auto coordinator
      = makeCoordinator(session, transaction, std::move(initial_host));
  advanceToExecuting(*coordinator, transaction.prepared);

  TransactionStatus publish_status;
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            require(token == session.token().value(), "guard token changed");
            ExecutedNativeTransaction executed
                = executeNativeTransaction(authority, context, transaction);
            require(coordinator
                        ->sealVerified(transaction.prepared,
                                       executed.post_identity)
                        .ok(),
                    "receipt fixture did not reach Verified");

            CsrFullExactReceipt rejected_receipt
                = std::move(executed.csr_full_receipt);
            if (mismatch == ReceiptMismatch::kMovedFrom) {
              CsrFullExactReceipt retained = std::move(rejected_receipt);
              (void) retained;
            } else if (mismatch == ReceiptMismatch::kOldDatabase) {
              auto alternate_host = buildAlternateContextHost(
                  executed.installed.database(), authority.sessionToken());
              rejected_receipt
                  = StaTimingEditSessionProbeAccess::proveCsrFullExact(
                      authority, *alternate_host,
                      executed.installed.database(), TransactionGeneration{1});
            } else {
              rejected_receipt
                  = StaTimingEditSessionProbeAccess::proveCsrFullExact(
                      authority, *executed.host_snapshot,
                      executed.installed.database(), TransactionGeneration{2});
            }
            CumulativeEditTrace trace = nativeSuccessorTrace(transaction);
            VerifiedPublishBundle bundle(
                std::move(executed.installed),
                std::move(executed.host_snapshot),
                std::move(rejected_receipt), std::move(transaction.prepared),
                std::move(trace), std::move(executed.staged_resolver));
            publish_status
                = coordinator->publish(authority, std::move(bundle));
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok()
              && publish_status.code
                     == TransactionStatusCode::kCsrFullReceiptMismatch,
          "invalid receipt did not return kCsrFullReceiptMismatch: "
              + guarded.message);
  requireExactG0(*coordinator, transaction, initial_host_address,
                 materialization);
  require(session.session().state() == StaSessionState::kBaselineExecuted,
          "receipt rollback changed the session state");
  require(session.closeAndRelease().ok(), "receipt session close failed");
}

}  // namespace

int main()
{
  NativeTransactionRuntime runtime;
  runReceiptMismatch(ReceiptMismatch::kMovedFrom);
  runReceiptMismatch(ReceiptMismatch::kOldDatabase);
  runReceiptMismatch(ReceiptMismatch::kWrongGeneration);
  return 0;
}
