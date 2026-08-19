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

#include <atomic>
#include <utility>

#include "STAInterface.hpp"
#include "StaLifecycleGuard.hpp"

namespace ista {
namespace {

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

class ProductionStaSessionOperations final : public StaSessionOperations
{
 public:
  StaStatus initialize(uint64_t token, std::map<std::string, std::any>& config_map) override
  {
    return STAInterface::getInst().initializeSession(token, config_map);
  }

  StaStatus runFullBaseline(uint64_t token) override { return STAInterface::getInst().runSessionBaseline(token); }

  StaStatus close(uint64_t token) override { return STAInterface::getInst().closeSession(token); }
};

uint64_t nextToken()
{
  static std::atomic<uint64_t> token{1};
  return token.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

IncrementalStaSession::IncrementalStaSession(StaExecutionRequest request)
    : IncrementalStaSession(std::move(request), std::make_unique<ProductionStaSessionOperations>())
{
}

IncrementalStaSession::IncrementalStaSession(StaExecutionRequest request, std::unique_ptr<StaSessionOperations> operations)
    : _request(std::move(request)), _operations(std::move(operations)), _token(nextToken())
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

StaStatus IncrementalStaSession::initialize(std::map<std::string, std::any> config_map)
{
  if (_state != StaSessionState::kCreated) {
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, _state, "STA session initialize requires created state");
  }
  StaExecutionResult resolution = resolveStage1StaExecution(_request);
  if (!resolution.ok()) {
    _state = StaSessionState::kClosed;
    return withCurrentState(std::move(resolution.status));
  }
  _resolved_record = std::move(resolution.record);
  StaStatus status;
  try {
    status = _operations->initialize(_token, config_map);
  } catch (...) {
    const std::string primary_error = exceptionMessage();
    StaStatus cleanup = StaStatus::success(StaSessionState::kClosed);
    try {
      cleanup = _operations->close(_token);
    } catch (...) {
      cleanup = StaStatus::failure(StaSessionStatusCode::kCleanupFailed, StaSessionState::kClosed, exceptionMessage());
    }
    _resolved_record.reset();
    _state = StaSessionState::kClosed;
    if (!cleanup.ok()) {
      return StaStatus::failure(StaSessionStatusCode::kCleanupFailed, _state,
                                "STA initialization threw: " + primary_error + "; cleanup failed: " + cleanup.message);
    }
    return StaStatus::failure(StaSessionStatusCode::kInitFailed, _state, "STA initialization threw: " + primary_error);
  }
  if (!status.ok()) {
    _resolved_record.reset();
    _state = StaSessionState::kClosed;
    return withCurrentState(std::move(status));
  }
  _state = StaSessionState::kInitialized;
  return StaStatus::success(_state);
}

StaExecutionResult IncrementalStaSession::runFullBaseline()
{
  if (_state != StaSessionState::kInitialized) {
    return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kInvalidState, _state, "STA baseline requires initialized state"));
  }
  StaStatus status;
  try {
    status = _operations->runFullBaseline(_token);
  } catch (...) {
    _state = StaSessionState::kPoisoned;
    return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kRunFailed, _state, "STA baseline threw: " + exceptionMessage()));
  }
  if (!status.ok()) {
    _state = StaSessionState::kPoisoned;
    return StaExecutionResult::failure(withCurrentState(std::move(status)));
  }
  if (!_resolved_record) {
    _state = StaSessionState::kPoisoned;
    return StaExecutionResult::failure(StaStatus::failure(StaSessionStatusCode::kRunFailed, _state, "STA execution record was not resolved"));
  }
  _state = StaSessionState::kBaselineExecuted;
  return StaExecutionResult::success(*_resolved_record, _state);
}

StaStatus IncrementalStaSession::close()
{
  if (_state == StaSessionState::kClosed)
    return StaStatus::success(_state);
  if (_state == StaSessionState::kCreated) {
    _state = StaSessionState::kClosed;
    return StaStatus::success(_state);
  }
  StaStatus status;
  try {
    status = _operations->close(_token);
  } catch (...) {
    status = StaStatus::failure(StaSessionStatusCode::kCleanupFailed, StaSessionState::kClosed, "STA session close threw: " + exceptionMessage());
  }
  _state = StaSessionState::kClosed;
  return withCurrentState(std::move(status));
}

StaStatus IncrementalStaSession::withCurrentState(StaStatus status) const
{
  status.state = _state;
  return status;
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
