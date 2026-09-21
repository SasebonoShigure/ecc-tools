#include "IncreGpuStaBuildConfig.hpp"
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
#include "../AllocationFailureSupport.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
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

void testUnknownExecutionFailpointRejectedBeforeSwap(NativeExecutionFailpoint failpoint = static_cast<NativeExecutionFailpoint>(255))
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
            auto candidate = NativeEditTransaction::prepareFull(authority);
            try {
              NativeCandidateExecutor::executeFull(candidate, failpoint);
            } catch (const std::invalid_argument& error) {
              rejected = std::string(error.what()).find(incregpusta_debug_checks_enabled ? "unknown execution failpoint" : "ECC_INCREGPUSTA_DEBUG_CHECKS=ON")
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

void testRollbackErrorImmediatelyPoisonsSession()
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  PreparedNativeTransaction transaction
      = prepareNativeTransaction(session.token());
  bool typed_error_seen = false;
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
            auto pending = NativeEditTransaction::prepareFull(authority, transaction.prepared, staged);
            try {
              NativeCandidateExecutor::executeFull(pending, NativeExecutionFailpoint::kAfterInstall,
                                                   NativeRollbackFailpoint::kDelayRuntimeCleanupAfterDatabaseRestore);
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
            require(!authority.active(), "poisoned scope still authorizes native execution");
            bool blocked = false;
            try {
              auto next = NativeEditTransaction::prepareFull(authority, transaction.prepared, staged);
              static_cast<void>(next);
            } catch (const std::invalid_argument&) {
              blocked = true;
            }
            require(blocked && !STADM.getDatabase().get_instance_map().contains(
                        staged.materialization(transaction.plan.edits.front().creation_token).instance_name),
                    "poisoned scope accepted another transaction or changed raw state");
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok() && typed_error_seen
              && session.session().state() == StaSessionState::kPoisoned,
          "rollback failure did not poison session");
  require(session.closeAndRelease().ok(), "rollback-error close failed");
}

void testCanonicalFullTransactionRollbackAndAbandon()
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  auto prepared = prepareNativeTransaction(session.token());
  const auto staged = prepared.resolver.stage(prepared.prepared);
  auto* const database = &STADM.getDatabase();
  auto* const library = &database->get_timing_library();
  const auto* const arcs = database->get_arc_list().data();
  const auto old_driver = database->get_net_map().at("n0").get_driver_pin();
  const auto old_loads = database->get_net_map().at("n0").get_load_pin_list();
  const auto old_direction = database->get_pin_map().at("out").get_direction();
  const auto guarded = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
      session.session(), [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
        requireReleasedSnapshotForNativeMutation(context, authority.sessionToken());
        {
          auto transaction = NativeEditTransaction::prepareFull(authority, prepared.prepared, staged);
          require(&transaction.database() == database && &database->get_timing_library() == library,
                  "full transaction replaced canonical Database or immutable Liberty");
          bool conflict = false;
          try {
            auto second = NativeEditTransaction::prepareFull(authority);
            static_cast<void>(second);
          } catch (const std::exception&) {
            conflict = true;
          }
          require(conflict && transaction.active(), "second pending transaction was accepted");
          NativeCandidateExecutor::executeFull(transaction);
          ista_test_allocation::armFailure(0);
          const bool restored = transaction.rollback();
          const auto allocations = ista_test_allocation::disable();
          require(restored && allocations == 0, "full rollback allocated or failed to restore");
        }
        require(database->get_arc_list().data() == arcs && database->get_net_map().at("n0").get_driver_pin() == old_driver
                    && database->get_pin_map().at("out").get_direction() == old_direction,
                "rollback lost moved graph storage, net cache or pin direction");
        {
          auto abandoned = NativeEditTransaction::prepareFull(authority, prepared.prepared, staged);
          NativeCandidateExecutor::prepareFullGraph(abandoned);
        }
        require(database->get_arc_list().data() == arcs && authority.active(), "abandoned transaction did not restore/release ownership");
        const auto identity = NativeTimingEditPreparer::captureCurrentIdentity(
            *database, TransactionGeneration{0}, editTestAnalysisContext(), session.token());
        require(identity == prepared.initial_identity, "local rollback did not restore canonical raw state");
        if constexpr (incregpusta_debug_checks_enabled) {
          for (const auto failpoint : {NativeExecutionFailpoint::kBeforeGraphBuilderDestroy,
                                       NativeExecutionFailpoint::kBeforeTimingAnalyzerWork}) {
            auto interrupted = NativeEditTransaction::prepareFull(authority, prepared.prepared, staged);
            bool failed = false;
            try {
              NativeCandidateExecutor::executeFull(interrupted, failpoint);
            } catch (const std::runtime_error& error) {
              failed = std::string(error.what()).find("injected native candidate execution failure at ") == 0;
            }
            require(failed && !interrupted.active() && authority.active()
                        && database->get_arc_list().data() == arcs
                        && database->get_net_map().at("n0").get_driver_pin() == old_driver
                        && database->get_net_map().at("n0").get_load_pin_list() == old_loads
                        && database->get_pin_map().at("out").get_direction() == old_direction
                        && NativeTimingEditPreparer::captureCurrentIdentity(
                               *database, TransactionGeneration{0}, editTestAnalysisContext(), session.token()) == identity,
                    "interrupted full computation failed to restore graph, caches, raw state or ownership");
          }
          auto retry = NativeEditTransaction::prepareFull(authority, prepared.prepared, staged);
          NativeCandidateExecutor::executeFull(retry);
          retry.commit();
          require(!retry.active() && database->get_instance_map().contains(
                      staged.materialization(prepared.plan.edits.front().creation_token).instance_name),
                  "full retry after interrupted build did not commit");
        }
        return StaStatus::success(session.session().state());
      });
  require(guarded.ok(), "canonical transaction guard failed");
  require(session.closeAndRelease().ok(), "canonical transaction cleanup failed");
}

}  // namespace

int main()
{
  NativeTransactionRuntime runtime;
  testCanonicalFullTransactionRollbackAndAbandon();
  testUnknownExecutionFailpointRejectedBeforeSwap();
  if constexpr (ista::incregpusta_debug_checks_enabled) {
    testRollbackErrorImmediatelyPoisonsSession();
  } else {
    testUnknownExecutionFailpointRejectedBeforeSwap(NativeExecutionFailpoint::kAfterInstall);
  }
  return 0;
}
