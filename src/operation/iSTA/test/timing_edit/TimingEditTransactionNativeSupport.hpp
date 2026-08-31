// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <memory>

#include "CsrSessionSnapshotAdapter.hpp"
#include "IncrementalStaSession.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeDesignEditTranslator.hpp"
#include "TimingEditCoordinator.hpp"
#include "TimingEditNativeFixture.hpp"

namespace ista::timing_edit::test {

class NativeTransactionRuntime final
{
 public:
  NativeTransactionRuntime();
  ~NativeTransactionRuntime();

  NativeTransactionRuntime(const NativeTransactionRuntime&) = delete;
  NativeTransactionRuntime& operator=(const NativeTransactionRuntime&) = delete;
};

class GuardedTestSession final
{
 public:
  GuardedTestSession();
  ~GuardedTestSession();

  GuardedTestSession(const GuardedTestSession&) = delete;
  GuardedTestSession& operator=(const GuardedTestSession&) = delete;
  GuardedTestSession(GuardedTestSession&&) = delete;
  GuardedTestSession& operator=(GuardedTestSession&&) = delete;

  [[nodiscard]] IncrementalStaSession& session() noexcept { return *_session; }
  [[nodiscard]] const IncrementalStaSession& session() const noexcept { return *_session; }
  [[nodiscard]] TimingEditSessionToken token() const noexcept { return _token; }
  [[nodiscard]] StaStatus closeAndRelease();

 private:
  std::unique_ptr<IncrementalStaSession> _session;
  TimingEditSessionToken _token;
  bool _lease_active{false};
};

struct PreparedNativeTransaction
{
  BaselineSemanticIdentity baseline;
  CommittedEditIdentity initial_identity;
  TimingEditResolverState resolver;
  DesignEditPlan plan;
  PreparedBatch prepared;
};

struct ExecutedNativeTransaction
{
  InstalledNativeDatabase installed;
  std::unique_ptr<csr::CsrCommittedSessionSnapshot> host_snapshot;
  CsrFullExactReceipt csr_full_receipt;
  CommittedEditIdentity post_identity;
  TimingEditResolverState staged_resolver;
  TokenMaterialization materialization;
};

void installNativeTransactionBaseline();

[[nodiscard]] PreparedNativeTransaction prepareNativeTransaction(
    TimingEditSessionToken session_token);

[[nodiscard]] std::unique_ptr<csr::CsrCommittedSessionSnapshot> buildNativeHostSnapshot(
    const Database& database, TimingEditSessionToken session_token);

[[nodiscard]] NativeDatabaseCandidate translateNativeCandidate(
    const PreparedNativeTransaction& transaction,
    const TimingEditResolverState& staged_resolver);

[[nodiscard]] ExecutedNativeTransaction executeNativeTransaction(
    const NativeExecutionAuthority& authority,
    StaCsrSessionContext& session_context,
    const PreparedNativeTransaction& transaction);

void requireReleasedSnapshotForNativeMutation(
    StaCsrSessionContext& session_context, TimingEditSessionToken session_token);

[[nodiscard]] CumulativeEditTrace nativeSuccessorTrace(
    const PreparedNativeTransaction& transaction);

void requireRecursiveStaOperationRejected(std::uint64_t token,
                                          StaCsrSessionContext& context);

}  // namespace ista::timing_edit::test
