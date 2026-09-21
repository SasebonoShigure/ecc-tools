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
#include "IncreGpuStaBuildConfig.hpp"
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
    return session_token == _session_token && active() && !_violation_reported.load(std::memory_order_acquire)
           && std::this_thread::get_id() == _guard_thread;
  }

  [[nodiscard]] bool readyForQuery(TimingEditSessionToken session_token) const noexcept
  {
    return matches(session_token) && _owner_count.load(std::memory_order_acquire) == 0;
  }

  [[nodiscard]] bool attachTransaction(TimingEditSessionToken session_token) noexcept
  {
    if (!matches(session_token))
      return false;
    std::uint32_t expected = 0;
    return _owner_count.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
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
    case NativeRollbackFailure::kDiagnosticsDisabled:
      return "rollback fault injection requires ECC_INCREGPUSTA_DEBUG_CHECKS=ON";
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

void NativeExecutionAuthority::poison() const noexcept
{
  _scope_state->reportViolation();
}

bool NativeExecutionAuthority::readyForQuery() const noexcept
{
  return _scope_state != nullptr && _scope_state->readyForQuery(_session_token);
}

void NativeEditTransaction::attach(const NativeExecutionAuthority& authority)
{
  if (!authority.active() || !authority._scope_state->attachTransaction(authority.sessionToken()))
    throw std::logic_error("native edit transaction conflicts with a pending transaction");
  _execution_scope = authority._scope_state;
  _session_token = authority.sessionToken();
  _attached = true;
}
bool NativeEditTransaction::active() const noexcept
{
  return _attached && _execution_scope && _execution_scope->matches(_session_token);
}
void NativeEditTransaction::commit() noexcept
{
  if (!active()) {
    if (_execution_scope)
      _execution_scope->reportViolation();
    return;
  }
  _applied = false;
  discardUndo();
  _execution_scope->detachOwner(_session_token);
  _attached = false;
}
bool NativeEditTransaction::rollback() noexcept
{
  return rollbackStatus() == NativeRollbackFailure::kNone;
}
NativeRollbackFailure NativeEditTransaction::rollbackStatus() noexcept
{
  return rollbackWithDiagnostics(NativeRollbackFailpoint::kNone);
}
NativeRollbackFailure NativeEditTransaction::rollbackWithDiagnostics(NativeRollbackFailpoint failpoint) noexcept
{
  if (!incregpusta_debug_checks_enabled && failpoint != NativeRollbackFailpoint::kNone)
    return NativeRollbackFailure::kDiagnosticsDisabled;
  if (!_attached)
    return NativeRollbackFailure::kNone;
  if (!_execution_scope || !_execution_scope->beginProtectedUse(_session_token)) {
    if (_execution_scope) {
      _execution_scope->reportViolation();
      _execution_scope->detachOwner(_session_token);
    }
    _attached = false;
    discardUndo();
    return NativeRollbackFailure::kOutsideOperationGuardScope;
  }
  if (_applied) {
    destroyTransientSingletons();
    STADC.resetForEditRollback();
  }
  // Metadata can already be applied while native graph preparation still throws.
  undo();
  bool cleanup_succeeded = true;
  discardUndo();
  cleanup_succeeded = cleanup_succeeded && !injectsCleanupFailure(failpoint);
  const bool restore_succeeded = !injectsPersistentRestoreFailure(failpoint);
  const auto failure = classifyRollbackFailure(cleanup_succeeded, restore_succeeded);
  if (failure != NativeRollbackFailure::kNone)
    _execution_scope->reportViolation();
  _execution_scope->detachOwner(_session_token);
  _attached = false;
  _execution_scope->endProtectedUse();
  return failure;
}
void NativeCandidateExecutor::executeFull(NativeEditTransaction& transaction,
                                          NativeExecutionFailpoint failpoint,
                                          NativeRollbackFailpoint rollback_failpoint)
{
  runTransaction(transaction, true, failpoint, rollback_failpoint);
}
void NativeCandidateExecutor::prepareFullGraph(NativeEditTransaction& transaction)
{
  runTransaction(transaction, false, NativeExecutionFailpoint::kNone, NativeRollbackFailpoint::kNone);
}
void NativeCandidateExecutor::runTransaction(NativeEditTransaction& transaction, bool cpu_timing,
                                             NativeExecutionFailpoint failpoint,
                                             NativeRollbackFailpoint rollback_failpoint)
{
  if (!transaction.active())
    throw std::invalid_argument("native computation requires an active edit transaction");
  if (!incregpusta_debug_checks_enabled
      && (failpoint != NativeExecutionFailpoint::kNone || rollback_failpoint != NativeRollbackFailpoint::kNone))
    throw std::invalid_argument("native fault injection requires ECC_INCREGPUSTA_DEBUG_CHECKS=ON");
  if (!isKnownExecutionFailpoint(failpoint))
    throw std::invalid_argument("native computation received an unknown execution failpoint");
  if (!isKnownRollbackFailpoint(rollback_failpoint))
    throw std::invalid_argument("native computation received an unknown rollback failpoint");
  try {
    inject(failpoint, NativeExecutionFailpoint::kAfterInstall);
    inject(failpoint, NativeExecutionFailpoint::kBeforeDelayRuntimeInit);
    STADC.init();
    if (cpu_timing) {
      runFourTimingStages(failpoint);
    } else {
      runStage(failpoint, NativeExecutionFailpoint::kBeforeGraphBuilderInit, NativeExecutionFailpoint::kBeforeGraphBuilderWork,
               NativeExecutionFailpoint::kBeforeGraphBuilderDestroy, [] { GraphBuilder::initInst(); }, [] { STAGB.buildStructure(); },
               [] { GraphBuilder::destroyInst(); });
    }
    inject(failpoint, NativeExecutionFailpoint::kBeforeValidation);
    if constexpr (incregpusta_debug_checks_enabled)
      if (cpu_timing)
        requireValidNativeDatabase(transaction.database());
    inject(failpoint, NativeExecutionFailpoint::kBeforeDelayRuntimeDestroy);
    STADC.destroy();
    inject(failpoint, NativeExecutionFailpoint::kBeforePersistentDelayRestore);
  } catch (...) {
    const auto original = std::current_exception();
    const auto failure = transaction.rollbackWithDiagnostics(rollback_failpoint);
    if (failure == NativeRollbackFailure::kNone)
      std::rethrow_exception(original);
    throw NativeExecutionRollbackError(failpoint, failure, original);
  }
}

}  // namespace ista::timing_edit
