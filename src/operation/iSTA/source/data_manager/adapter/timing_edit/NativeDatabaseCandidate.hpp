// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of the License at: http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <memory>

#include "TimingEditTypes.hpp"

namespace ista {
class Database;
}

namespace ista::timing_edit {

enum class NativeRollbackFailure : std::uint8_t;
enum class NativeRollbackFailpoint : std::uint8_t;

class InstalledNativeDatabase;
class NativeCandidateExecutor;
class NativeExecutionAuthority;
class NativeExecutionScopeState;
class TimingEditCoordinator;

// Owns a detached raw-input clone. Derived timing state is always empty while
// the candidate is detached, so no copied Arc pointer may be observed.
class NativeDatabaseCandidate final
{
 public:
  NativeDatabaseCandidate(const NativeDatabaseCandidate&) = delete;
  NativeDatabaseCandidate& operator=(const NativeDatabaseCandidate&) = delete;
  NativeDatabaseCandidate(NativeDatabaseCandidate&&) noexcept;
  NativeDatabaseCandidate& operator=(NativeDatabaseCandidate&&) noexcept;
  ~NativeDatabaseCandidate();

  [[nodiscard]] static NativeDatabaseCandidate create(const Database& source);
  [[nodiscard]] Database& database();
  [[nodiscard]] const Database& database() const;
  [[nodiscard]] bool empty() const noexcept { return _database == nullptr; }

  static void resetDerivedState(Database& database) noexcept;

 private:
  explicit NativeDatabaseCandidate(std::unique_ptr<Database> database) noexcept;
  [[nodiscard]] InstalledNativeDatabase installForExecution(
      TimingEditSessionToken session_token,
      std::shared_ptr<NativeExecutionScopeState> execution_scope);
  [[nodiscard]] static std::unique_ptr<Database> exchangeActiveDatabase(std::unique_ptr<Database> replacement) noexcept;
  [[nodiscard]] static Database& activeDatabaseForVerification();

  std::unique_ptr<Database> _database;

  friend class InstalledNativeDatabase;
  friend class NativeCandidateExecutor;
  friend class TimingEditCoordinator;
};

// A successful native execution leaves the candidate installed but keeps the
// previous canonical Database as a rollback owner. Publication is one explicit
// no-throw dismissal. Transactional callers must invoke rollback() explicitly;
// the destructor is a diagnostic fallback, not the primary control path.
class InstalledNativeDatabase final
{
 public:
  InstalledNativeDatabase(const InstalledNativeDatabase&) = delete;
  InstalledNativeDatabase& operator=(const InstalledNativeDatabase&) = delete;
  InstalledNativeDatabase(InstalledNativeDatabase&& other) noexcept;
  InstalledNativeDatabase& operator=(InstalledNativeDatabase&&) = delete;
  ~InstalledNativeDatabase();

  [[nodiscard]] Database& database();
  [[nodiscard]] const Database& database() const;
  [[nodiscard]] bool active() const noexcept;

  [[nodiscard]] bool rollback() noexcept;
  [[nodiscard]] NativeRollbackFailure rollbackStatus() noexcept;

 private:
  InstalledNativeDatabase(std::unique_ptr<Database> rollback_database,
                          TimingEditSessionToken session_token,
                          std::shared_ptr<NativeExecutionScopeState> execution_scope) noexcept;
  [[nodiscard]] NativeRollbackFailure rollbackWithDiagnostics(
      NativeRollbackFailpoint failpoint) noexcept;
  [[nodiscard]] TimingEditSessionToken executionSessionToken() const noexcept { return _execution_session_token; }
  [[nodiscard]] bool executionScopeActive() const noexcept;
  [[nodiscard]] bool executionScopeMatches(
      const NativeExecutionAuthority& authority) const noexcept;
  void commit() noexcept;

  std::unique_ptr<Database> _rollback_database;
  std::shared_ptr<NativeExecutionScopeState> _execution_scope;
  TimingEditSessionToken _execution_session_token;
  bool _scope_attached{false};
  bool _installed{false};

  friend class NativeDatabaseCandidate;
  friend class NativeCandidateExecutor;
  friend class TimingEditCoordinator;
};

}  // namespace ista::timing_edit
