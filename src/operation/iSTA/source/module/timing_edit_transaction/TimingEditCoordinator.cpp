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
#include "TimingEditCoordinator.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "CsrSnapshotObservation.hpp"
#include "CsrSnapshotObservationValidator.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeDatabaseCandidate.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditValidation.hpp"

namespace ista::timing_edit {
namespace {
inline constexpr std::uint64_t kStage3GraphGeneration = 0;

[[nodiscard]] bool sizeMatchesGeneration(std::size_t size, TransactionGeneration generation)
{
  if (size > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
    return false;
  }
  return static_cast<std::uint64_t>(size) == generation.value();
}

[[nodiscard]] bool isWellFormedTrace(const CumulativeEditTrace& trace, const BaselineSemanticIdentity& baseline,
                                     TransactionGeneration generation)
{
  if (!sizeMatchesGeneration(trace.batches.size(), generation)) {
    return false;
  }
  for (std::size_t index = 0; index < trace.batches.size(); ++index) {
    const DesignEditPlan& plan = trace.batches[index];
    if (plan.schema_version != kDesignEditPlanSchema || plan.baseline != baseline
        || plan.expected_generation != TransactionGeneration(static_cast<std::uint64_t>(index))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool isKnownFailpoint(PrecommitFailpoint failpoint) noexcept
{
  return failpoint == PrecommitFailpoint::kNone || failpoint == PrecommitFailpoint::kPublishBeforeCommit;
}

[[nodiscard]] bool isKnownCommitLatchOrdinal(CommitLatchOrdinal ordinal) noexcept
{
  return ordinal == CommitLatchOrdinal::kDatabaseCanonical
         || ordinal == CommitLatchOrdinal::kHostSnapshot
         || ordinal == CommitLatchOrdinal::kTransactionSidecars
         || ordinal == CommitLatchOrdinal::kDeviceState;
}

[[nodiscard]] bool sameMetadata(const csr::CsrCaptureMetadata& lhs, const csr::CsrCaptureMetadata& rhs)
{
  return lhs.level_offsets == rhs.level_offsets && lhs.level_nodes == rhs.level_nodes && lhs.node_to_level == rhs.node_to_level;
}

[[nodiscard]] bool validSnapshotEnvelope(const csr::CsrCommittedSessionSnapshot& snapshot)
{
  if (snapshot.active_observation == nullptr || snapshot.released_observation == nullptr) {
    return false;
  }
  if (snapshot.active_observation->state != csr::CsrSnapshotState::kReady
      || snapshot.released_observation->state != csr::CsrSnapshotState::kReleased) {
    return false;
  }
  if (snapshot.active_observation->identity != snapshot.input.source_identity
      || snapshot.released_observation->identity != snapshot.input.source_identity) {
    return false;
  }
  return csr::validateCaptureMetadata(snapshot.input, snapshot.metadata).empty()
         && csr::validateSnapshotObservation(*snapshot.active_observation).empty()
         && csr::validateSnapshotObservation(*snapshot.released_observation).empty();
}

[[nodiscard]] std::unique_ptr<csr::CsrCommittedSessionSnapshot> rebuildMatchingHostSnapshot(
    const Database& database, const csr::CsrCommittedSessionSnapshot& supplied, TimingEditSessionToken session_token)
{
  if (!validSnapshotEnvelope(supplied)) {
    return nullptr;
  }

  csr::CsrSessionSnapshotBuildResult rebuilt_result
      = csr::buildCsrSessionSnapshot(database, supplied.input.analysis_context, session_token.value());
  auto* rebuilt = std::get_if<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>(&rebuilt_result);
  if (rebuilt == nullptr || *rebuilt == nullptr || !validSnapshotEnvelope(**rebuilt)) {
    return nullptr;
  }

  const auto& supplied_identity = supplied.input.source_identity;
  const auto& rebuilt_identity = (*rebuilt)->input.source_identity;
  if (supplied_identity.session_token != session_token.value()
      || rebuilt_identity.session_token != session_token.value()
      || supplied_identity.graph_generation != kStage3GraphGeneration
      || rebuilt_identity.graph_generation != kStage3GraphGeneration) {
    return nullptr;
  }

  // The logical reference input and its immutable envelope are compared as
  // canonical byte sequences. This is a functional equality gate, not a
  // hash-only proxy. FIFO metadata is compared field by field.
  if (csr::canonicalReferenceInputBytes(supplied.input) != csr::canonicalReferenceInputBytes((*rebuilt)->input)
      || csr::canonicalSnapshotIdentityBytes(supplied_identity) != csr::canonicalSnapshotIdentityBytes(rebuilt_identity)
      || !sameMetadata(supplied.metadata, (*rebuilt)->metadata)) {
    return nullptr;
  }
  return std::move(*rebuilt);
}

[[nodiscard]] bool candidateMatchesPostIdentity(Database& database, const BaselineSemanticIdentity& baseline,
                                                const CommittedEditIdentity& target,
                                                const csr::StaAnalysisContext& analysis_context,
                                                TimingEditSessionToken session_token)
{
  if (!NativeTimingEditPreparer::matchesDesignName(database, baseline.design_name) || !validateCommittedEditIdentity(target).empty()) {
    return false;
  }
  return NativeTimingEditPreparer::captureCurrentIdentity(database, target.generation, analysis_context, session_token) == target;
}

}  // namespace

VerifiedPublishBundle::VerifiedPublishBundle(InstalledNativeDatabase&& installed_database,
                                             std::unique_ptr<csr::CsrCommittedSessionSnapshot> host_snapshot,
                                             CsrFullExactReceipt&& csr_full_receipt,
                                             PreparedBatch&& prepared_batch, CumulativeEditTrace&& cumulative_trace,
                                             TimingEditResolverState&& resolver_state, CommitFaultLatch commit_fault_latch,
                                             bool publish_after_commit_fault) noexcept
    : _installed_database(std::move(installed_database)),
      _host_snapshot(std::move(host_snapshot)),
      _csr_full_receipt(std::move(csr_full_receipt)),
      _prepared_batch(std::move(prepared_batch)),
      _cumulative_trace(std::move(cumulative_trace)),
      _resolver_state(std::move(resolver_state)),
      _device_state{_prepared_batch.targetGeneration()},
      _commit_marker{_prepared_batch.targetGeneration()},
      _commit_fault_latch(commit_fault_latch),
      _publish_after_commit_fault(publish_after_commit_fault)
{
}

VerifiedPublishBundle::~VerifiedPublishBundle() noexcept = default;
VerifiedPublishBundle::VerifiedPublishBundle(VerifiedPublishBundle&&) noexcept = default;

static_assert(std::is_nothrow_move_constructible_v<InstalledNativeDatabase>);
static_assert(std::is_nothrow_move_constructible_v<CsrFullExactReceipt>);
static_assert(std::is_nothrow_move_constructible_v<PreparedBatch>);
static_assert(std::is_nothrow_move_constructible_v<CumulativeEditTrace>);
static_assert(std::is_nothrow_move_constructible_v<TimingEditResolverState>);
static_assert(std::is_nothrow_constructible_v<VerifiedPublishBundle, InstalledNativeDatabase&&,
                                              std::unique_ptr<csr::CsrCommittedSessionSnapshot>,
                                              CsrFullExactReceipt&&, PreparedBatch&&,
                                              CumulativeEditTrace&&, TimingEditResolverState&&, CommitFaultLatch, bool>);

bool VerifiedPublishBundle::nativeDatabaseInstalled() const noexcept
{
  return _installed_database.active();
}

bool VerifiedPublishBundle::rollbackDatabase() noexcept
{
  if (!nativeDatabaseInstalled()) {
    return false;
  }
  return _installed_database.rollback();
}

FreshRecoveryBundle::FreshRecoveryBundle(TimingEditSessionToken new_session_token,
                                         std::unique_ptr<csr::CsrCommittedSessionSnapshot> host_snapshot,
                                         CsrFullExactReceipt&& csr_full_receipt,
                                         CommittedEditIdentity&& committed_identity, CumulativeEditTrace&& cumulative_trace,
                                         TimingEditResolverState&& resolver_state, SessionStateSignal new_session_signal) noexcept
    : _new_session_token(new_session_token),
      _host_snapshot(std::move(host_snapshot)),
      _csr_full_receipt(std::move(csr_full_receipt)),
      _committed_identity(std::move(committed_identity)),
      _cumulative_trace(std::move(cumulative_trace)),
      _resolver_state(std::move(resolver_state)),
      _device_state{_committed_identity.generation},
      _new_session_signal(new_session_signal)
{
}

FreshRecoveryBundle::~FreshRecoveryBundle() noexcept = default;
FreshRecoveryBundle::FreshRecoveryBundle(FreshRecoveryBundle&&) noexcept = default;
FreshRecoveryBundle& FreshRecoveryBundle::operator=(FreshRecoveryBundle&&) noexcept = default;

TimingEditCoordinator::TimingEditCoordinator(TimingEditSessionToken session_token, SessionStateSignal session_signal,
                                             BaselineSemanticIdentity baseline, CommittedEditIdentity initial_identity,
                                             CumulativeEditTrace initial_trace, TimingEditResolverState initial_resolver_state,
                                             std::unique_ptr<csr::CsrCommittedSessionSnapshot> initial_host_snapshot)
    : _session_token(session_token),
      _session_signal(session_signal),
      _baseline(std::move(baseline)),
      _host_snapshot(std::move(initial_host_snapshot)),
      _committed_identity(std::move(initial_identity)),
      _cumulative_trace(std::move(initial_trace)),
      _resolver_state(std::move(initial_resolver_state)),
      _device_state{_committed_identity.generation},
      _commit_marker{_committed_identity.generation}
{
  if (_session_token.value() == 0) {
    throw std::invalid_argument("timing edit coordinator requires a nonzero session token");
  }
  if (!_session_signal.valid()) {
    throw std::invalid_argument("timing edit coordinator requires a bound session signal");
  }
  if (!validateBaselineSemanticIdentity(_baseline).empty()) {
    throw std::invalid_argument("timing edit coordinator requires a valid baseline identity");
  }
  if (!validateCommittedEditIdentity(_committed_identity).empty()) {
    throw std::invalid_argument("timing edit coordinator requires a valid committed identity");
  }
  if (_committed_identity.generation == TransactionGeneration(0)
      && (_committed_identity.current_fingerprints != _baseline.fingerprints
          || _committed_identity.canonical_pre_state != _baseline.canonical_pre_state)) {
    throw std::invalid_argument("generation zero committed identity must equal the baseline identity");
  }
  if (!isWellFormedTrace(_cumulative_trace, _baseline, _committed_identity.generation)) {
    throw std::invalid_argument("initial cumulative trace does not match the committed generation");
  }
}

TimingEditCoordinator::~TimingEditCoordinator() noexcept = default;

bool TimingEditCoordinator::matchesCurrentBatch(const PreparedBatch& prepared_batch) const
{
  const std::uint64_t current_generation = _committed_identity.generation.value();
  if (current_generation == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  return prepared_batch.sessionToken() == _session_token && prepared_batch.baseline() == _baseline
         && prepared_batch.expectedGeneration() == _committed_identity.generation
         && prepared_batch.targetGeneration() == TransactionGeneration(current_generation + 1)
         && prepared_batch.expectedPreCommittedIdentity() == _committed_identity
         && prepared_batch.policyReceipt().disposition == EditDisposition::kSupported;
}

TransactionStatus TimingEditCoordinator::beginPrepare(const PreparedBatch& prepared_batch)
{
  if (_state == TimingEditCoordinatorState::kClosed) {
    return status(TransactionStatusCode::kClosed);
  }
  if (_state != TimingEditCoordinatorState::kIdle) {
    return status(TransactionStatusCode::kInvalidState);
  }
  if (!matchesCurrentBatch(prepared_batch)) {
    return status(TransactionStatusCode::kStaleCommittedIdentity);
  }
  if (prepared_batch.sealed()) {
    return status(TransactionStatusCode::kUnsealedBatch);
  }
  _state = TimingEditCoordinatorState::kPrepared;
  return status(TransactionStatusCode::kOk);
}

TransactionStatus TimingEditCoordinator::markStaged() noexcept
{
  if (_state != TimingEditCoordinatorState::kPrepared) {
    return status(TransactionStatusCode::kInvalidState);
  }
  _state = TimingEditCoordinatorState::kStaged;
  return status(TransactionStatusCode::kOk);
}

TransactionStatus TimingEditCoordinator::markExecuting() noexcept
{
  if (_state != TimingEditCoordinatorState::kStaged) {
    return status(TransactionStatusCode::kInvalidState);
  }
  _state = TimingEditCoordinatorState::kExecuting;
  return status(TransactionStatusCode::kOk);
}

TransactionStatus TimingEditCoordinator::sealVerified(PreparedBatch& prepared_batch, CommittedEditIdentity target_post_identity)
{
  if (_state != TimingEditCoordinatorState::kExecuting) {
    return status(TransactionStatusCode::kInvalidState);
  }
  if (!matchesCurrentBatch(prepared_batch)) {
    return status(TransactionStatusCode::kStaleCommittedIdentity);
  }
  if (prepared_batch.sealed()) {
    return status(TransactionStatusCode::kUnsealedBatch);
  }
  if (target_post_identity.generation != prepared_batch.targetGeneration()) {
    return status(TransactionStatusCode::kInvalidGeneration);
  }
  try {
    prepared_batch.seal(std::move(target_post_identity));
  } catch (const std::exception&) {
    return status(TransactionStatusCode::kInvalidPostIdentity);
  }
  _state = TimingEditCoordinatorState::kVerified;
  return status(TransactionStatusCode::kOk);
}

TransactionStatus TimingEditCoordinator::rollbackPrecommit(PrecommitRollbackSignal rollback_signal) noexcept
{
  if (_state != TimingEditCoordinatorState::kStaged && _state != TimingEditCoordinatorState::kExecuting
      && _state != TimingEditCoordinatorState::kVerified) {
    return status(TransactionStatusCode::kInvalidState);
  }
  if (!rollback_signal.valid()) {
    return status(TransactionStatusCode::kInvalidSessionSignal);
  }
  if (!rollback_signal.invoke()) {
    _state = TimingEditCoordinatorState::kPoisoned;
    _session_signal.set(SessionTransactionState::kPoisoned);
    return status(TransactionStatusCode::kRollbackFailed);
  }
  _state = TimingEditCoordinatorState::kIdle;
  return status(TransactionStatusCode::kOk);
}

TransactionStatus TimingEditCoordinator::cancelPrepared() noexcept
{
  if (_state != TimingEditCoordinatorState::kPrepared) {
    return status(TransactionStatusCode::kInvalidState);
  }
  _state = TimingEditCoordinatorState::kIdle;
  return status(TransactionStatusCode::kOk);
}

bool TimingEditCoordinator::isValidCumulativeSuccessor(const CumulativeEditTrace& trace, const PreparedBatch& prepared_batch) const
{
  if (trace.batches.size() != _cumulative_trace.batches.size() + 1 || !sizeMatchesGeneration(trace.batches.size(), prepared_batch.targetGeneration())) {
    return false;
  }
  if (!std::equal(_cumulative_trace.batches.begin(), _cumulative_trace.batches.end(), trace.batches.begin())) {
    return false;
  }
  const DesignEditPlan& current_batch = trace.batches.back();
  return current_batch.schema_version == kDesignEditPlanSchema && current_batch.baseline == _baseline
         && current_batch.expected_generation == prepared_batch.expectedGeneration()
         && current_batch.edits == prepared_batch.normalizedEdits();
}

TransactionStatus TimingEditCoordinator::rejectBeforeCommit(VerifiedPublishBundle& bundle, TransactionStatusCode code) noexcept
{
  if (!bundle.rollbackDatabase()) {
    _state = TimingEditCoordinatorState::kPoisoned;
    _session_signal.set(SessionTransactionState::kPoisoned);
    return status(TransactionStatusCode::kRollbackFailed);
  }
  if (_state != TimingEditCoordinatorState::kPoisoned && _state != TimingEditCoordinatorState::kRecovering
      && _state != TimingEditCoordinatorState::kClosed) {
    _state = TimingEditCoordinatorState::kIdle;
  }
  return status(code);
}

TransactionStatus TimingEditCoordinator::publish(const NativeExecutionAuthority& authority,
                                                 VerifiedPublishBundle&& bundle, PrecommitFailpoint failpoint)
{
  if (!authority.active() || authority.sessionToken() != _session_token) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidSessionSignal);
  }
  if (_state != TimingEditCoordinatorState::kVerified) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidState);
  }
  if (!isKnownFailpoint(failpoint)
      || (bundle._commit_fault_latch.armed()
          && !isKnownCommitLatchOrdinal(bundle._commit_fault_latch.armedOrdinal()))) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kPrecommitFault);
  }
  if (!bundle.nativeDatabaseInstalled() || !bundle.hostSnapshotReady()) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidState);
  }
  if (bundle._installed_database.executionSessionToken() != authority.sessionToken()
      || !bundle._installed_database.executionScopeMatches(authority)) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidSessionSignal);
  }

  const PreparedBatch& prepared_batch = bundle._prepared_batch;
  if (!matchesCurrentBatch(prepared_batch)) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kStaleCommittedIdentity);
  }
  const auto& target_post_identity = prepared_batch.targetPostCommittedIdentity();
  if (!prepared_batch.sealed() || !target_post_identity) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kUnsealedBatch);
  }
  if (target_post_identity->generation != prepared_batch.targetGeneration()
      || bundle._device_state.generation != prepared_batch.targetGeneration()
      || bundle._commit_marker.generation != prepared_batch.targetGeneration()) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidGeneration);
  }
  if (!isValidCumulativeSuccessor(bundle._cumulative_trace, prepared_batch)) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidTrace);
  }
  if (failpoint == PrecommitFailpoint::kPublishBeforeCommit) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kPrecommitFault);
  }

  try {
    Database& candidate_database = bundle._installed_database.database();
    if (!NativeTimingEditPreparer::matchesDesignName(candidate_database, _baseline.design_name)) {
      return rejectBeforeCommit(bundle, TransactionStatusCode::kCandidateDatabaseMismatch);
    }
    if (bundle._host_snapshot->input.source_identity.session_token != _session_token.value()) {
      return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidSessionSignal);
    }
    if (bundle._host_snapshot->input.source_identity.graph_generation != kStage3GraphGeneration) {
      return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidGeneration);
    }
    auto rebuilt_host = rebuildMatchingHostSnapshot(candidate_database, *bundle._host_snapshot, _session_token);
    if (rebuilt_host == nullptr) {
      return rejectBeforeCommit(bundle, TransactionStatusCode::kHostSnapshotMismatch);
    }
    csr::CsrComputationResult computation = csr::computeCsrSessionReference(*rebuilt_host);
    const auto* exact_result = std::get_if<csr::CsrTimingResult>(&computation.outcome);
    if (exact_result == nullptr
        || !bundle._csr_full_receipt.matches(_session_token, prepared_batch.targetGeneration(),
                                             rebuilt_host->input, *exact_result)) {
      return rejectBeforeCommit(bundle, TransactionStatusCode::kCsrFullReceiptMismatch);
    }
    if (!candidateMatchesPostIdentity(candidate_database, _baseline, *target_post_identity,
                                      rebuilt_host->input.analysis_context, _session_token)) {
      return rejectBeforeCommit(bundle, TransactionStatusCode::kInvalidPostIdentity);
    }
    TimingEditResolverState expected_resolver = _resolver_state.stage(prepared_batch);
    if (bundle._resolver_state != expected_resolver) {
      return rejectBeforeCommit(bundle, TransactionStatusCode::kResolverMismatch);
    }
    NativeTimingEditPreparer::requireExactCommittedTraceClosure(candidate_database, rebuilt_host->input,
                                                                expected_resolver, _baseline,
                                                                bundle._cumulative_trace.batches);
    bundle._host_snapshot.swap(rebuilt_host);
  } catch (...) {
    return rejectBeforeCommit(bundle, TransactionStatusCode::kPublishVerificationFailed);
  }

  return completePublishNoexcept(bundle);
}

TransactionStatus TimingEditCoordinator::completePublishNoexcept(VerifiedPublishBundle& bundle) noexcept
{
  static_assert(noexcept(std::declval<InstalledNativeDatabase&>().commit()));
  static_assert(std::is_nothrow_swappable_v<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>);
  static_assert(std::is_nothrow_swappable_v<CommittedEditIdentity>);
  static_assert(std::is_nothrow_swappable_v<CumulativeEditTrace>);
  static_assert(std::is_nothrow_swappable_v<TimingEditResolverState>);
  static_assert(std::is_nothrow_copy_assignable_v<DeviceCsrState>);
  static_assert(std::is_nothrow_copy_assignable_v<CommitMarker>);

  // This is the sole commit boundary. There are no allocations, user
  // callbacks, throwing operations, or early returns until the full g+1 is
  // installed and the final session signal has been emitted.
  bundle._installed_database.commit();
  bundle._commit_fault_latch.check(CommitLatchOrdinal::kDatabaseCanonical);

  _host_snapshot.swap(bundle._host_snapshot);
  bundle._commit_fault_latch.check(CommitLatchOrdinal::kHostSnapshot);

  std::swap(_committed_identity, *bundle._prepared_batch._target_post_committed_identity);
  std::swap(_cumulative_trace, bundle._cumulative_trace);
  std::swap(_resolver_state, bundle._resolver_state);
  bundle._commit_fault_latch.check(CommitLatchOrdinal::kTransactionSidecars);

  _device_state = bundle._device_state;
  bundle._commit_fault_latch.check(CommitLatchOrdinal::kDeviceState);

  _commit_marker = bundle._commit_marker;

  const bool poisoned = bundle._commit_fault_latch.latched() || bundle._publish_after_commit_fault;
  if (poisoned) {
    _state = TimingEditCoordinatorState::kPoisoned;
    _session_signal.set(SessionTransactionState::kPoisoned);
  } else {
    _state = TimingEditCoordinatorState::kIdle;
    _session_signal.set(SessionTransactionState::kBaselineExecuted);
  }
  return status(poisoned ? TransactionStatusCode::kPoisonedCommit : TransactionStatusCode::kOk);
}

TransactionStatus TimingEditCoordinator::acknowledgePoisonedSessionClosed(TimingEditSessionToken closed_session_token) noexcept
{
  if (_state != TimingEditCoordinatorState::kPoisoned) {
    return status(TransactionStatusCode::kInvalidState);
  }
  if (closed_session_token != _session_token) {
    return status(TransactionStatusCode::kInvalidSessionSignal);
  }
  _state = TimingEditCoordinatorState::kRecovering;
  return status(TransactionStatusCode::kOk);
}

bool TimingEditCoordinator::isValidFreshRecovery(const FreshRecoveryBundle& bundle) const
{
  return bundle._new_session_token.value() != 0 && bundle._new_session_token != _session_token && bundle._new_session_signal.valid()
         && bundle._host_snapshot != nullptr && bundle._committed_identity == _committed_identity
         && bundle._cumulative_trace == _cumulative_trace && bundle._device_state == _device_state
         && bundle._resolver_state == _resolver_state
         && isWellFormedTrace(bundle._cumulative_trace, _baseline, bundle._committed_identity.generation);
}

TransactionStatus TimingEditCoordinator::installFreshRecovery(const NativeExecutionAuthority& authority,
                                                              FreshRecoveryBundle&& bundle)
{
  if (_state != TimingEditCoordinatorState::kRecovering) {
    return status(TransactionStatusCode::kInvalidState);
  }
  if (!authority.active() || !isValidFreshRecovery(bundle)) {
    return status(TransactionStatusCode::kRecoveryMismatch);
  }
  if (authority.sessionToken() != bundle._new_session_token) {
    return status(TransactionStatusCode::kInvalidSessionSignal);
  }
  try {
    Database& fresh_database = NativeDatabaseCandidate::activeDatabaseForVerification();
    if (!NativeTimingEditPreparer::matchesDesignName(fresh_database, _baseline.design_name)
        || bundle._host_snapshot->input.source_identity.session_token != bundle._new_session_token.value()
        || bundle._host_snapshot->input.source_identity.graph_generation != kStage3GraphGeneration) {
      return status(TransactionStatusCode::kRecoveryMismatch);
    }

    std::unique_ptr<csr::CsrCommittedSessionSnapshot> rebuilt_host
        = rebuildMatchingHostSnapshot(fresh_database, *bundle._host_snapshot, bundle._new_session_token);
    if (rebuilt_host != nullptr) {
      csr::CsrComputationResult computation = csr::computeCsrSessionReference(*rebuilt_host);
      const auto* exact_result = std::get_if<csr::CsrTimingResult>(&computation.outcome);
      if (exact_result == nullptr
          || !bundle._csr_full_receipt.matches(bundle._new_session_token, bundle._committed_identity.generation,
                                               rebuilt_host->input, *exact_result)) {
        return status(TransactionStatusCode::kRecoveryMismatch);
      }
    }
    if (rebuilt_host == nullptr
        || !candidateMatchesPostIdentity(fresh_database, _baseline, bundle._committed_identity,
                                         rebuilt_host->input.analysis_context, bundle._new_session_token)) {
      return status(TransactionStatusCode::kRecoveryMismatch);
    }
    NativeTimingEditPreparer::requireExactCommittedTraceClosure(fresh_database, rebuilt_host->input,
                                                                bundle._resolver_state, _baseline,
                                                                bundle._cumulative_trace.batches);

    // Recovery publishes the snapshot rebuilt from the currently installed
    // fresh Database, rather than trusting a publicly constructible bundle's
    // observation sidecars. All allocation and validation ends before the
    // no-throw recovery install begins.
    bundle._host_snapshot.swap(rebuilt_host);
  } catch (...) {
    return status(TransactionStatusCode::kRecoveryMismatch);
  }
  return completeRecoveryNoexcept(bundle);
}

TransactionStatus TimingEditCoordinator::completeRecoveryNoexcept(FreshRecoveryBundle& bundle) noexcept
{
  static_assert(std::is_nothrow_swappable_v<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>);
  static_assert(std::is_nothrow_swappable_v<CommittedEditIdentity>);
  static_assert(std::is_nothrow_swappable_v<CumulativeEditTrace>);
  static_assert(std::is_nothrow_swappable_v<TimingEditResolverState>);
  static_assert(std::is_nothrow_copy_assignable_v<DeviceCsrState>);
  static_assert(std::is_nothrow_copy_assignable_v<TimingEditSessionToken>);
  static_assert(std::is_nothrow_copy_assignable_v<SessionStateSignal>);
  static_assert(std::is_nothrow_copy_assignable_v<CommitMarker>);

  _host_snapshot.swap(bundle._host_snapshot);
  std::swap(_committed_identity, bundle._committed_identity);
  std::swap(_cumulative_trace, bundle._cumulative_trace);
  std::swap(_resolver_state, bundle._resolver_state);
  _device_state = bundle._device_state;
  _session_token = bundle._new_session_token;
  _session_signal = bundle._new_session_signal;
  _commit_marker = CommitMarker{_committed_identity.generation};
  _state = TimingEditCoordinatorState::kIdle;
  _session_signal.set(SessionTransactionState::kBaselineExecuted);
  return status(TransactionStatusCode::kOk);
}

TransactionStatus TimingEditCoordinator::acknowledgeSessionClosed(TimingEditSessionToken closed_session_token) noexcept
{
  if (_state != TimingEditCoordinatorState::kIdle && _state != TimingEditCoordinatorState::kPoisoned
      && _state != TimingEditCoordinatorState::kRecovering) {
    return status(TransactionStatusCode::kInvalidState);
  }
  if (closed_session_token != _session_token) {
    return status(TransactionStatusCode::kInvalidSessionSignal);
  }
  _state = TimingEditCoordinatorState::kClosed;
  return status(TransactionStatusCode::kOk);
}

}  // namespace ista::timing_edit
