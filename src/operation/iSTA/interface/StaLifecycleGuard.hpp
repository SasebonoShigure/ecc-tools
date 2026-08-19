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
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ista {

struct StaOperationResult
{
  bool succeeded = true;
  bool cleanup_failed = false;
  std::string message;

  static StaOperationResult success();
  static StaOperationResult workFailure(std::string message);
  static StaOperationResult cleanupFailure(std::string message);
  void mergeCleanup(const StaOperationResult& cleanup_result);
};

class StaLifecycleGuard
{
 public:
  using Cleanup = std::function<void()>;

  StaLifecycleGuard() = default;
  StaLifecycleGuard(const StaLifecycleGuard&) = delete;
  StaLifecycleGuard(StaLifecycleGuard&&) = delete;
  ~StaLifecycleGuard() noexcept;
  StaLifecycleGuard& operator=(const StaLifecycleGuard&) = delete;
  StaLifecycleGuard& operator=(StaLifecycleGuard&&) = delete;

  void add(std::string name, Cleanup cleanup);
  std::size_t addUnarmed(std::string name, Cleanup cleanup);
  void arm(std::size_t index) noexcept;
  StaOperationResult cleanupLast() noexcept;
  StaOperationResult cleanup() noexcept;
  void dismiss() noexcept;

 private:
  struct Entry
  {
    std::string name;
    Cleanup cleanup;
    bool armed = false;
  };
  std::vector<Entry> _entries;
};

void emitStaLifecycleDiagnostic(std::string_view prefix, std::string_view message) noexcept;

StaOperationResult executeStaLifecycleStage(const std::string& name, const std::function<void()>& initialize, const std::function<void()>& work,
                                            const std::function<void()>& cleanup);

struct StaLifecycleStage
{
  std::string name;
  std::function<void()> initialize;
  std::function<void()> work;
  std::function<void()> cleanup;
};

StaOperationResult executeStaLifecyclePipeline(const std::vector<StaLifecycleStage>& stages, const std::optional<StaLifecycleStage>& envelope = std::nullopt,
                                               bool retain_envelope = false);

StaOperationResult executeStaPersistentInitialization(const std::vector<StaLifecycleStage>& stages);

struct StaCleanupAction
{
  std::string name;
  std::function<void()> cleanup;
};

StaOperationResult executeStaCleanupSequence(const std::vector<StaCleanupAction>& actions);

enum class StaOwnerKind
{
  kNone,
  kLegacy,
  kSession
};

struct StaOwner
{
  StaOwnerKind kind = StaOwnerKind::kNone;
  uint64_t token = 0;

  static StaOwner none();
  static StaOwner legacy();
  static StaOwner session(uint64_t token);
  bool operator==(const StaOwner&) const = default;
};

struct StaLeaseResult
{
  bool succeeded;
  bool quarantined;
  std::string message;
};

class StaOwnerLease
{
 public:
  StaLeaseResult acquireLegacy();
  StaLeaseResult acquireSession(uint64_t token);
  StaLeaseResult requireLegacyUse() const;
  StaLeaseResult requireSessionUse(uint64_t token) const;
  StaLeaseResult requireLegacyClose() const;
  StaLeaseResult requireSessionClose(uint64_t token) const;

  void releaseAfterRollback();
  void quarantineAfterRollback();
  void quarantineCurrent();
  void completeClose(bool cleanup_succeeded);

  [[nodiscard]] bool isNone() const;
  [[nodiscard]] bool isQuarantined() const;
  [[nodiscard]] std::string describe() const;

 private:
  StaLeaseResult acquire(StaOwner owner);
  StaLeaseResult requireUse(StaOwner owner) const;
  StaLeaseResult requireClose(StaOwner owner) const;

  StaOwner _owner;
  bool _quarantined = false;
  StaOwner _cleanup_owner;
};

}  // namespace ista
