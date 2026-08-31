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

#include <cstdint>
#include <type_traits>

namespace ista::timing_edit {

enum class TimingEditCoordinatorState : std::uint8_t
{
  kIdle,
  kPrepared,
  kStaged,
  kExecuting,
  kVerified,
  kPoisoned,
  kRecovering,
  kClosed
};

enum class TransactionStatusCode : std::uint8_t
{
  kOk,
  kInvalidState,
  kInvalidSessionSignal,
  kStaleCommittedIdentity,
  kUnsealedBatch,
  kInvalidGeneration,
  kInvalidPostIdentity,
  kInvalidTrace,
  kCandidateDatabaseMismatch,
  kHostSnapshotMismatch,
  kCsrFullReceiptMismatch,
  kResolverMismatch,
  kPublishVerificationFailed,
  kPrecommitFault,
  kRollbackFailed,
  kPoisonedCommit,
  kRecoveryMismatch,
  kClosed
};

struct TransactionStatus
{
  TransactionStatusCode code{TransactionStatusCode::kOk};
  TimingEditCoordinatorState state{TimingEditCoordinatorState::kIdle};

  [[nodiscard]] constexpr bool ok() const noexcept { return code == TransactionStatusCode::kOk; }
  friend bool operator==(const TransactionStatus&, const TransactionStatus&) = default;
};

enum class SessionTransactionState : std::uint8_t
{
  kBaselineExecuted,
  kPoisoned
};

using SessionStateWriter = void (*)(void*, SessionTransactionState) noexcept;

struct SessionStateSignal
{
  void* context{nullptr};
  SessionStateWriter write{nullptr};

  [[nodiscard]] constexpr bool valid() const noexcept { return context != nullptr && write != nullptr; }
  void set(SessionTransactionState state) const noexcept { write(context, state); }
};

enum class PrecommitFailpoint : std::uint8_t
{
  kNone,
  kPublishBeforeCommit
};

// A hit is remembered but can never interrupt the remaining no-throw
// publication steps. The complete marker is installed after all four checks.
enum class CommitLatchOrdinal : std::uint8_t
{
  kDatabaseCanonical = 1,
  kHostSnapshot = 2,
  kTransactionSidecars = 3,
  kDeviceState = 4
};

class CommitFaultLatch final
{
 public:
  constexpr CommitFaultLatch() noexcept = default;
  explicit constexpr CommitFaultLatch(CommitLatchOrdinal armed_ordinal) noexcept : _armed(true), _armed_ordinal(armed_ordinal) {}

  void check(CommitLatchOrdinal ordinal) noexcept
  {
    if (_armed && ordinal == _armed_ordinal) {
      _latched = true;
    }
  }

  [[nodiscard]] constexpr bool armed() const noexcept { return _armed; }
  [[nodiscard]] constexpr bool latched() const noexcept { return _latched; }
  [[nodiscard]] constexpr CommitLatchOrdinal armedOrdinal() const noexcept { return _armed_ordinal; }

 private:
  bool _armed{false};
  bool _latched{false};
  CommitLatchOrdinal _armed_ordinal{CommitLatchOrdinal::kDatabaseCanonical};
};

static_assert(std::is_trivially_copyable_v<SessionStateSignal>);
static_assert(std::is_nothrow_copy_constructible_v<CommitFaultLatch>);
static_assert(noexcept(CommitFaultLatch{}.check(CommitLatchOrdinal::kDatabaseCanonical)));

}  // namespace ista::timing_edit
