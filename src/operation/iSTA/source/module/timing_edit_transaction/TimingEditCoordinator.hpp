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
#include <memory>
#include <vector>

#include "CsrFullExactReceipt.hpp"
#include "NativeDatabaseCandidate.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditTransactionTypes.hpp"
#include "TimingEditTypes.hpp"

namespace ista::csr {
struct CsrCommittedSessionSnapshot;
}

namespace ista::timing_edit {

class NativeExecutionAuthority;

enum class DeviceCsrMaterializationState : std::uint8_t
{
  kNotMaterialized
};

enum class DeviceCsrUnavailableReason : std::uint8_t
{
  kStageNotImplemented
};

struct DeviceCsrState
{
  TransactionGeneration generation;
  DeviceCsrMaterializationState state{DeviceCsrMaterializationState::kNotMaterialized};
  DeviceCsrUnavailableReason reason{DeviceCsrUnavailableReason::kStageNotImplemented};

  friend bool operator==(const DeviceCsrState&, const DeviceCsrState&) = default;
};

enum class CommitMarkerState : std::uint8_t
{
  kComplete
};

struct CommitMarker
{
  TransactionGeneration generation;
  CommitMarkerState state{CommitMarkerState::kComplete};

  friend bool operator==(const CommitMarker&, const CommitMarker&) = default;
};

// The durable transaction trace stores semantic plans only. It never contains
// a session token, native rank, dense ID, pointer, or resolved slot.
struct CumulativeEditTrace
{
  std::vector<DesignEditPlan> batches;

  friend bool operator==(const CumulativeEditTrace&, const CumulativeEditTrace&) = default;
};

using PrecommitRollbackWriter = bool (*)(void*) noexcept;

struct PrecommitRollbackSignal
{
  void* context{nullptr};
  PrecommitRollbackWriter rollback{nullptr};

  [[nodiscard]] constexpr bool valid() const noexcept { return context != nullptr && rollback != nullptr; }
  [[nodiscard]] bool invoke() const noexcept { return rollback(context); }
};

class VerifiedPublishBundle final
{
 public:
  VerifiedPublishBundle(InstalledNativeDatabase&& installed_database,
                        std::unique_ptr<csr::CsrCommittedSessionSnapshot> host_snapshot,
                        CsrFullExactReceipt&& csr_full_receipt, PreparedBatch&& prepared_batch,
                        CumulativeEditTrace&& cumulative_trace, TimingEditResolverState&& resolver_state,
                        CommitFaultLatch commit_fault_latch = {}, bool publish_after_commit_fault = false) noexcept;
  ~VerifiedPublishBundle() noexcept;

  VerifiedPublishBundle(const VerifiedPublishBundle&) = delete;
  VerifiedPublishBundle& operator=(const VerifiedPublishBundle&) = delete;
  VerifiedPublishBundle(VerifiedPublishBundle&&) noexcept;
  VerifiedPublishBundle& operator=(VerifiedPublishBundle&&) = delete;

  [[nodiscard]] const PreparedBatch& preparedBatch() const noexcept { return _prepared_batch; }
  [[nodiscard]] const CumulativeEditTrace& cumulativeTrace() const noexcept { return _cumulative_trace; }
  [[nodiscard]] const DeviceCsrState& deviceState() const noexcept { return _device_state; }
  [[nodiscard]] const CommitMarker& commitMarker() const noexcept { return _commit_marker; }
  [[nodiscard]] bool nativeDatabaseInstalled() const noexcept;
  [[nodiscard]] bool hostSnapshotReady() const noexcept { return _host_snapshot != nullptr; }


 private:
  [[nodiscard]] bool rollbackDatabase() noexcept;

  InstalledNativeDatabase _installed_database;
  std::unique_ptr<csr::CsrCommittedSessionSnapshot> _host_snapshot;
  CsrFullExactReceipt _csr_full_receipt;
  PreparedBatch _prepared_batch;
  CumulativeEditTrace _cumulative_trace;
  TimingEditResolverState _resolver_state;
  DeviceCsrState _device_state;
  CommitMarker _commit_marker;
  CommitFaultLatch _commit_fault_latch;
  bool _publish_after_commit_fault{false};

  friend class TimingEditCoordinator;
};

class FreshRecoveryBundle final
{
 public:
  FreshRecoveryBundle(TimingEditSessionToken new_session_token, std::unique_ptr<csr::CsrCommittedSessionSnapshot> host_snapshot,
                      CsrFullExactReceipt&& csr_full_receipt,
                      CommittedEditIdentity&& committed_identity, CumulativeEditTrace&& cumulative_trace,
                      TimingEditResolverState&& resolver_state, SessionStateSignal new_session_signal) noexcept;
  ~FreshRecoveryBundle() noexcept;

  FreshRecoveryBundle(const FreshRecoveryBundle&) = delete;
  FreshRecoveryBundle& operator=(const FreshRecoveryBundle&) = delete;
  FreshRecoveryBundle(FreshRecoveryBundle&&) noexcept;
  FreshRecoveryBundle& operator=(FreshRecoveryBundle&&) noexcept;

 private:
  TimingEditSessionToken _new_session_token;
  std::unique_ptr<csr::CsrCommittedSessionSnapshot> _host_snapshot;
  CsrFullExactReceipt _csr_full_receipt;
  CommittedEditIdentity _committed_identity;
  CumulativeEditTrace _cumulative_trace;
  TimingEditResolverState _resolver_state;
  DeviceCsrState _device_state;
  SessionStateSignal _new_session_signal;

  friend class TimingEditCoordinator;
};

class TimingEditCoordinator final
{
 public:
  TimingEditCoordinator(TimingEditSessionToken session_token, SessionStateSignal session_signal, BaselineSemanticIdentity baseline,
                        CommittedEditIdentity initial_identity, CumulativeEditTrace initial_trace,
                        TimingEditResolverState initial_resolver_state,
                        std::unique_ptr<csr::CsrCommittedSessionSnapshot> initial_host_snapshot = nullptr);
  ~TimingEditCoordinator() noexcept;

  TimingEditCoordinator(const TimingEditCoordinator&) = delete;
  TimingEditCoordinator& operator=(const TimingEditCoordinator&) = delete;
  TimingEditCoordinator(TimingEditCoordinator&&) = delete;
  TimingEditCoordinator& operator=(TimingEditCoordinator&&) = delete;

  [[nodiscard]] TimingEditCoordinatorState state() const noexcept { return _state; }
  [[nodiscard]] TimingEditSessionToken sessionToken() const noexcept { return _session_token; }
  [[nodiscard]] const BaselineSemanticIdentity& baseline() const noexcept { return _baseline; }
  [[nodiscard]] const CommittedEditIdentity& committedIdentity() const noexcept { return _committed_identity; }
  [[nodiscard]] const CumulativeEditTrace& cumulativeTrace() const noexcept { return _cumulative_trace; }
  [[nodiscard]] const DeviceCsrState& deviceState() const noexcept { return _device_state; }
  [[nodiscard]] const CommitMarker& commitMarker() const noexcept { return _commit_marker; }
  [[nodiscard]] const csr::CsrCommittedSessionSnapshot* hostSnapshot() const noexcept { return _host_snapshot.get(); }
  [[nodiscard]] const TimingEditResolverState& resolverState() const noexcept { return _resolver_state; }

  [[nodiscard]] TransactionStatus beginPrepare(const PreparedBatch& prepared_batch);
  [[nodiscard]] TransactionStatus markStaged() noexcept;
  [[nodiscard]] TransactionStatus markExecuting() noexcept;
  [[nodiscard]] TransactionStatus sealVerified(PreparedBatch& prepared_batch, CommittedEditIdentity target_post_identity);

  // The caller owns candidate rollback before a VerifiedPublishBundle exists.
  [[nodiscard]] TransactionStatus rollbackPrecommit(PrecommitRollbackSignal rollback_signal) noexcept;
  [[nodiscard]] TransactionStatus cancelPrepared() noexcept;

  [[nodiscard]] TransactionStatus publish(const NativeExecutionAuthority& authority, VerifiedPublishBundle&& bundle,
                                          PrecommitFailpoint failpoint = PrecommitFailpoint::kNone);

  // The adapter calls these only after the session's standard close cleanup.
  // The transaction layer never writes the closed state itself.
  [[nodiscard]] TransactionStatus acknowledgePoisonedSessionClosed(TimingEditSessionToken closed_session_token) noexcept;
  [[nodiscard]] TransactionStatus installFreshRecovery(const NativeExecutionAuthority& authority,
                                                       FreshRecoveryBundle&& bundle);
  [[nodiscard]] TransactionStatus acknowledgeSessionClosed(TimingEditSessionToken closed_session_token) noexcept;

 private:
  [[nodiscard]] TransactionStatus status(TransactionStatusCode code) const noexcept { return TransactionStatus{code, _state}; }
  [[nodiscard]] bool matchesCurrentBatch(const PreparedBatch& prepared_batch) const;
  [[nodiscard]] bool isValidCumulativeSuccessor(const CumulativeEditTrace& trace, const PreparedBatch& prepared_batch) const;
  [[nodiscard]] bool isValidFreshRecovery(const FreshRecoveryBundle& bundle) const;
  [[nodiscard]] TransactionStatus rejectBeforeCommit(VerifiedPublishBundle& bundle, TransactionStatusCode code) noexcept;
  [[nodiscard]] TransactionStatus completePublishNoexcept(VerifiedPublishBundle& bundle) noexcept;
  [[nodiscard]] TransactionStatus completeRecoveryNoexcept(FreshRecoveryBundle& bundle) noexcept;

  TimingEditCoordinatorState _state{TimingEditCoordinatorState::kIdle};
  TimingEditSessionToken _session_token;
  SessionStateSignal _session_signal;
  BaselineSemanticIdentity _baseline;
  std::unique_ptr<csr::CsrCommittedSessionSnapshot> _host_snapshot;
  CommittedEditIdentity _committed_identity;
  CumulativeEditTrace _cumulative_trace;
  TimingEditResolverState _resolver_state;
  DeviceCsrState _device_state;
  CommitMarker _commit_marker;
};

}  // namespace ista::timing_edit
