// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "IncrementalStaSession.hpp"
#include "NativeCandidateExecutor.hpp"
#include "STAInterface.hpp"
#include "StaSessionApiGuard.hpp"

namespace ista {
// Internal authority for one batch/query. End the native execution scope before
// dropping the global operation guard and the session API lock.
class StaEditOperation final
{
 public:
  StaEditOperation(const StaEditOperation&) = delete;
  StaEditOperation& operator=(const StaEditOperation&) = delete;
  [[nodiscard]] StaCsrSessionContext& context() const noexcept;
  [[nodiscard]] const timing_edit::NativeExecutionAuthority& authority() const noexcept { return _authority; }
  [[nodiscard]] std::uint64_t token() const noexcept { return _authority.sessionToken().value(); }

 private:
  friend class IncrementalStaSession;
  friend class StaTimingEditSessionProbeAccess;
  explicit StaEditOperation(IncrementalStaSession& session);
  static STAInterface::OperationGuard acquire(IncrementalStaSession& session, bool api_acquired);
  static void poison(void* session) noexcept;

  SessionApiGuard _api_guard;
  IncrementalStaSession& _session;
  STAInterface::OperationGuard _operation_guard;
  timing_edit::NativeExecutionAuthority _authority;
};
}  // namespace ista
