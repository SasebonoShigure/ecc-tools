// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <stdexcept>
#include <utility>

#include "DataManager.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace ista::timing_edit::test {
namespace {
void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}
}  // namespace

void testTimingEditRuntimeContract()
{
  // Both real commit and real rollback on a rejected generation run without injection.
  for (bool reject_generation : {false, true}) {
    installNativeTransactionBaseline();
    GuardedTestSession session;
    auto transaction = prepareNativeTransaction(session.token());
    TimingEditCoordinator coordinator(session.token(), StaTimingEditSessionProbeAccess::stateSignal(session.session()), transaction.baseline,
                                      transaction.initial_identity, {}, transaction.resolver, buildNativeHostSnapshot(STADM.getDatabase(), session.token()));
    require(coordinator.beginPrepare(transaction.prepared).ok() && coordinator.markStaged().ok() && coordinator.markExecuting().ok(),
            "runtime transaction did not enter execution");
    auto guarded = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session.session(), [&](std::uint64_t, StaCsrSessionContext& context,
                                                                                                       const NativeExecutionAuthority& authority) {
      auto executed = executeNativeTransaction(authority, context, transaction);
      require(executed.csr_full_receipt.hasFullExactProof() == incregpusta_debug_checks_enabled, "execution authorization falsely reported a full exact proof");
      require(coordinator.sealVerified(transaction.prepared, executed.post_identity).ok(), "runtime transaction did not seal");
      auto trace = nativeSuccessorTrace(transaction);
      const auto materialization = executed.materialization;
      if (reject_generation) {
        // A receipt for another generation is a necessary identity failure.
        executed.csr_full_receipt
            = StaTimingEditSessionProbeAccess::proveCsrFullExact(authority, *executed.host_snapshot, executed.installed.database(), TransactionGeneration{2});
      }
      VerifiedPublishBundle bundle(std::move(executed.installed), std::move(executed.host_snapshot), std::move(executed.csr_full_receipt),
                                   std::move(transaction.prepared), std::move(trace), std::move(executed.staged_resolver));
      auto status = coordinator.publish(authority, std::move(bundle));
      require(status.ok() != reject_generation, "runtime publish result differs from generation binding");
      const auto& database = STADM.getDatabase();
      require(database.get_instance_map().contains(materialization.instance_name) != reject_generation
                  && database.get_net_map().contains(materialization.output_net_name) != reject_generation,
              "native commit or rollback did not update the actual database");
      require(coordinator.committedIdentity().generation == TransactionGeneration{reject_generation ? 0U : 1U},
              "rejected publish changed generation or successful commit was lost");
      return StaStatus::success(session.session().state());
    });
    require(guarded.ok(), "runtime transaction guard failed: " + guarded.message);
    require(session.closeAndRelease().ok(), "runtime transaction did not close");
  }
}
}  // namespace ista::timing_edit::test
