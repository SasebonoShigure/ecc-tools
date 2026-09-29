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
#include <cmath>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

#include "CsrSessionSnapshotAdapter.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "DataManager.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "Logger.hpp"
#include "STAHeader.hpp"
#include "STAInterface.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaProductionLifecycle.hpp"

namespace ista {
namespace {

thread_local STAInterface* current_operation_interface = nullptr;

std::string currentExceptionMessage()
{
  try {
    throw;
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "unknown native exception";
  }
}

template <typename Callable>
StaOperationResult invokeLifecycleOperation(const char* operation, Callable&& callable)
{
  try {
    return callable();
  } catch (...) {
    return StaOperationResult::cleanupFailure(std::string(operation) + " threw before cleanup could be confirmed: " + currentExceptionMessage());
  }
}

[[nodiscard]] StaLeaseResult requireUse(const StaOwnerLease& lifecycle, StaOwner owner)
{
  return owner.kind == StaOwnerKind::kLegacy ? lifecycle.requireLegacyUse() : lifecycle.requireSessionUse(owner.token);
}

[[nodiscard]] StaLeaseResult requireClose(const StaOwnerLease& lifecycle, StaOwner owner)
{
  return owner.kind == StaOwnerKind::kLegacy ? lifecycle.requireLegacyClose() : lifecycle.requireSessionClose(owner.token);
}

[[nodiscard]] StaCsrReferenceAttempt invalidReferenceAttempt() noexcept
{
  return StaCsrReferenceAttempt{std::variant<csr::CsrComputationResult, StaCsrSessionOperationFailure>{StaCsrSessionOperationFailure{}}};
}

[[nodiscard]] StaCsrScatterAttempt invalidScatterAttempt() noexcept
{
  return StaCsrScatterAttempt{csr::CsrTimingResultScatterAttempt{csr::CsrScatterResult{csr::CsrScatterCode::kInvalidState, 0, std::nullopt}, {}, std::nullopt}};
}

}  // namespace

STAInterface::OperationGuard::OperationGuard(STAInterface* interface, std::unique_lock<std::mutex> lock) noexcept
    : _interface(interface), _lock(std::move(lock))
{
  current_operation_interface = interface;
}

STAInterface::OperationGuard::OperationGuard(OperationGuard&& other) noexcept
    : _interface(other._interface), _lock(std::move(other._lock)), _owns_sentinel(other._owns_sentinel)
{
  other._owns_sentinel = false;
}

STAInterface::OperationGuard::~OperationGuard() noexcept
{
  if (_owns_sentinel)
    current_operation_interface = nullptr;
}

STAInterface::ClosePendingReservation::ClosePendingReservation(STAInterface* interface, StaOwner owner) noexcept : _interface(interface), _owner(owner)
{
  _interface->_close_pending = owner;
}

STAInterface::ClosePendingReservation::ClosePendingReservation(ClosePendingReservation&& other) noexcept
    : _interface(other._interface), _owner(other._owner), _active(other._active)
{
  other._active = false;
}

STAInterface::ClosePendingReservation::~ClosePendingReservation() noexcept
{
  if (!_active)
    return;
  try {
    std::scoped_lock lock(_interface->_lifecycle_mutex);
    if (_interface->_close_pending == _owner)
      _interface->_close_pending = StaOwner::none();
  } catch (...) {
    emitStaLifecycleDiagnostic("STA close-pending rollback failed: ", currentExceptionMessage());
  }
}

void STAInterface::ClosePendingReservation::commit() noexcept
{
  if (!_active)
    return;
  std::scoped_lock lock(_interface->_lifecycle_mutex);
  if (_interface->_close_pending == _owner)
    _interface->_close_pending = StaOwner::none();
  _active = false;
}

bool STAInterface::operationSentinelActive() noexcept
{
  return current_operation_interface != nullptr;
}

StaStatus STAInterface::leaseFailure(const StaLeaseResult& lease, StaSessionState state) const
{
  return StaStatus::failure(lease.quarantined ? StaSessionStatusCode::kQuarantined : StaSessionStatusCode::kOwnerConflict, state, lease.message);
}

std::optional<STAInterface::OperationGuard> STAInterface::acquireUse(StaOwner owner, const StaCsrSessionContext* context, bool context_required,
                                                                     StaSessionState failure_state, StaStatus* failure)
{
  if (operationSentinelActive()) {
    *failure = StaStatus::failure(StaSessionStatusCode::kInvalidState, failure_state, "recursive STA operation is not allowed");
    return std::nullopt;
  }

  {
    std::scoped_lock lifecycle_lock(_lifecycle_mutex);
    const StaLeaseResult lease = requireUse(_lifecycle, owner);
    if (!lease.succeeded) {
      *failure = leaseFailure(lease, failure_state);
      return std::nullopt;
    }
    if (_close_pending.kind != StaOwnerKind::kNone) {
      *failure = StaStatus::failure(StaSessionStatusCode::kInvalidState, failure_state, "STA close is pending");
      return std::nullopt;
    }
  }

  std::unique_lock operation_lock(_operation_mutex);
  std::optional<OperationGuard> guard{std::in_place, this, std::move(operation_lock)};
  {
    std::scoped_lock lifecycle_lock(_lifecycle_mutex);
    const StaLeaseResult lease = requireUse(_lifecycle, owner);
    if (!lease.succeeded) {
      *failure = leaseFailure(lease, failure_state);
      return std::nullopt;
    }
    if (_close_pending.kind != StaOwnerKind::kNone) {
      *failure = StaStatus::failure(StaSessionStatusCode::kInvalidState, failure_state, "STA close is pending");
      return std::nullopt;
    }
    if (context_required && context == nullptr) {
      *failure = StaStatus::failure(StaSessionStatusCode::kInvalidState, failure_state, "STA session operation requires a context");
      return std::nullopt;
    }
    if (context != nullptr && (owner.kind != StaOwnerKind::kSession || context->token() != owner.token)) {
      *failure = StaStatus::failure(StaSessionStatusCode::kInvalidState, failure_state, "STA session context token differs from the active owner");
      return std::nullopt;
    }
  }
  return guard;
}

void STAInterface::initSTA(std::map<std::string, std::any> config_map)
{
  StaStatus status = initializeOwned(StaOwner::legacy(), config_map);
  if (!status.ok())
    throw std::runtime_error(status.message);
}

void STAInterface::runSTA()
{
  StaStatus status = runOwned(StaOwner::legacy(), nullptr);
  if (!status.ok())
    throw std::runtime_error(status.message);
}

void STAInterface::extractLib()
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::legacy(), nullptr, false, StaSessionState::kInitialized, &failure);
  if (!authority)
    throw std::runtime_error(failure.message);
  StaOperationResult result = invokeLifecycleOperation("STA extract", [this] { return _lifecycle_operations->extractLib(); });
  if (!result.succeeded) {
    if (result.cleanup_failed) {
      std::scoped_lock lock(_lifecycle_mutex);
      _lifecycle.quarantineCurrent();
    }
    throw std::runtime_error(result.message);
  }
}

void STAInterface::destroySTA()
{
  std::unique_ptr<csr::CsrSnapshotObservation> released;
  StaStatus status = closeOwned(StaOwner::legacy(), nullptr, &released);
  if (!status.ok())
    throw std::runtime_error(status.message);
}

bool STAInterface::updateTiming(std::string& error_message)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::legacy(), nullptr, false, StaSessionState::kInitialized, &failure);
  if (!authority) {
    error_message = failure.message;
    return false;
  }
  if (!isSTAInitialized(error_message))
    return false;
  if (_is_timing_updated) {
    error_message = "update_timing may only be called once after init_sta";
    return false;
  }
  StaOperationResult result = invokeLifecycleOperation("STA timing update", [this] { return _lifecycle_operations->updateTiming(); });
  if (!result.succeeded) {
    error_message = result.message;
    if (result.cleanup_failed) {
      std::scoped_lock lock(_lifecycle_mutex);
      _lifecycle.quarantineCurrent();
    }
    return false;
  }
  _is_timing_updated = true;
  return true;
}

bool STAInterface::writeSDF(const std::string& file_path, std::string& error_message)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::legacy(), nullptr, false, StaSessionState::kInitialized, &failure);
  if (!authority) {
    error_message = failure.message;
    return false;
  }
  if (!isSTAInitialized(error_message))
    return false;
  if (!_is_timing_updated) {
    error_message = "update_timing must be called before write_sdf";
    return false;
  }
  if (file_path.empty()) {
    error_message = "write_sdf requires a non-empty output file path";
    return false;
  }
  StaOperationResult result = invokeLifecycleOperation("STA SDF write", [this, &file_path] { return _lifecycle_operations->writeSdf(file_path); });
  if (!result.succeeded) {
    error_message = result.message;
    if (result.cleanup_failed) {
      std::scoped_lock lock(_lifecycle_mutex);
      _lifecycle.quarantineCurrent();
    }
    return false;
  }
  return true;
}

bool STAInterface::reportTiming(std::string& error_message)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::legacy(), nullptr, false, StaSessionState::kInitialized, &failure);
  if (!authority) {
    error_message = failure.message;
    return false;
  }
  if (!isSTAInitialized(error_message))
    return false;
  if (!_is_timing_updated) {
    error_message = "update_timing must be called before report_timing";
    return false;
  }
  StaOperationResult result = invokeLifecycleOperation("STA timing report", [this] { return _lifecycle_operations->reportTiming(); });
  if (!result.succeeded) {
    error_message = result.message;
    if (result.cleanup_failed) {
      std::scoped_lock lock(_lifecycle_mutex);
      _lifecycle.quarantineCurrent();
    }
    return false;
  }
  return true;
}

bool STAInterface::createClock(const std::string& clock_name, const double period, const double rise_edge, const double fall_edge,
                               const std::vector<std::string>& source_list, std::string& error_message)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::legacy(), nullptr, false, StaSessionState::kInitialized, &failure);
  if (!authority) {
    error_message = failure.message;
    return false;
  }
  if (!isSTAInitialized(error_message))
    return false;
  if (clock_name.empty()) {
    error_message = "create_clock requires a non-empty -name";
    return false;
  }
  if (!std::isfinite(period) || period <= 0.0) {
    error_message = "create_clock -period must be a positive finite value";
    return false;
  }
  if (!std::isfinite(rise_edge) || !std::isfinite(fall_edge) || rise_edge < 0.0 || fall_edge < 0.0 || rise_edge >= fall_edge || fall_edge >= period) {
    error_message = "create_clock -waveform must satisfy 0 <= rise < fall < period";
    return false;
  }
  if (source_list.empty()) {
    error_message = "create_clock requires at least one source port";
    return false;
  }

  Database& database = STADM.getDatabase();
  auto& clock_map = database.get_timing_constraint().get_clock_map();
  if (clock_map.contains(clock_name))
    STALOG.warn(Loc::current(), "clock '", clock_name, "' already exists and will be overwritten");

  std::vector<std::string> unique_sources;
  std::set<std::string> seen;
  for (const std::string& source_name : source_list) {
    auto pin = database.get_pin_map().find(source_name);
    if (pin == database.get_pin_map().end()) {
      STALOG.warn(Loc::current(), "source '", source_name, "' not found");
      error_message = "clock source is not a top-level port";
      return false;
    }
    if (!pin->second.get_is_port()) {
      STALOG.warn(Loc::current(), "clock source '", source_name, "' found but is not a top-level port");
      error_message = "clock source is not a top-level port";
      return false;
    }
    if (seen.insert(source_name).second)
      unique_sources.push_back(source_name);
  }

  TimingClock clock;
  clock.set_clock_name(clock_name);
  clock.set_period(period);
  clock.set_rise_edge(rise_edge);
  clock.set_fall_edge(fall_edge);
  clock.set_source_list(unique_sources);
  clock.set_is_propagated(false);
  clock_map[clock_name] = clock;
  return true;
}

bool STAInterface::setPropagatedClock(const std::vector<std::string>& clock_names, std::string& error_message)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::legacy(), nullptr, false, StaSessionState::kInitialized, &failure);
  if (!authority) {
    error_message = failure.message;
    return false;
  }
  if (!isSTAInitialized(error_message))
    return false;
  if (clock_names.empty()) {
    error_message = "set_propagated_clock requires at least one clock";
    return false;
  }
  auto& clocks = STADM.getDatabase().get_timing_constraint().get_clock_map();
  for (const std::string& name : clock_names) {
    if (!clocks.contains(name)) {
      STALOG.warn(Loc::current(), "clock '", name, "' does not exist");
      error_message = "clock does not exist";
      return false;
    }
  }
  for (const std::string& name : clock_names)
    clocks[name].set_is_propagated(true);
  return true;
}

bool STAInterface::getPorts(const std::vector<std::string>& names, std::vector<std::string>& resolved, std::string& error_message)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::legacy(), nullptr, false, StaSessionState::kInitialized, &failure);
  if (!authority) {
    error_message = failure.message;
    return false;
  }
  if (!isSTAInitialized(error_message))
    return false;
  if (names.empty()) {
    error_message = "get_ports requires at least one port name";
    return false;
  }
  Database& database = STADM.getDatabase();
  resolved.clear();
  for (const std::string& name : names) {
    auto pin = database.get_pin_map().find(name);
    if (pin == database.get_pin_map().end()) {
      STALOG.warn(Loc::current(), "source '", name, "' not found");
      error_message = "clock source not found";
      return false;
    }
    if (!pin->second.get_is_port()) {
      STALOG.warn(Loc::current(), "clock source '", name, "' found but is not a top-level port");
      error_message = "clock source is not a top-level port";
      return false;
    }
    resolved.push_back(name);
  }
  return true;
}

bool STAInterface::getClocks(const std::vector<std::string>& names, std::vector<std::string>& resolved, std::string& error_message)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::legacy(), nullptr, false, StaSessionState::kInitialized, &failure);
  if (!authority) {
    error_message = failure.message;
    return false;
  }
  if (!isSTAInitialized(error_message))
    return false;
  if (names.empty()) {
    error_message = "get_clocks requires at least one clock name";
    return false;
  }
  auto& clocks = STADM.getDatabase().get_timing_constraint().get_clock_map();
  resolved.clear();
  for (const std::string& name : names) {
    if (!clocks.contains(name)) {
      STALOG.error(Loc::current(), "clock '", name, "' does not exist");
      error_message = "clock does not exist";
      return false;
    }
    resolved.push_back(name);
  }
  return true;
}

StaStatus STAInterface::initializeSession(uint64_t token, std::map<std::string, std::any>& config_map)
{
  return initializeOwned(StaOwner::session(token), config_map);
}

StaStatus STAInterface::runSessionBaseline(uint64_t token, StaCsrSessionContext* capture_or_null)
{
  return runOwned(StaOwner::session(token), capture_or_null);
}

StaStatus STAInterface::closeSession(uint64_t token, StaCsrSessionContext* context, std::unique_ptr<csr::CsrSnapshotObservation>* released_out)
{
  return closeOwned(StaOwner::session(token), context, released_out);
}

StaStatus STAInterface::initializeOwned(StaOwner owner, std::map<std::string, std::any>& config_map)
{
  if (operationSentinelActive())
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kCreated, "recursive STA initialize is not allowed");

  std::unique_lock operation_lock(_operation_mutex);
  OperationGuard authority(this, std::move(operation_lock));
  {
    std::scoped_lock lifecycle_lock(_lifecycle_mutex);
    StaLeaseResult lease = owner.kind == StaOwnerKind::kLegacy ? _lifecycle.acquireLegacy() : _lifecycle.acquireSession(owner.token);
    if (!lease.succeeded)
      return leaseFailure(lease, StaSessionState::kCreated);
  }

  StaOperationResult result;
  try {
    result = _lifecycle_operations->initialize(config_map);
  } catch (...) {
    const std::string primary = "STA initialization threw: " + currentExceptionMessage();
    StaOperationResult rollback = invokeLifecycleOperation("STA initialization rollback", [this] { return _lifecycle_operations->close(); });
    _is_initialized = false;
    _is_timing_updated = false;
    {
      std::scoped_lock lifecycle_lock(_lifecycle_mutex);
      if (rollback.succeeded)
        _lifecycle.releaseAfterRollback();
      else
        _lifecycle.quarantineAfterRollback();
    }
    if (!rollback.succeeded)
      return StaStatus::failure(StaSessionStatusCode::kCleanupFailed, StaSessionState::kClosed, primary + "; rollback cleanup failed: " + rollback.message);
    return StaStatus::failure(StaSessionStatusCode::kInitFailed, StaSessionState::kClosed, primary);
  }

  if (result.succeeded) {
    _is_initialized = true;
    _is_timing_updated = false;
    return StaStatus::success(StaSessionState::kInitialized);
  }
  {
    std::scoped_lock lifecycle_lock(_lifecycle_mutex);
    if (result.cleanup_failed)
      _lifecycle.quarantineAfterRollback();
    else
      _lifecycle.releaseAfterRollback();
  }
  return StaStatus::failure(result.cleanup_failed ? StaSessionStatusCode::kCleanupFailed : StaSessionStatusCode::kInitFailed, StaSessionState::kClosed,
                            result.message);
}

StaStatus STAInterface::runOwned(StaOwner owner, StaCsrSessionContext* capture_or_null, bool include_artifacts)
{
  StaStatus failure;
  auto authority = acquireUse(owner, capture_or_null, false, StaSessionState::kInitialized, &failure);
  if (!authority)
    return failure;
  if (!_is_initialized)
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kCreated, "init_sta must be called before STA commands");

  StaOperationResult result = invokeLifecycleOperation("STA full baseline", [this, include_artifacts] {
    return include_artifacts ? _lifecycle_operations->runFullBaseline() : runFullStaPipeline(false);
  });
  if (!result.succeeded) {
    if (result.cleanup_failed) {
      std::scoped_lock lifecycle_lock(_lifecycle_mutex);
      _lifecycle.quarantineCurrent();
    }
    return StaStatus::failure(result.cleanup_failed ? StaSessionStatusCode::kCleanupFailed : StaSessionStatusCode::kRunFailed, StaSessionState::kPoisoned,
                              result.message);
  }

  if (capture_or_null != nullptr)
    capture_or_null->_baseline_ready = true;
  return StaStatus::success(StaSessionState::kBaselineExecuted);
}

StaStatus STAInterface::closeOwned(StaOwner owner, StaCsrSessionContext* context, std::unique_ptr<csr::CsrSnapshotObservation>* released_out)
{
  if (operationSentinelActive())
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kClosed, "recursive STA close is not allowed");
  if (released_out == nullptr || *released_out)
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kClosed, "STA close requires a pre-cleared released observation output");

  std::optional<ClosePendingReservation> reservation;
  {
    std::scoped_lock lifecycle_lock(_lifecycle_mutex);
    if (owner.kind == StaOwnerKind::kLegacy && _lifecycle.isNone())
      return StaStatus::success(StaSessionState::kClosed);
    const StaLeaseResult lease = requireClose(_lifecycle, owner);
    if (!lease.succeeded)
      return leaseFailure(lease, StaSessionState::kClosed);
    if (_close_pending.kind != StaOwnerKind::kNone)
      return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kClosed, "STA close is already pending");
    reservation.emplace(this, owner);
  }

  std::unique_lock operation_lock(_operation_mutex);
  OperationGuard authority(this, std::move(operation_lock));
  {
    std::scoped_lock lifecycle_lock(_lifecycle_mutex);
    const StaLeaseResult lease = requireClose(_lifecycle, owner);
    if (!lease.succeeded)
      return leaseFailure(lease, StaSessionState::kClosed);
    if (!(_close_pending == owner))
      return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kClosed, "STA close reservation changed");
    if (context != nullptr && (owner.kind != StaOwnerKind::kSession || context->token() != owner.token)) {
      return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kClosed, "STA session context token differs from the cleanup owner");
    }
  }

  const bool device_cleanup_succeeded = context == nullptr || context->release(released_out);
  if (context != nullptr)
    context->_library_models.reset();
  StaOperationResult result = invokeLifecycleOperation("STA persistent close", [this] { return _lifecycle_operations->close(); });
  const bool clean = device_cleanup_succeeded && result.succeeded;
  _is_initialized = false;
  _is_timing_updated = false;
  {
    std::scoped_lock lifecycle_lock(_lifecycle_mutex);
    _lifecycle.completeClose(clean);
  }
  reservation->commit();
  if (clean)
    return StaStatus::success(StaSessionState::kClosed);
  if (!device_cleanup_succeeded)
    result.message = "CUDA STA cleanup failed" + (result.message.empty() ? std::string{} : ": " + result.message);
  return StaStatus::failure(StaSessionStatusCode::kCleanupFailed, StaSessionState::kClosed, result.message);
}

StaStatus STAInterface::copySessionCsrSnapshotObservation(uint64_t token, StaCsrSessionContext* context,
                                                          std::unique_ptr<csr::CsrSnapshotObservation>* detached_out)
{
  if (operationSentinelActive())
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kBaselineExecuted, "recursive STA observation read is not allowed");
  if (detached_out == nullptr || *detached_out)
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kBaselineExecuted, "STA observation read requires a pre-cleared output");
  StaStatus failure;
  auto authority = acquireUse(StaOwner::session(token), context, true, StaSessionState::kBaselineExecuted, &failure);
  if (!authority)
    return failure;
  try {
    if constexpr (incregpusta_debug_checks_enabled) {
      if (context->_baseline_ready && context->substate() == StaCsrSessionSubstate::kAwaitingCapture) {
        const auto captured = context->capture(STADM.getDatabase());
        const auto* result = std::get_if<csr::CsrCaptureResult>(&captured.value);
        if (result == nullptr || result->error)
          return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kBaselineExecuted,
                                    "explicit CSR observation capture failed");
      }
    }
    if (!context->copyObservation(detached_out))
      return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kBaselineExecuted,
                                "CSR snapshot observation is unavailable in the current state");
  } catch (...) {
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kBaselineExecuted,
                              "CSR snapshot observation copy threw: " + currentExceptionMessage());
  }
  return StaStatus::success(StaSessionState::kBaselineExecuted);
}

StaCsrReferenceAttempt STAInterface::computeSessionCsrReferenceForTesting(uint64_t token, StaCsrSessionContext* context)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::session(token), context, true, StaSessionState::kBaselineExecuted, &failure);
  if (!authority)
    return invalidReferenceAttempt();
  if constexpr (!incregpusta_debug_checks_enabled)
    return invalidReferenceAttempt();
  if (context->_baseline_ready && context->substate() == StaCsrSessionSubstate::kAwaitingCapture) {
    const auto captured = context->capture(STADM.getDatabase());
    const auto* result = std::get_if<csr::CsrCaptureResult>(&captured.value);
    if (result == nullptr || result->error)
      return invalidReferenceAttempt();
  }
  return context->computeReference();
}

StaCsrScatterAttempt STAInterface::scatterSessionCsrTimingResultForTesting(uint64_t token, StaCsrSessionContext* context, const csr::CsrTimingResult& result)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::session(token), context, true, StaSessionState::kBaselineExecuted, &failure);
  if (!authority)
    return invalidScatterAttempt();
  return context->scatter(STADM.getDatabase(), result);
}

void STAInterface::inspectSessionDatabaseForTesting(uint64_t token, const std::function<void(const Database&)>& callback)
{
  if (!callback)
    throw std::invalid_argument("STA session Database inspection requires a callback");
  StaStatus failure;
  auto authority = acquireUse(StaOwner::session(token), nullptr, false, StaSessionState::kBaselineExecuted, &failure);
  if (!authority)
    throw std::runtime_error(failure.message);
  callback(STADM.getDatabase());
}

std::string STAInterface::lifecycleStateForTesting() const
{
  std::scoped_lock lifecycle_lock(_lifecycle_mutex);
  return _lifecycle.describe();
}

void STAInterface::setLifecycleOperationsForTesting(StaProductionLifecycleOperations* operations)
{
  if (operationSentinelActive())
    throw std::logic_error("cannot replace STA lifecycle operations recursively");
  if (operations == nullptr)
    throw std::invalid_argument("STA lifecycle test operations must not be null");
  std::unique_lock operation_lock(_operation_mutex);
  OperationGuard authority(this, std::move(operation_lock));
  std::scoped_lock lifecycle_lock(_lifecycle_mutex);
  if (!_lifecycle.isNone() || _close_pending.kind != StaOwnerKind::kNone)
    throw std::logic_error("cannot replace STA lifecycle operations while lifecycle is active");
  _lifecycle_operations = operations;
}

bool STAInterface::isSTAInitialized(std::string& error_message) const
{
  if (_is_initialized)
    return true;
  error_message = "init_sta must be called before STA commands";
  return false;
}

}  // namespace ista
