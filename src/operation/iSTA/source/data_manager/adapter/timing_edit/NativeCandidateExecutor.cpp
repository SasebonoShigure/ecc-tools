// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeCandidateExecutor.hpp"

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "ClockPropagator.hpp"
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "GraphBuilder.hpp"
#include "NativeDatabaseValidation.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingPropagator.hpp"

namespace ista::timing_edit {

class NativeExecutionScopeState final
{
 public:
  NativeExecutionScopeState(TimingEditSessionToken session_token,
                            NativeExecutionScopeViolationSignal violation_signal) noexcept
      : _session_token(session_token),
        _violation_signal(violation_signal),
        _guard_thread(std::this_thread::get_id())
  {
  }

  [[nodiscard]] bool active() const noexcept
  {
    return _active.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool matches(TimingEditSessionToken session_token) const noexcept
  {
    return session_token == _session_token && active()
           && std::this_thread::get_id() == _guard_thread;
  }

  [[nodiscard]] bool attachOwner(TimingEditSessionToken session_token) noexcept
  {
    if (!matches(session_token)) {
      return false;
    }
    _owner_count.fetch_add(1, std::memory_order_acq_rel);
    if (matches(session_token)) {
      return true;
    }
    detachOwner(session_token);
    return false;
  }

  void detachOwner(TimingEditSessionToken session_token) noexcept
  {
    if (session_token != _session_token) {
      reportViolation();
      return;
    }
    std::uint32_t previous = _owner_count.load(std::memory_order_acquire);
    while (previous != 0
           && !_owner_count.compare_exchange_weak(previous, previous - 1,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
    }
    if (previous == 0) {
      reportViolation();
    }
  }

  [[nodiscard]] bool beginProtectedUse(TimingEditSessionToken session_token) noexcept
  {
    if (!matches(session_token)) {
      return false;
    }
    _protected_use_count.fetch_add(1, std::memory_order_acq_rel);
    if (matches(session_token)) {
      return true;
    }
    endProtectedUse();
    return false;
  }

  void endProtectedUse() noexcept
  {
    std::uint32_t previous = _protected_use_count.load(std::memory_order_acquire);
    while (previous != 0
           && !_protected_use_count.compare_exchange_weak(previous, previous - 1,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
    }
    if (previous == 0) {
      reportViolation();
    }
  }

  void close() noexcept
  {
    if (!_active.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    while (_protected_use_count.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
    if (_owner_count.load(std::memory_order_acquire) != 0) {
      reportViolation();
    }
  }

  void reportViolation() noexcept
  {
    if (!_violation_reported.exchange(true, std::memory_order_acq_rel)
        && _violation_signal.valid()) {
      _violation_signal.invoke();
    }
  }

 private:
  TimingEditSessionToken _session_token;
  NativeExecutionScopeViolationSignal _violation_signal;
  std::atomic<bool> _active{true};
  const std::thread::id _guard_thread;
  std::atomic<std::uint32_t> _owner_count{0};
  std::atomic<std::uint32_t> _protected_use_count{0};
  std::atomic<bool> _violation_reported{false};
};

namespace {

using StageFunction = void (*)();

[[nodiscard]] const char* executionFailpointName(NativeExecutionFailpoint failpoint) noexcept
{
  switch (failpoint) {
    case NativeExecutionFailpoint::kNone:
      return "none";
    case NativeExecutionFailpoint::kAfterInstall:
      return "after-install";
    case NativeExecutionFailpoint::kBeforeDelayRuntimeInit:
      return "before-delay-runtime-init";
    case NativeExecutionFailpoint::kBeforeGraphBuilderInit:
      return "before-graph-builder-init";
    case NativeExecutionFailpoint::kBeforeGraphBuilderWork:
      return "before-graph-builder-work";
    case NativeExecutionFailpoint::kBeforeGraphBuilderDestroy:
      return "before-graph-builder-destroy";
    case NativeExecutionFailpoint::kBeforeClockPropagatorInit:
      return "before-clock-propagator-init";
    case NativeExecutionFailpoint::kBeforeClockPropagatorWork:
      return "before-clock-propagator-work";
    case NativeExecutionFailpoint::kBeforeClockPropagatorDestroy:
      return "before-clock-propagator-destroy";
    case NativeExecutionFailpoint::kBeforeTimingPropagatorInit:
      return "before-timing-propagator-init";
    case NativeExecutionFailpoint::kBeforeTimingPropagatorWork:
      return "before-timing-propagator-work";
    case NativeExecutionFailpoint::kBeforeTimingPropagatorDestroy:
      return "before-timing-propagator-destroy";
    case NativeExecutionFailpoint::kBeforeTimingAnalyzerInit:
      return "before-timing-analyzer-init";
    case NativeExecutionFailpoint::kBeforeTimingAnalyzerWork:
      return "before-timing-analyzer-work";
    case NativeExecutionFailpoint::kBeforeTimingAnalyzerDestroy:
      return "before-timing-analyzer-destroy";
    case NativeExecutionFailpoint::kBeforeValidation:
      return "before-validation";
    case NativeExecutionFailpoint::kBeforeDelayRuntimeDestroy:
      return "before-delay-runtime-destroy";
    case NativeExecutionFailpoint::kBeforePersistentDelayRestore:
      return "before-persistent-delay-restore";
  }
  return "unknown";
}

[[nodiscard]] bool isKnownExecutionFailpoint(NativeExecutionFailpoint failpoint) noexcept
{
  return static_cast<std::uint8_t>(failpoint)
         <= static_cast<std::uint8_t>(
             NativeExecutionFailpoint::kBeforePersistentDelayRestore);
}

[[nodiscard]] bool isKnownRollbackFailpoint(NativeRollbackFailpoint failpoint) noexcept
{
  return failpoint == NativeRollbackFailpoint::kNone
         || failpoint == NativeRollbackFailpoint::kDelayRuntimeCleanupAfterDatabaseRestore
         || failpoint == NativeRollbackFailpoint::kPersistentDelayRestoreAfterAttempt
         || failpoint == NativeRollbackFailpoint::kCleanupAndPersistentRestore;
}

[[nodiscard]] bool injectsCleanupFailure(NativeRollbackFailpoint failpoint) noexcept
{
  return failpoint == NativeRollbackFailpoint::kDelayRuntimeCleanupAfterDatabaseRestore
         || failpoint == NativeRollbackFailpoint::kCleanupAndPersistentRestore;
}

[[nodiscard]] bool injectsPersistentRestoreFailure(NativeRollbackFailpoint failpoint) noexcept
{
  return failpoint == NativeRollbackFailpoint::kPersistentDelayRestoreAfterAttempt
         || failpoint == NativeRollbackFailpoint::kCleanupAndPersistentRestore;
}

[[nodiscard]] NativeRollbackFailure classifyRollbackFailure(bool cleanup_succeeded,
                                                            bool restore_succeeded) noexcept
{
  if (cleanup_succeeded && restore_succeeded) {
    return NativeRollbackFailure::kNone;
  }
  if (!cleanup_succeeded && !restore_succeeded) {
    return NativeRollbackFailure::kDelayRuntimeCleanupAndPersistentDelayRestore;
  }
  return cleanup_succeeded ? NativeRollbackFailure::kPersistentDelayRestore
                           : NativeRollbackFailure::kDelayRuntimeCleanup;
}

[[nodiscard]] const char* rollbackFailureDescription(NativeRollbackFailure failure) noexcept
{
  switch (failure) {
    case NativeRollbackFailure::kNone:
      return "none";
    case NativeRollbackFailure::kDelayRuntimeCleanup:
      return "DelayCalculator runtime cleanup failed";
    case NativeRollbackFailure::kPersistentDelayRestore:
      return "persistent DelayCalculator restore failed";
    case NativeRollbackFailure::kDelayRuntimeCleanupAndPersistentDelayRestore:
      return "DelayCalculator runtime cleanup and persistent restore both failed";
    case NativeRollbackFailure::kOutsideOperationGuardScope:
      return "native Database owner escaped its operation-guard scope";
  }
  return "unknown rollback failure";
}

[[nodiscard]] std::string originalFailureDescription(const std::exception_ptr& failure)
{
  if (!failure) {
    return "missing original exception";
  }
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "non-std exception";
  }
}

void inject(NativeExecutionFailpoint armed, NativeExecutionFailpoint current)
{
  if (armed == current) {
    throw std::runtime_error(std::string("injected native candidate execution failure at ")
                             + executionFailpointName(current));
  }
}

void destroyTransientSingletons() noexcept
{
  TimingAnalyzer::destroyInst();
  TimingPropagator::destroyInst();
  ClockPropagator::destroyInst();
  GraphBuilder::destroyInst();
}

bool quiesceDelayCalculatorNoThrow() noexcept
{
  bool success = true;
  try {
    // initInst() is idempotent and guarantees STADC cannot dereference null.
    DelayCalculator::initInst();
    STADC.destroy();
  } catch (...) {
    success = false;
  }
  DelayCalculator::destroyInst();
  return success;
}

void quiesceDelayCalculatorOrRestore()
{
  DelayCalculator::initInst();
  try {
    STADC.destroy();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    DelayCalculator::destroyInst();
    try {
      DelayCalculator::initInst();
    } catch (...) {
      // The active Database is still the old canonical Database. Preserve the
      // original runtime-cleanup failure after attempting persistent restore.
    }
    std::rethrow_exception(failure);
  }
  DelayCalculator::destroyInst();
}

bool restorePersistentDelayCalculator() noexcept
{
  try {
    DelayCalculator::initInst();
    return true;
  } catch (...) {
    return false;
  }
}

void runStage(NativeExecutionFailpoint armed, NativeExecutionFailpoint before_init, NativeExecutionFailpoint before_work,
              NativeExecutionFailpoint before_destroy, StageFunction init, StageFunction work, StageFunction destroy)
{
  inject(armed, before_init);
  init();
  try {
    inject(armed, before_work);
    work();
    inject(armed, before_destroy);
  } catch (...) {
    destroy();
    throw;
  }
  destroy();
}

void runFourTimingStages(NativeExecutionFailpoint failpoint)
{
  runStage(failpoint, NativeExecutionFailpoint::kBeforeGraphBuilderInit, NativeExecutionFailpoint::kBeforeGraphBuilderWork,
           NativeExecutionFailpoint::kBeforeGraphBuilderDestroy, [] { GraphBuilder::initInst(); }, [] { STAGB.build(); },
           [] { GraphBuilder::destroyInst(); });
  runStage(failpoint, NativeExecutionFailpoint::kBeforeClockPropagatorInit, NativeExecutionFailpoint::kBeforeClockPropagatorWork,
           NativeExecutionFailpoint::kBeforeClockPropagatorDestroy, [] { ClockPropagator::initInst(); }, [] { STACP.propagate(); },
           [] { ClockPropagator::destroyInst(); });
  runStage(failpoint, NativeExecutionFailpoint::kBeforeTimingPropagatorInit, NativeExecutionFailpoint::kBeforeTimingPropagatorWork,
           NativeExecutionFailpoint::kBeforeTimingPropagatorDestroy, [] { TimingPropagator::initInst(); }, [] { STATP.propagate(); },
           [] { TimingPropagator::destroyInst(); });
  runStage(failpoint, NativeExecutionFailpoint::kBeforeTimingAnalyzerInit, NativeExecutionFailpoint::kBeforeTimingAnalyzerWork,
           NativeExecutionFailpoint::kBeforeTimingAnalyzerDestroy, [] { TimingAnalyzer::initInst(); }, [] { STATA.analyze(); },
           [] { TimingAnalyzer::destroyInst(); });
}

}  // namespace

NativeExecutionRollbackError::NativeExecutionRollbackError(
    NativeExecutionFailpoint execution_failpoint, NativeRollbackFailure rollback_failure,
    std::exception_ptr original_failure)
    : std::runtime_error(
          std::string("native candidate primary execution failed at failpoint ")
          + executionFailpointName(execution_failpoint) + ": "
          + originalFailureDescription(original_failure) + "; explicit rollback failed: "
          + rollbackFailureDescription(rollback_failure)),
      _execution_failpoint(execution_failpoint),
      _rollback_failure(rollback_failure),
      _original_failure(std::move(original_failure))
{
}

void NativeExecutionRollbackError::rethrowOriginalFailure() const
{
  if (!_original_failure) {
    throw std::logic_error("native rollback error did not retain its original failure");
  }
  std::rethrow_exception(_original_failure);
}

NativeExecutionAuthority::NativeExecutionAuthority(
    TimingEditSessionToken session_token,
    NativeExecutionScopeViolationSignal violation_signal)
    : _session_token(session_token)
{
  if (_session_token.value() == 0) {
    throw std::invalid_argument("native execution authority requires a nonzero session token");
  }
  if (!violation_signal.valid()) {
    throw std::invalid_argument(
        "native execution authority requires an operation-scope violation signal");
  }
  _scope_state = std::make_shared<NativeExecutionScopeState>(_session_token,
                                                             violation_signal);
}

NativeExecutionAuthority::~NativeExecutionAuthority() noexcept
{
  if (_scope_state != nullptr) {
    _scope_state->close();
  }
}

bool NativeExecutionAuthority::active() const noexcept
{
  return _scope_state != nullptr && _scope_state->matches(_session_token);
}

InstalledNativeDatabase::InstalledNativeDatabase(
    std::unique_ptr<Database> rollback_database,
    TimingEditSessionToken session_token,
    std::shared_ptr<NativeExecutionScopeState> execution_scope) noexcept
    : _rollback_database(std::move(rollback_database)),
      _execution_scope(std::move(execution_scope)),
      _execution_session_token(session_token),
      _scope_attached(_execution_scope != nullptr
                      && _execution_scope->attachOwner(session_token)),
      _installed(true)
{
  if (!_scope_attached && _execution_scope != nullptr) {
    _execution_scope->reportViolation();
  }
}

bool InstalledNativeDatabase::active() const noexcept
{
  return _installed && executionScopeActive();
}

bool InstalledNativeDatabase::executionScopeActive() const noexcept
{
  return _scope_attached && _execution_scope != nullptr
         && _execution_scope->matches(_execution_session_token);
}

bool InstalledNativeDatabase::executionScopeMatches(
    const NativeExecutionAuthority& authority) const noexcept
{
  return executionScopeActive() && authority.active()
         && authority.sessionToken() == _execution_session_token
         && authority._scope_state == _execution_scope;
}

void InstalledNativeDatabase::commit() noexcept
{
  _rollback_database.reset();
  if (_scope_attached && _execution_scope != nullptr) {
    _execution_scope->detachOwner(_execution_session_token);
  }
  _scope_attached = false;
  _installed = false;
}

InstalledNativeDatabase::~InstalledNativeDatabase()
{
  if (!rollback()) {
    std::fputs("ERROR: InstalledNativeDatabase destructor fallback could not restore all native "
               "timing runtime invariants; the owning timing-edit session must be poisoned.\n",
               stderr);
    std::fflush(stderr);
  }
}

bool InstalledNativeDatabase::rollback() noexcept
{
  return rollbackStatus() == NativeRollbackFailure::kNone;
}

NativeRollbackFailure InstalledNativeDatabase::rollbackStatus() noexcept
{
  return rollbackWithDiagnostics(NativeRollbackFailpoint::kNone);
}

NativeRollbackFailure InstalledNativeDatabase::rollbackWithDiagnostics(
    NativeRollbackFailpoint failpoint) noexcept
{
  if (!_installed) {
    return NativeRollbackFailure::kNone;
  }
  if (_execution_scope == nullptr || !_scope_attached
      || !_execution_scope->beginProtectedUse(_execution_session_token)) {
    if (_execution_scope != nullptr) {
      _execution_scope->reportViolation();
      if (_scope_attached) {
        _execution_scope->detachOwner(_execution_session_token);
      }
    }
    _rollback_database.reset();
    _scope_attached = false;
    _installed = false;
    return NativeRollbackFailure::kOutsideOperationGuardScope;
  }

  destroyTransientSingletons();
  bool cleanup_succeeded = quiesceDelayCalculatorNoThrow();
  std::unique_ptr<Database> rejected_candidate
      = NativeDatabaseCandidate::exchangeActiveDatabase(std::move(_rollback_database));
  rejected_candidate.reset();

  // Cleanup fault injection is observed only after the old canonical Database
  // has been exchanged back and before persistent runtime restoration.
  cleanup_succeeded = cleanup_succeeded && !injectsCleanupFailure(failpoint);
  bool restore_succeeded = restorePersistentDelayCalculator();
  // Restore fault injection is observed after the actual restore attempt.
  restore_succeeded = restore_succeeded && !injectsPersistentRestoreFailure(failpoint);
  const NativeRollbackFailure failure
      = classifyRollbackFailure(cleanup_succeeded, restore_succeeded);
  if (failure != NativeRollbackFailure::kNone) {
    _execution_scope->reportViolation();
  }
  if (_scope_attached) {
    _execution_scope->detachOwner(_execution_session_token);
  }
  _scope_attached = false;
  _installed = false;
  _execution_scope->endProtectedUse();
  return failure;
}

InstalledNativeDatabase NativeCandidateExecutor::installAndExecute(const NativeExecutionAuthority& authority,
                                                                    NativeDatabaseCandidate&& candidate,
                                                                    NativeExecutionFailpoint failpoint,
                                                                    NativeRollbackFailpoint rollback_failpoint)
{
  if (!authority.active() || authority.sessionToken().value() == 0) {
    throw std::invalid_argument("native candidate executor received invalid operation-guard authority");
  }
  if (candidate.empty()) {
    throw std::invalid_argument("native candidate executor received an empty candidate");
  }
  if (!isKnownExecutionFailpoint(failpoint)) {
    throw std::invalid_argument("native candidate executor received an unknown execution failpoint");
  }
  if (!isKnownRollbackFailpoint(rollback_failpoint)) {
    throw std::invalid_argument("native candidate executor received an unknown rollback failpoint");
  }

  destroyTransientSingletons();
  quiesceDelayCalculatorOrRestore();

  InstalledNativeDatabase installed
      = candidate.installForExecution(authority.sessionToken(), authority._scope_state);
  try {
    if (!installed.executionScopeMatches(authority)) {
      throw std::logic_error("native Database installation lost its operation-guard scope");
    }
    inject(failpoint, NativeExecutionFailpoint::kAfterInstall);
    DelayCalculator::initInst();
    inject(failpoint, NativeExecutionFailpoint::kBeforeDelayRuntimeInit);
    STADC.init();

    runFourTimingStages(failpoint);
    inject(failpoint, NativeExecutionFailpoint::kBeforeValidation);
    requireValidNativeDatabase(installed.database());

    inject(failpoint, NativeExecutionFailpoint::kBeforeDelayRuntimeDestroy);
    destroyTransientSingletons();
    STADC.destroy();
    DelayCalculator::destroyInst();
    inject(failpoint, NativeExecutionFailpoint::kBeforePersistentDelayRestore);
    DelayCalculator::initInst();
    return installed;
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    const NativeRollbackFailure rollback_failure
        = installed.rollbackWithDiagnostics(rollback_failpoint);
    if (rollback_failure == NativeRollbackFailure::kNone) {
      std::rethrow_exception(failure);
    }
    throw NativeExecutionRollbackError(failpoint, rollback_failure, failure);
  }
}

}  // namespace ista::timing_edit
