// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "DataManager.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

static_assert(!std::is_constructible_v<NativeExecutionAuthority,
                                       TimingEditSessionToken>);
static_assert(!std::is_constructible_v<
              NativeExecutionAuthority, TimingEditSessionToken,
              NativeExecutionScopeViolationSignal>);
static_assert(!std::is_copy_constructible_v<NativeExecutionAuthority>);
static_assert(!std::is_move_constructible_v<NativeExecutionAuthority>);

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireMaterializationPresent(const TokenMaterialization& materialization)
{
  const Database& database = STADM.getDatabase();
  require(database.get_instance_map().contains(materialization.instance_name),
          "escaped candidate lost its materialized instance");
  require(database.get_net_map().contains(materialization.output_net_name),
          "escaped candidate lost its materialized output net");
  require(database.get_pin_map().contains(materialization.input_pin_name)
              && database.get_pin_map().contains(
                  materialization.output_pin_name),
          "escaped candidate lost its materialized pins");
}

void testInstalledOwnerCannotEscapeOperationGuard()
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  PreparedNativeTransaction transaction
      = prepareNativeTransaction(session.token());
  std::optional<NativeEditTransaction> escaped_owner;
  CommittedEditIdentity installed_identity;
  TokenMaterialization materialization;
  Database* installed_address{nullptr};

  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            require(token == session.token().value()
                        && authority.active()
                        && authority.sessionToken() == session.token(),
                    "guard did not supply a live session-scoped authority");
            requireRecursiveStaOperationRejected(token, context);
            ExecutedNativeTransaction executed
                = executeNativeTransaction(authority, context, transaction);
            installed_identity = executed.post_identity;
            materialization = executed.materialization;
            installed_address = &executed.installed.database();
            escaped_owner.emplace(std::move(executed.installed));
            require(escaped_owner->active(),
                    "installed owner was inactive before guard release");
            return StaStatus::success(session.session().state());
          });

  require(guarded.ok() && escaped_owner.has_value()
              && session.session().state() == StaSessionState::kPoisoned
              && !escaped_owner->active(),
          "escaped installed owner did not poison and close its guard scope");
  require(&STADM.getDatabase() == installed_address,
          "candidate Database changed while leaving the guard callback");
  const CommittedEditIdentity before_outside_rollback
      = NativeTimingEditPreparer::captureCurrentIdentity(
          STADM.getDatabase(), TransactionGeneration{1},
          editTestAnalysisContext(), session.token());
  require(before_outside_rollback == installed_identity,
          "escaped candidate identity changed at guard release");
  requireMaterializationPresent(materialization);
  bool database_access_rejected = false;
  try {
    static_cast<void>(escaped_owner->database());
  } catch (const std::logic_error&) {
    database_access_rejected = true;
  }
  require(database_access_rejected && &STADM.getDatabase() == installed_address,
          "escaped owner retained guard-external Database access");

  const NativeRollbackFailure outside_status
      = escaped_owner->rollbackStatus();
  const CommittedEditIdentity after_outside_rollback
      = NativeTimingEditPreparer::captureCurrentIdentity(
          STADM.getDatabase(), TransactionGeneration{1},
          editTestAnalysisContext(), session.token());
  require(outside_status == NativeRollbackFailure::kOutsideOperationGuardScope
              && !escaped_owner->active()
              && &STADM.getDatabase() == installed_address
              && after_outside_rollback == before_outside_rollback,
          "guard-external rollback exchanged the canonical Database");
  requireMaterializationPresent(materialization);
  escaped_owner.reset();
  require(session.closeAndRelease().ok(),
          "owner-escape poisoned session close failed");
}

struct CrossThreadOutcome
{
  bool active_on_wrong_thread{true};
  bool database_access_rejected{false};
  NativeRollbackFailure rollback_status{NativeRollbackFailure::kNone};
};

void testInstalledOwnerIsBoundToGuardThread()
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  PreparedNativeTransaction transaction
      = prepareNativeTransaction(session.token());
  CrossThreadOutcome outcome;
  CommittedEditIdentity installed_identity;
  TokenMaterialization materialization;
  Database* installed_address{nullptr};

  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t token, StaCsrSessionContext& context,
              const NativeExecutionAuthority& authority) {
            requireRecursiveStaOperationRejected(token, context);
            ExecutedNativeTransaction executed
                = executeNativeTransaction(authority, context, transaction);
            installed_identity = executed.post_identity;
            materialization = executed.materialization;
            installed_address = &executed.installed.database();

            std::thread wrong_thread(
                [owner = std::move(executed.installed), &outcome]() mutable {
                  outcome.active_on_wrong_thread = owner.active();
                  try {
                    static_cast<void>(owner.database());
                  } catch (const std::logic_error&) {
                    outcome.database_access_rejected = true;
                  }
                  outcome.rollback_status = owner.rollbackStatus();
                });
            wrong_thread.join();
            return StaStatus::success(session.session().state());
          });

  require(guarded.ok() && !outcome.active_on_wrong_thread
              && outcome.database_access_rejected
              && outcome.rollback_status
                     == NativeRollbackFailure::kOutsideOperationGuardScope
              && session.session().state() == StaSessionState::kPoisoned,
          "wrong-thread owner use did not fail closed and poison the session");
  require(&STADM.getDatabase() == installed_address,
          "wrong-thread rollback exchanged the active Database");
  const CommittedEditIdentity current
      = NativeTimingEditPreparer::captureCurrentIdentity(
          STADM.getDatabase(), TransactionGeneration{1},
          editTestAnalysisContext(), session.token());
  require(current == installed_identity,
          "wrong-thread rollback changed the installed candidate identity");
  requireMaterializationPresent(materialization);
  require(session.closeAndRelease().ok(),
          "wrong-thread poisoned session close failed");
}

}  // namespace

int main()
{
  NativeTransactionRuntime runtime;
  testInstalledOwnerCannotEscapeOperationGuard();
  testInstalledOwnerIsBoundToGuardThread();
  return 0;
}
