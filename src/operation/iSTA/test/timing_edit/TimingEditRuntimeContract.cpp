// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <stdexcept>
#include <utility>

#include "DataManager.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace {
using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;
void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}
void testCommitAndRollback()
{
  for (bool commit : {false, true}) {
    installNativeTransactionBaseline();
    GuardedTestSession session;
    auto prepared = prepareNativeTransaction(session.token());
    auto guarded = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
        session.session(), [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
          requireReleasedSnapshotForNativeMutation(context, authority.sessionToken());
          PreparedTimingEditResolverPatch resolver_patch(
              prepared.resolver, prepared.resolver.prepare(prepared.prepared));
          auto installed = NativeEditTransaction::prepareFull(
              authority, prepared.prepared, resolver_patch.view());
          const auto names = prepared.prepared.tokenMaterializations().front();
          NativeCandidateExecutor::executeFull(installed);
          resolver_patch.apply();
          if constexpr (incregpusta_debug_checks_enabled) {
            auto host = buildNativeHostSnapshot(installed.database(), session.token());
            StaTimingEditSessionProbeAccess::requireCsrFullExact(*host, installed.database());
          }
          if (commit) {
            installed.commit();
            resolver_patch.commit();
          } else {
            require(installed.rollback(), "native full failed actual rollback");
            resolver_patch.rollback();
          }
          auto& database = STADM.getDatabase();
          require(database.get_instance_map().contains(names.instance_name) == commit
                      && database.get_net_map().contains(names.output_net_name) == commit,
                  "native commit/rollback lost its raw edit");
          const auto identity = NativeTimingEditPreparer::captureCurrentIdentity(
              database, TransactionGeneration{commit ? 1U : 0U}, editTestAnalysisContext(), session.token());
          if (!commit)
            require(identity == prepared.initial_identity, "rollback did not restore the committed identity");
          if (commit) {
            bool stale_rejected = false;
            try {
              auto stale = NativeEditTransaction::prepareFull(
                  authority, prepared.prepared, prepared.resolver);
              static_cast<void>(stale);
            } catch (const std::invalid_argument&) {
              stale_rejected = true;
            }
            require(stale_rejected && authority.readyForQuery(),
                    "already applied batch was accepted or retained pending ownership");
          }
          // Both publication and rollback release pending ownership for the next operation.
          auto next = NativeEditTransaction::prepareFull(authority);
          NativeCandidateExecutor::executeFull(next);
          next.commit();
          require(NativeTimingEditPreparer::captureCurrentIdentity(
                      database, identity.generation, editTestAnalysisContext(), session.token()) == identity,
                  "continued full execution changed the raw design");
          return StaStatus::success(session.session().state());
        });
    require(guarded.ok(), "native full guard failed: " + guarded.message);
    require(session.closeAndRelease().ok(), "native full session cleanup failed");
  }
}
}
int main()
{
  NativeTransactionRuntime runtime;
  testCommitAndRollback();
}
