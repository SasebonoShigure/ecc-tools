// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <mutex>
#include "STAInterface.hpp"
namespace ista {
class IncrementalStaSession;
inline thread_local const IncrementalStaSession* current_session_api = nullptr;
class SessionApiGuard
{
 public:
  SessionApiGuard(const IncrementalStaSession* session, std::mutex& mutex) : _session(session), _lock(mutex, std::defer_lock)
  {
    // Reject global -> API recursion before waiting: another thread may hold
    // the API lock while waiting for the global operation lock (for example close).
    if (current_session_api == session || STAInterface::operationSentinelActive())
      return;
    _lock.lock();
    _previous = current_session_api;
    current_session_api = session;
    _acquired = true;
  }

  SessionApiGuard(const SessionApiGuard&) = delete;
  SessionApiGuard(SessionApiGuard&&) = delete;
  SessionApiGuard& operator=(const SessionApiGuard&) = delete;
  SessionApiGuard& operator=(SessionApiGuard&&) = delete;

  ~SessionApiGuard()
  {
    if (_acquired)
      current_session_api = _previous;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return _acquired; }

 private:
  const IncrementalStaSession* _session;
  const IncrementalStaSession* _previous{nullptr};
  std::unique_lock<std::mutex> _lock;
  bool _acquired{false};
};

}  // namespace ista
