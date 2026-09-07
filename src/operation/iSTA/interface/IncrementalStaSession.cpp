// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "IncrementalStaSession.hpp"

#include <exception>
#include <utility>

#include "CsrReferenceValidator.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "STAInterface.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaLifecycleGuard.hpp"

namespace ista {
namespace {

thread_local const IncrementalStaSession* current_session_api = nullptr;

std::string exceptionMessage()
{
  try {
    throw;
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "unknown native exception";
  }
}

class SessionApiGuard
{
 public:
  SessionApiGuard(const IncrementalStaSession* session, std::mutex& mutex) : _session(session), _lock(mutex, std::defer_lock)
  {
    if (current_session_api == session)
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

StaCsrReferenceAttempt invalidReferenceAttempt()
{
  return {.value = StaCsrSessionOperationFailure{}};
}

StaCsrScatterAttempt invalidScatterAttempt()
{
  csr::CsrTimingResultScatterAttempt attempt;
  attempt.scatter_result = {.code = csr::CsrScatterCode::kInvalidState, .writes_applied = 0, .invalidation_reason = std::nullopt};
  return {.value = std::move(attempt)};
}

class ProductionStaSessionOperations final : public StaSessionOperations
{
 public:
  StaStatus initialize(std::uint64_t token, std::map<std::string, std::any>& config_map) override
  {
    return STAInterface::getInst().initializeSession(token, config_map);
  }

  StaStatus runFullBaseline(std::uint64_t token, StaCsrSessionContext* capture_or_null) override
  {
    return STAInterface::getInst().runSessionBaseline(token, capture_or_null);
  }

  StaStatus close(std::uint64_t token, StaCsrSessionContext* context, std::unique_ptr<csr::CsrSnapshotObservation>* released_out) override
  {
    return STAInterface::getInst().closeSession(token, context, released_out);
  }

  StaStatus copyObservation(std::uint64_t token, StaCsrSessionContext* context, std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) override
  {
    return STAInterface::getInst().copySessionCsrSnapshotObservation(token, context, detached_out);
  }

  StaCsrReferenceAttempt computeCsrReferenceForTesting(std::uint64_t token, StaCsrSessionContext* context) override
  {
    return STAInterface::getInst().computeSessionCsrReferenceForTesting(token, context);
  }

  StaCsrScatterAttempt scatterCsrTimingResultForTesting(std::uint64_t token, StaCsrSessionContext* context, const csr::CsrTimingResult& result) override
  {
    return STAInterface::getInst().scatterSessionCsrTimingResultForTesting(token, context, result);
  }
};

}  // namespace

IncrementalStaSession::IncrementalStaSession(StaExecutionRequest request)
    : IncrementalStaSession(std::move(request), std::make_unique<ProductionStaSessionOperations>())
{
}

IncrementalStaSession::IncrementalStaSession(StaExecutionRequest request, std::unique_ptr<StaSessionOperations> operations)
    : _request(std::move(request)), _operations(std::move(operations))
{
  if (!_operations)
    throw std::invalid_argument("IncrementalStaSession requires operations");
}

IncrementalStaSession::~IncrementalStaSession() noexcept
{
  try {
    StaStatus status = close();
    if (!status.ok())
      emitStaLifecycleDiagnostic("IncrementalStaSession cleanup failed: ", status.message);
  } catch (const std::exception& error) {
    emitStaLifecycleDiagnostic("IncrementalStaSession cleanup failed: ", error.what());
  } catch (...) {
    emitStaLifecycleDiagnostic("IncrementalStaSession cleanup failed: ", "unknown native exception");
  }
}

StaStatus IncrementalStaSession::initialize(const csr::StaAnalysisContext& analysis_context, std::map<std::string, std::any> config_map)
{
  SessionApiGuard guard(this, _api_mutex);
  if (!guard)
    return invalidState("reentrant STA session initialize is not allowed");
  if (state() != StaSessionState::kCreated)
    return invalidState("STA session initialize requires created state");

  const auto context_errors = csr::validateAnalysisContextIntrinsic(analysis_context);
  if (!context_errors.empty()) {
    const auto& error = context_errors.front();
    return StaStatus::failure(StaSessionStatusCode::kInitFailed, state(), error.field + ": " + error.message);
  }

  StaExecutionResult resolution = resolveStage1StaExecution(_request);
  if (!resolution.ok()) {
    _state.store(StaSessionState::kClosed, std::memory_order_release);
    return withCurrentState(std::move(resolution.status));
  }
  _resolved_record = std::move(resolution.record);

  const std::optional<std::uint64_t> allocated_token = productionStaSessionTokenAllocator().allocate();
  if (!allocated_token) {
    _resolved_record.reset();
    _state.store(StaSessionState::kClosed, std::memory_order_release);
    return StaStatus::failure(StaSessionStatusCode::kInitFailed, state(), "STA session token allocator is exhausted");
  }
  _token.store(*allocated_token, std::memory_order_release);

  try {
    _context = std::make_unique<StaCsrSessionContext>(analysis_context, *allocated_token);
  } catch (...) {
    _resolved_record.reset();
    _state.store(StaSessionState::kClosed, std::memory_order_release);
    return StaStatus::failure(StaSessionStatusCode::kInitFailed, state(), "STA session context construction threw: " + exceptionMessage());
  }

  StaStatus status;
  try {
    status = _operations->initialize(*allocated_token, config_map);
  } catch (...) {
    const std::string primary_error = exceptionMessage();
    std::unique_ptr<csr::CsrSnapshotObservation> released;
    StaStatus cleanup = StaStatus::success(StaSessionState::kClosed);
    std::exception_ptr cleanup_exception;
    try {
      cleanup = _operations->close(*allocated_token, _context.get(), &released);
    } catch (...) {
      cleanup_exception = std::current_exception();
    }
    if (released)
      _released_observation = std::move(released);
    _resolved_record.reset();
    _state.store(StaSessionState::kClosed, std::memory_order_release);

    if (cleanup_exception) {
      try {
        std::rethrow_exception(cleanup_exception);
      } catch (...) {
        return StaStatus::failure(StaSessionStatusCode::kCleanupFailed, state(),
                                  "STA initialization threw: " + primary_error + "; cleanup failed: " + exceptionMessage());
      }
    }

    if (_released_observation) {
      if (const auto error = observationOwnershipError(*_released_observation)) {
        cleanup = StaStatus::failure(StaSessionStatusCode::kCleanupFailed, StaSessionState::kClosed, *error);
      }
    }

    if (!cleanup.ok()) {
      return StaStatus::failure(StaSessionStatusCode::kCleanupFailed, state(),
                                "STA initialization threw: " + primary_error + "; cleanup failed: " + cleanup.message);
    }
    return StaStatus::failure(StaSessionStatusCode::kInitFailed, state(), "STA initialization threw: " + primary_error);
  }
  if (!status.ok()) {
    _resolved_record.reset();
    _state.store(StaSessionState::kClosed, std::memory_order_release);
    return withCurrentState(std::move(status));
  }

  _state.store(StaSessionState::kInitialized, std::memory_order_release);
  return StaStatus::success(state());
}

StaExecutionResult IncrementalStaSession::runFullBaseline()
{
  SessionApiGuard guard(this, _api_mutex);
  if (!guard)
    return StaExecutionResult::failure(invalidState("reentrant STA baseline is not allowed"));
  if (state() != StaSessionState::kInitialized)
    return StaExecutionResult::failure(invalidState("STA baseline requires initialized state"));

  StaStatus status;
  try {
    status = _operations->runFullBaseline(_token.load(std::memory_order_acquire), isIncrementalRequest() ? _context.get() : nullptr);
  } catch (...) {
    _state.store(StaSessionState::kPoisoned, std::memory_order_release);
    return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), "STA baseline threw: " + exceptionMessage()));
  }
  if (!status.ok()) {
    _state.store(StaSessionState::kPoisoned, std::memory_order_release);
    return StaExecutionResult::failure(withCurrentState(std::move(status)));
  }
  if (!_resolved_record) {
    _state.store(StaSessionState::kPoisoned, std::memory_order_release);
    return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), "STA execution record was not resolved"));
  }

  if (isIncrementalRequest() && !incregpusta_debug_checks_enabled && (!_context || !_context->hasReadySnapshot())) {
    _state.store(StaSessionState::kPoisoned, std::memory_order_release);
    return StaExecutionResult::failure(
        StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), "incremental STA capture did not publish a ready snapshot"));
  }
  if (isIncrementalRequest() && incregpusta_debug_checks_enabled) {
    std::unique_ptr<csr::CsrSnapshotObservation> active;
    try {
      status = _operations->copyObservation(_token.load(std::memory_order_acquire), _context.get(), &active);
    } catch (...) {
      _state.store(StaSessionState::kPoisoned, std::memory_order_release);
      return StaExecutionResult::failure(
          StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), "STA snapshot observation read threw: " + exceptionMessage()));
    }
    if (!status.ok() || !active) {
      _state.store(StaSessionState::kPoisoned, std::memory_order_release);
      const std::string message = status.ok() ? "incremental STA capture did not publish a ready observation" : status.message;
      return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), message));
    }
    if (const auto error = observationOwnershipError(*active)) {
      _state.store(StaSessionState::kPoisoned, std::memory_order_release);
      return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), *error));
    }
  }

  _baseline_executed = true;
  _state.store(StaSessionState::kBaselineExecuted, std::memory_order_release);
  return StaExecutionResult::success(*_resolved_record, state());
}

StaStatus IncrementalStaSession::close()
{
  SessionApiGuard guard(this, _api_mutex);
  if (!guard)
    return invalidState("reentrant STA session close is not allowed");
  if (state() == StaSessionState::kClosed)
    return StaStatus::success(state());
  if (state() == StaSessionState::kCreated) {
    _state.store(StaSessionState::kClosed, std::memory_order_release);
    return StaStatus::success(state());
  }

  std::unique_ptr<csr::CsrSnapshotObservation> released;
  StaStatus status;
  std::exception_ptr close_exception;
  try {
    status = _operations->close(_token.load(std::memory_order_acquire), _context.get(), &released);
  } catch (...) {
    close_exception = std::current_exception();
  }

  if (released)
    _released_observation = std::move(released);
  _state.store(StaSessionState::kClosed, std::memory_order_release);

  if (close_exception) {
    try {
      std::rethrow_exception(close_exception);
    } catch (...) {
      return StaStatus::failure(StaSessionStatusCode::kCleanupFailed, StaSessionState::kClosed, "STA session close threw: " + exceptionMessage());
    }
  }

  if (_released_observation) {
    if (const auto error = observationOwnershipError(*_released_observation)) {
      const std::string combined = status.message.empty() ? *error : status.message + "; " + *error;
      status = StaStatus::failure(StaSessionStatusCode::kCleanupFailed, StaSessionState::kClosed, combined);
    }
  }

  return withCurrentState(std::move(status));
}

StaCsrReferenceAttempt IncrementalStaSession::computeCsrReferenceForTesting()
{
  SessionApiGuard guard(this, _api_mutex);
  if (!guard || !_baseline_executed || state() != StaSessionState::kBaselineExecuted || !isIncrementalRequest() || !_context)
    return invalidReferenceAttempt();
  return _operations->computeCsrReferenceForTesting(_token.load(std::memory_order_acquire), _context.get());
}

StaCsrScatterAttempt IncrementalStaSession::scatterCsrTimingResultForTesting(const csr::CsrTimingResult& result)
{
  SessionApiGuard guard(this, _api_mutex);
  if (!guard || !_baseline_executed || state() != StaSessionState::kBaselineExecuted || !isIncrementalRequest() || !_context)
    return invalidScatterAttempt();
  StaCsrScatterAttempt attempt = _operations->scatterCsrTimingResultForTesting(_token.load(std::memory_order_acquire), _context.get(), result);
  if (attempt.value.scatter_result.code == csr::CsrScatterCode::kSnapshotInvalidated)
    _state.store(StaSessionState::kPoisoned, std::memory_order_release);
  return attempt;
}

std::unique_ptr<csr::CsrSnapshotObservation> IncrementalStaSession::snapshotObservation()
{
  SessionApiGuard guard(this, _api_mutex);
  if (!guard)
    throw StaSessionException(invalidState("reentrant STA snapshot observation read is not allowed"), _request);

  if (state() == StaSessionState::kClosed) {
    if (!_released_observation)
      return nullptr;
    if (const auto error = observationOwnershipError(*_released_observation))
      throw std::logic_error(*error);
    return std::make_unique<csr::CsrSnapshotObservation>(*_released_observation);
  }

  if (!_baseline_executed || !isIncrementalRequest())
    return nullptr;

  std::unique_ptr<csr::CsrSnapshotObservation> detached;
  StaStatus status;
  try {
    status = _operations->copyObservation(_token.load(std::memory_order_acquire), _context.get(), &detached);
  } catch (...) {
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kInvalidState, state(), "STA snapshot observation read threw: " + exceptionMessage()),
                              _request);
  }
  throwOnStaFailure(status, _request);
  if (detached) {
    if (const auto error = observationOwnershipError(*detached))
      throw std::logic_error(*error);
  }
  return detached;
}

std::optional<std::uint64_t> IncrementalStaSession::sessionToken() const noexcept
{
  const std::uint64_t token = _token.load(std::memory_order_acquire);
  if (token == 0)
    return std::nullopt;
  return token;
}

StaStatus IncrementalStaSession::withCurrentState(StaStatus status) const
{
  status.state = state();
  return status;
}

StaStatus IncrementalStaSession::invalidState(std::string message) const
{
  return StaStatus::failure(StaSessionStatusCode::kInvalidState, state(), std::move(message));
}

bool IncrementalStaSession::isIncrementalRequest() const noexcept
{
  return _request.backend == StaBackend::kCpu && _request.update_strategy == StaUpdateStrategy::kIncremental;
}

std::optional<std::string> IncrementalStaSession::observationOwnershipError(const csr::CsrSnapshotObservation& observation) const
{
  const std::uint64_t token = _token.load(std::memory_order_acquire);
  if (token == 0)
    return "STA snapshot observation exists before session token allocation";
  if (!_context)
    return "STA snapshot observation exists without an immutable analysis context";
  if (_context->token() != token)
    return "STA snapshot context token differs from the session token";
  if (observation.identity.session_token != token)
    return "STA snapshot observation token differs from the session token";
  return std::nullopt;
}

StaSessionException::StaSessionException(StaStatus status, StaExecutionRequest request)
    : std::runtime_error(status.message), _status(std::move(status)), _request(std::move(request))
{
}

void throwOnStaFailure(const StaStatus& status, const StaExecutionRequest& request)
{
  if (!status.ok())
    throw StaSessionException(status, request);
}

StaExecutionRecord requireStaExecutionRecord(StaExecutionResult result, const StaExecutionRequest& request)
{
  if (!result.ok())
    throw StaSessionException(std::move(result.status), request);
  return std::move(*result.record);
}

}  // namespace ista
