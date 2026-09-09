// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>

#include "NativeDatabaseCandidate.hpp"
#include "TimingEditTypes.hpp"

namespace ista {
class StaTimingEditSessionProbeAccess;
}

namespace ista::timing_edit {

using NativeExecutionScopeViolationWriter = void (*)(void*) noexcept;

struct NativeExecutionScopeViolationSignal
{
  void* context{nullptr};
  NativeExecutionScopeViolationWriter notify{nullptr};

  [[nodiscard]] constexpr bool valid() const noexcept
  {
    return context != nullptr && notify != nullptr;
  }
  void invoke() const noexcept { notify(context); }
};

class NativeExecutionScopeState;

enum class NativeExecutionFailpoint : std::uint8_t
{
  kNone,
  kAfterInstall,
  kBeforeDelayRuntimeInit,
  kBeforeGraphBuilderInit,
  kBeforeGraphBuilderWork,
  kBeforeGraphBuilderDestroy,
  kBeforeClockPropagatorInit,
  kBeforeClockPropagatorWork,
  kBeforeClockPropagatorDestroy,
  kBeforeTimingPropagatorInit,
  kBeforeTimingPropagatorWork,
  kBeforeTimingPropagatorDestroy,
  kBeforeTimingAnalyzerInit,
  kBeforeTimingAnalyzerWork,
  kBeforeTimingAnalyzerDestroy,
  kBeforeValidation,
  kBeforeDelayRuntimeDestroy,
  kBeforePersistentDelayRestore
};

// Test-only fault injection for the explicit rollback performed after a
// candidate execution failure. Production callers leave this at kNone.
enum class NativeRollbackFailpoint : std::uint8_t
{
  kNone,
  // Recorded after the old Database is exchanged back, before persistent
  // DelayCalculator restoration.
  kDelayRuntimeCleanupAfterDatabaseRestore,
  // Recorded after the persistent DelayCalculator restoration attempt.
  kPersistentDelayRestoreAfterAttempt,
  kCleanupAndPersistentRestore
};

enum class NativeRollbackFailure : std::uint8_t
{
  kNone,
  kDelayRuntimeCleanup,
  kPersistentDelayRestore,
  kDelayRuntimeCleanupAndPersistentDelayRestore,
  kOutsideOperationGuardScope,
  kDiagnosticsDisabled
};

// installAndExecute throws this only when candidate execution has already
// failed and the explicit rollback cannot restore every runtime invariant.
// The original execution exception is retained verbatim for diagnosis. The
// operation-guard owner must map this typed failure to a failed precommit
// rollback and poison both the coordinator and its session.
class NativeExecutionRollbackError final : public std::runtime_error
{
 public:
  NativeExecutionRollbackError(NativeExecutionFailpoint execution_failpoint,
                               NativeRollbackFailure rollback_failure,
                               std::exception_ptr original_failure);

  [[nodiscard]] NativeExecutionFailpoint executionFailpoint() const noexcept
  {
    return _execution_failpoint;
  }
  [[nodiscard]] NativeRollbackFailure rollbackFailure() const noexcept
  {
    return _rollback_failure;
  }
  [[nodiscard]] std::exception_ptr originalFailure() const noexcept
  {
    return _original_failure;
  }
  [[noreturn]] void rethrowOriginalFailure() const;

 private:
  NativeExecutionFailpoint _execution_failpoint;
  NativeRollbackFailure _rollback_failure;
  std::exception_ptr _original_failure;
};

// A capability created only by the operation-guard owner. Merely reaching the
// native adapter is insufficient authority to exchange the global Database.
class NativeExecutionAuthority final
{
 public:
  NativeExecutionAuthority(const NativeExecutionAuthority&) = delete;
  NativeExecutionAuthority& operator=(const NativeExecutionAuthority&) = delete;
  NativeExecutionAuthority(NativeExecutionAuthority&&) = delete;
  NativeExecutionAuthority& operator=(NativeExecutionAuthority&&) = delete;
  ~NativeExecutionAuthority() noexcept;

  [[nodiscard]] TimingEditSessionToken sessionToken() const noexcept { return _session_token; }
  [[nodiscard]] bool active() const noexcept;

 private:
  NativeExecutionAuthority(TimingEditSessionToken session_token,
                           NativeExecutionScopeViolationSignal violation_signal);

  TimingEditSessionToken _session_token;
  std::shared_ptr<NativeExecutionScopeState> _scope_state;

  friend class ::ista::StaTimingEditSessionProbeAccess;
  friend class InstalledNativeDatabase;
  friend class NativeCandidateExecutor;
};

class NativeCandidateExecutor final
{
 public:
  [[nodiscard]] static InstalledNativeDatabase installPreparedGraph(const NativeExecutionAuthority& authority, NativeDatabaseCandidate&& candidate);

  // Prepare canonical graph/library state without executing CPU timing.
  [[nodiscard]] static InstalledNativeDatabase installAndPrepareGraph(const NativeExecutionAuthority& authority, NativeDatabaseCandidate&& candidate);

  [[nodiscard]] static InstalledNativeDatabase installAndExecute(
      const NativeExecutionAuthority& authority, NativeDatabaseCandidate&& candidate,
      NativeExecutionFailpoint failpoint = NativeExecutionFailpoint::kNone,
      NativeRollbackFailpoint rollback_failpoint = NativeRollbackFailpoint::kNone);

 private:
  enum class Execution
  {
    kCpuTiming,
    kGraphPreparation,
    kPreparedGraph
  };
  [[nodiscard]] static InstalledNativeDatabase installAndRun(const NativeExecutionAuthority& authority, NativeDatabaseCandidate&& candidate,
                                                             Execution execution, NativeExecutionFailpoint failpoint,
                                                             NativeRollbackFailpoint rollback_failpoint);
};

}  // namespace ista::timing_edit
