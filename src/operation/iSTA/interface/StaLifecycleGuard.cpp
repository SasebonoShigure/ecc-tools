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
#include "StaLifecycleGuard.hpp"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

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

void appendMessage(std::string& destination, const std::string& message)
{
  if (!destination.empty())
    destination += "; ";
  destination += message;
}

std::string ownerName(const StaOwner& owner)
{
  switch (owner.kind) {
    case StaOwnerKind::kNone:
      return "none";
    case StaOwnerKind::kLegacy:
      return "legacy";
    case StaOwnerKind::kSession:
      return "session(" + std::to_string(owner.token) + ")";
  }
  return "unknown";
}

StaLeaseResult allowed()
{
  return {.succeeded = true, .quarantined = false, .message = {}};
}

StaLeaseResult denied(bool quarantined, std::string message)
{
  return {.succeeded = false, .quarantined = quarantined, .message = std::move(message)};
}

}  // namespace

StaOperationResult StaOperationResult::success()
{
  return {};
}

StaOperationResult StaOperationResult::workFailure(std::string message)
{
  return {.succeeded = false, .cleanup_failed = false, .message = std::move(message)};
}

StaOperationResult StaOperationResult::cleanupFailure(std::string message)
{
  return {.succeeded = false, .cleanup_failed = true, .message = std::move(message)};
}

void StaOperationResult::mergeCleanup(const StaOperationResult& cleanup_result)
{
  if (cleanup_result.succeeded)
    return;
  succeeded = false;
  cleanup_failed = cleanup_failed || cleanup_result.cleanup_failed;
  appendMessage(message, cleanup_result.message);
}

void emitStaLifecycleDiagnostic(std::string_view prefix, std::string_view message) noexcept
{
  std::fwrite(prefix.data(), sizeof(char), prefix.size(), stderr);
  std::fwrite(message.data(), sizeof(char), message.size(), stderr);
  std::fputc('\n', stderr);
}

StaLifecycleGuard::~StaLifecycleGuard() noexcept
{
  StaOperationResult result = cleanup();
  if (!result.succeeded)
    emitStaLifecycleDiagnostic("STA lifecycle destructor cleanup failed: ", result.message);
}

void StaLifecycleGuard::add(std::string name, Cleanup cleanup)
{
  _entries.push_back({.name = std::move(name), .cleanup = std::move(cleanup), .armed = true});
}

std::size_t StaLifecycleGuard::addUnarmed(std::string name, Cleanup cleanup)
{
  _entries.push_back({.name = std::move(name), .cleanup = std::move(cleanup), .armed = false});
  return _entries.size() - 1;
}

void StaLifecycleGuard::arm(std::size_t index) noexcept
{
  if (index < _entries.size())
    _entries[index].armed = true;
}

StaOperationResult StaLifecycleGuard::cleanupLast() noexcept
{
  if (_entries.empty())
    return StaOperationResult::success();
  Entry entry = std::move(_entries.back());
  _entries.pop_back();
  if (!entry.armed)
    return StaOperationResult::success();
  try {
    entry.cleanup();
    return StaOperationResult::success();
  } catch (...) {
    return StaOperationResult::cleanupFailure(entry.name + " cleanup failed: " + exceptionMessage());
  }
}

StaOperationResult StaLifecycleGuard::cleanup() noexcept
{
  StaOperationResult result = StaOperationResult::success();
  while (!_entries.empty())
    result.mergeCleanup(cleanupLast());
  return result;
}

void StaLifecycleGuard::dismiss() noexcept
{
  _entries.clear();
}

StaOperationResult executeStaLifecycleStage(const std::string& name, const std::function<void()>& initialize, const std::function<void()>& work,
                                            const std::function<void()>& cleanup)
{
  StaLifecycleGuard guard;
  std::size_t cleanup_index;
  try {
    cleanup_index = guard.addUnarmed(name, cleanup);
  } catch (...) {
    return StaOperationResult::workFailure(name + " cleanup ledger setup failed: " + exceptionMessage());
  }
  try {
    initialize();
    guard.arm(cleanup_index);
    work();
  } catch (...) {
    StaOperationResult result = StaOperationResult::workFailure(name + " failed: " + exceptionMessage());
    result.mergeCleanup(guard.cleanup());
    return result;
  }
  return guard.cleanup();
}

StaOperationResult executeStaLifecyclePipeline(const std::vector<StaLifecycleStage>& stages, const std::optional<StaLifecycleStage>& envelope,
                                               bool retain_envelope)
{
  StaLifecycleGuard envelope_guard;
  if (envelope) {
    std::size_t cleanup_index;
    try {
      cleanup_index = envelope_guard.addUnarmed(envelope->name, envelope->cleanup);
    } catch (...) {
      return StaOperationResult::workFailure(envelope->name + " cleanup ledger setup failed: " + exceptionMessage());
    }
    try {
      envelope->initialize();
      envelope_guard.arm(cleanup_index);
      envelope->work();
    } catch (...) {
      StaOperationResult result = StaOperationResult::workFailure(envelope->name + " failed: " + exceptionMessage());
      result.mergeCleanup(envelope_guard.cleanup());
      return result;
    }
  }
  for (const StaLifecycleStage& stage : stages) {
    StaOperationResult result = executeStaLifecycleStage(stage.name, stage.initialize, stage.work, stage.cleanup);
    if (!result.succeeded) {
      result.mergeCleanup(envelope_guard.cleanup());
      return result;
    }
  }
  if (retain_envelope) {
    envelope_guard.dismiss();
    return StaOperationResult::success();
  }
  return envelope_guard.cleanup();
}

StaOperationResult executeStaPersistentInitialization(const std::vector<StaLifecycleStage>& stages)
{
  StaLifecycleGuard rollback;
  std::vector<std::size_t> cleanup_indices;
  try {
    cleanup_indices.reserve(stages.size());
    for (const StaLifecycleStage& stage : stages)
      cleanup_indices.push_back(rollback.addUnarmed(stage.name, stage.cleanup));
  } catch (...) {
    return StaOperationResult::workFailure("persistent cleanup ledger setup failed: " + exceptionMessage());
  }
  for (std::size_t index = 0; index < stages.size(); ++index) {
    const StaLifecycleStage& stage = stages[index];
    try {
      stage.initialize();
      rollback.arm(cleanup_indices[index]);
      stage.work();
    } catch (...) {
      StaOperationResult result = StaOperationResult::workFailure(stage.name + " failed: " + exceptionMessage());
      result.mergeCleanup(rollback.cleanup());
      return result;
    }
  }
  rollback.dismiss();
  return StaOperationResult::success();
}

StaOperationResult executeStaCleanupSequence(const std::vector<StaCleanupAction>& actions)
{
  StaOperationResult result = StaOperationResult::success();
  for (const StaCleanupAction& action : actions) {
    try {
      action.cleanup();
    } catch (...) {
      result.mergeCleanup(StaOperationResult::cleanupFailure(action.name + " cleanup failed: " + exceptionMessage()));
    }
  }
  return result;
}

StaOwner StaOwner::none()
{
  return {};
}

StaOwner StaOwner::legacy()
{
  return {.kind = StaOwnerKind::kLegacy, .token = 0};
}

StaOwner StaOwner::session(uint64_t token)
{
  return {.kind = StaOwnerKind::kSession, .token = token};
}

StaLeaseResult StaOwnerLease::acquireLegacy()
{
  return acquire(StaOwner::legacy());
}

StaLeaseResult StaOwnerLease::acquireSession(uint64_t token)
{
  return acquire(StaOwner::session(token));
}

StaLeaseResult StaOwnerLease::requireLegacyUse() const
{
  return requireUse(StaOwner::legacy());
}

StaLeaseResult StaOwnerLease::requireSessionUse(uint64_t token) const
{
  return requireUse(StaOwner::session(token));
}

StaLeaseResult StaOwnerLease::requireLegacyClose() const
{
  return requireClose(StaOwner::legacy());
}

StaLeaseResult StaOwnerLease::requireSessionClose(uint64_t token) const
{
  return requireClose(StaOwner::session(token));
}

StaLeaseResult StaOwnerLease::acquire(StaOwner owner)
{
  if (_quarantined)
    return denied(true, "STA process is quarantined and requires a fresh process");
  if (_owner.kind != StaOwnerKind::kNone)
    return denied(false, "STA lifecycle is owned by " + ownerName(_owner));
  _owner = owner;
  return allowed();
}

StaLeaseResult StaOwnerLease::requireUse(StaOwner owner) const
{
  if (_quarantined)
    return denied(true, "STA process is quarantined; execution is disabled");
  if (_owner == owner)
    return allowed();
  return denied(false, "STA lifecycle owner mismatch: expected " + ownerName(owner) + ", actual " + ownerName(_owner));
}

StaLeaseResult StaOwnerLease::requireClose(StaOwner owner) const
{
  if (!_quarantined && _owner == owner)
    return allowed();
  if (_quarantined && _cleanup_owner == owner)
    return allowed();
  if (_quarantined)
    return denied(true, "STA cleanup ownership has already been consumed or belongs to another owner");
  return denied(false, "STA lifecycle owner mismatch: expected " + ownerName(owner) + ", actual " + ownerName(_owner));
}

void StaOwnerLease::releaseAfterRollback()
{
  if (_quarantined)
    throw std::logic_error("cannot release a quarantined STA lease");
  _owner = StaOwner::none();
}

void StaOwnerLease::quarantineAfterRollback()
{
  _quarantined = true;
  _owner = StaOwner::none();
  _cleanup_owner = StaOwner::none();
}

void StaOwnerLease::quarantineCurrent()
{
  if (_owner.kind == StaOwnerKind::kNone)
    throw std::logic_error("cannot retain cleanup ownership without an STA owner");
  _quarantined = true;
  _cleanup_owner = _owner;
}

void StaOwnerLease::completeClose(bool cleanup_succeeded)
{
  if (_quarantined) {
    _cleanup_owner = StaOwner::none();
    _owner = StaOwner::none();
    return;
  }
  _owner = StaOwner::none();
  if (!cleanup_succeeded) {
    _quarantined = true;
    _cleanup_owner = StaOwner::none();
  }
}

bool StaOwnerLease::isNone() const
{
  return !_quarantined && _owner.kind == StaOwnerKind::kNone;
}

bool StaOwnerLease::isQuarantined() const
{
  return _quarantined;
}

std::string StaOwnerLease::describe() const
{
  if (!_quarantined)
    return ownerName(_owner);
  return "quarantined(" + ownerName(_cleanup_owner) + ")";
}

}  // namespace ista
