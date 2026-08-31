// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "TimingEditTransactionNativeSupport.hpp"

#include <any>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "DataManager.hpp"
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "Logger.hpp"
#include "NativeDatabaseValidation.hpp"
#include "STAInterface.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"

namespace ista {

class StaInterfaceLifecycleTestAccess final
{
 public:
  static void acquireSession(std::uint64_t token)
  {
    STAInterface& interface = STAInterface::getInst();
    std::scoped_lock lock(interface._lifecycle_mutex);
    const StaLeaseResult result = interface._lifecycle.acquireSession(token);
    if (!result.succeeded) {
      throw std::runtime_error("test failed to acquire STA session owner lease: "
                               + result.message);
    }
  }

  static void releaseSession() noexcept
  {
    STAInterface& interface = STAInterface::getInst();
    std::scoped_lock lock(interface._lifecycle_mutex);
    interface._lifecycle.completeClose(true);
  }
};

}  // namespace ista

namespace ista::timing_edit::test {
namespace {

class FakeSessionOperations final : public StaSessionOperations
{
 public:
  StaStatus initialize(std::uint64_t, std::map<std::string, std::any>&) override
  {
    return StaStatus::success(StaSessionState::kInitialized);
  }

  StaStatus runFullBaseline(std::uint64_t token,
                            StaCsrSessionContext* context) override
  {
    if (context == nullptr || context->token() != token) {
      return StaStatus::failure(StaSessionStatusCode::kRunFailed,
                                StaSessionState::kInitialized,
                                "real test snapshot context/token mismatch");
    }
    StaCsrCaptureAttempt attempt
        = StaTimingEditSessionProbeAccess::captureSnapshotForTesting(
            *context, STADM.getDatabase());
    const auto* capture = std::get_if<csr::CsrCaptureResult>(&attempt.value);
    if (capture == nullptr || capture->error.has_value()) {
      return StaStatus::failure(StaSessionStatusCode::kRunFailed,
                                StaSessionState::kInitialized,
                                "real test CSR snapshot capture failed");
    }
    return StaStatus::success(StaSessionState::kBaselineExecuted);
  }

  StaStatus close(
      std::uint64_t, StaCsrSessionContext* context,
      std::unique_ptr<csr::CsrSnapshotObservation>* released_out) override
  {
    if (context != nullptr) {
      StaTimingEditSessionProbeAccess::releaseSnapshotForMutation(
          *context, released_out);
    }
    return StaStatus::success(StaSessionState::kClosed);
  }

  StaStatus copyObservation(
      std::uint64_t, StaCsrSessionContext* context,
      std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) override
  {
    if (context == nullptr
        || !StaTimingEditSessionProbeAccess::copyObservationForTesting(
            *context, detached_out)) {
      return StaStatus::failure(StaSessionStatusCode::kInvalidState,
                                StaSessionState::kBaselineExecuted,
                                "real test CSR observation is unavailable");
    }
    return StaStatus::success(StaSessionState::kBaselineExecuted);
  }

  StaCsrReferenceAttempt computeCsrReferenceForTesting(
      std::uint64_t, StaCsrSessionContext* context) override
  {
    if (context == nullptr) {
      return {.value = StaCsrSessionOperationFailure{}};
    }
    return StaTimingEditSessionProbeAccess::computeReferenceForTesting(*context);
  }

  StaCsrScatterAttempt scatterCsrTimingResultForTesting(
      std::uint64_t, StaCsrSessionContext* context,
      const csr::CsrTimingResult& result) override
  {
    if (context == nullptr) {
      return {.value = csr::CsrTimingResultScatterAttempt{}};
    }
    return StaTimingEditSessionProbeAccess::scatterForTesting(
        *context, STADM.getDatabase(), result);
  }
};

void resetDelayRuntime()
{
  try {
    DelayCalculator::initInst();
    STADC.destroy();
  } catch (...) {
  }
  DelayCalculator::destroyInst();
}

}  // namespace

NativeTransactionRuntime::NativeTransactionRuntime()
{
  Logger::initInst();
  DataManager::initInst();
}

NativeTransactionRuntime::~NativeTransactionRuntime()
{
  resetDelayRuntime();
  DataManager::destroyInst();
  Logger::destroyInst();
}

GuardedTestSession::GuardedTestSession()
{
  StaExecutionRequest request{StaBackend::kCpu, StaUpdateStrategy::kIncremental,
                              StaFallbackPolicy::kError};
  _session = std::make_unique<IncrementalStaSession>(
      request, std::make_unique<FakeSessionOperations>());
  const csr::StaAnalysisContext context = editTestAnalysisContext();
  if (!_session->initialize(context, {}).ok() || !_session->runFullBaseline().ok()
      || _session->state() != StaSessionState::kBaselineExecuted
      || !_session->sessionToken().has_value()) {
    throw std::runtime_error("fake incremental session baseline failed");
  }
  _token = TimingEditSessionToken{*_session->sessionToken()};
  StaInterfaceLifecycleTestAccess::acquireSession(_token.value());
  _lease_active = true;
}

GuardedTestSession::~GuardedTestSession()
{
  if (_lease_active) {
    StaInterfaceLifecycleTestAccess::releaseSession();
  }
}

StaStatus GuardedTestSession::closeAndRelease()
{
  const StaStatus status = _session->close();
  if (_lease_active) {
    StaInterfaceLifecycleTestAccess::releaseSession();
    _lease_active = false;
  }
  return status;
}

void installNativeTransactionBaseline()
{
  resetDelayRuntime();
  NativeEditFixture fixture = makeNativeEditFixture();
  STADM.getDatabase() = std::move(fixture.database());
  const NativeDatabaseValidationResult failure
      = validateNativeDatabase(STADM.getDatabase());
  if (failure.has_value()) {
    throw std::runtime_error("real transaction baseline is invalid: "
                             + failure->object + ": " + failure->detail);
  }
}

PreparedNativeTransaction prepareNativeTransaction(
    TimingEditSessionToken session_token)
{
  constexpr TransactionGeneration kGeneration{0};
  Database& database = STADM.getDatabase();
  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(database);
  const CommittedEditIdentity initial_identity
      = NativeTimingEditPreparer::captureCurrentIdentity(
          database, kGeneration, editTestAnalysisContext(), session_token);
  const BaselineSemanticIdentity baseline{
      database.get_design_name(), initial_identity.current_fingerprints,
      initial_identity.canonical_pre_state};
  DesignEditPlan plan = bufferInsertionPlan(
      database, baseline, kGeneration, editTestCreationToken());
  const TimingEditPrepareContext context{
      session_token, baseline, initial_identity, editTestAnalysisContext()};
  PreparedBatch prepared
      = NativeTimingEditPreparer::prepare(database, plan, context, resolver);
  return {baseline, initial_identity, std::move(resolver), std::move(plan),
          std::move(prepared)};
}

std::unique_ptr<csr::CsrCommittedSessionSnapshot> buildNativeHostSnapshot(
    const Database& database, TimingEditSessionToken session_token)
{
  csr::CsrSessionSnapshotBuildResult result = csr::buildCsrSessionSnapshot(
      database, editTestAnalysisContext(), session_token.value());
  auto* snapshot
      = std::get_if<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>(&result);
  if (snapshot == nullptr || *snapshot == nullptr) {
    throw std::runtime_error("real transaction host CSR snapshot build failed");
  }
  return std::move(*snapshot);
}

NativeDatabaseCandidate translateNativeCandidate(
    const PreparedNativeTransaction& transaction,
    const TimingEditResolverState& staged_resolver)
{
  NativeDatabaseCandidate candidate
      = NativeDatabaseCandidate::create(STADM.getDatabase());
  const NativeTranslationResult translated
      = NativeDesignEditTranslator::applyNormalizedBatch(
          candidate.database(), transaction.prepared, staged_resolver);
  if (!translated.applied() || !translated.reasons.empty()) {
    throw std::runtime_error("real transaction native translation fell back");
  }
  return candidate;
}

void requireReleasedSnapshotForNativeMutation(
    StaCsrSessionContext& session_context,
    TimingEditSessionToken session_token)
{
  std::unique_ptr<csr::CsrSnapshotObservation> released_observation;
  StaTimingEditSessionProbeAccess::releaseSnapshotForMutation(
      session_context, &released_observation);
  if (released_observation == nullptr
      || released_observation->state != csr::CsrSnapshotState::kReleased
      || released_observation->identity.session_token
             != session_token.value()) {
    throw std::runtime_error(
        "native mutation did not release the old CSR snapshot first");
  }
}

ExecutedNativeTransaction executeNativeTransaction(
    const NativeExecutionAuthority& authority,
    StaCsrSessionContext& session_context,
    const PreparedNativeTransaction& transaction)
{
  requireReleasedSnapshotForNativeMutation(session_context,
                                           authority.sessionToken());
  TimingEditResolverState staged_resolver
      = transaction.resolver.stage(transaction.prepared);
  NativeDatabaseCandidate candidate
      = translateNativeCandidate(transaction, staged_resolver);
  InstalledNativeDatabase installed = NativeCandidateExecutor::installAndExecute(
      authority, std::move(candidate));
  std::unique_ptr<csr::CsrCommittedSessionSnapshot> host_snapshot
      = buildNativeHostSnapshot(installed.database(), authority.sessionToken());
  CsrFullExactReceipt csr_full_receipt
      = StaTimingEditSessionProbeAccess::proveCsrFullExact(
          authority, *host_snapshot, installed.database(),
          transaction.prepared.targetGeneration());
  const CommittedEditIdentity post_identity
      = NativeTimingEditPreparer::captureCurrentIdentity(
          installed.database(), transaction.prepared.targetGeneration(),
          editTestAnalysisContext(), authority.sessionToken());
  const TokenMaterialization materialization = staged_resolver.materialization(
      transaction.plan.edits.front().creation_token);
  return {std::move(installed), std::move(host_snapshot),
          std::move(csr_full_receipt), post_identity,
          std::move(staged_resolver), materialization};
}

CumulativeEditTrace nativeSuccessorTrace(
    const PreparedNativeTransaction& transaction)
{
  DesignEditPlan plan;
  plan.baseline = transaction.baseline;
  plan.expected_generation = transaction.prepared.expectedGeneration();
  plan.edits = transaction.prepared.normalizedEdits();
  return CumulativeEditTrace{{std::move(plan)}};
}

void requireRecursiveStaOperationRejected(std::uint64_t token,
                                          StaCsrSessionContext& context)
{
  std::unique_ptr<csr::CsrSnapshotObservation> detached;
  const StaStatus nested = STAInterface::getInst().copySessionCsrSnapshotObservation(
      token, &context, &detached);
  if (nested.code != StaSessionStatusCode::kInvalidState || detached != nullptr
      || nested.message.find("recursive") == std::string::npos) {
    throw std::runtime_error(
        "public recursive STA operation was not rejected inside edit guard");
  }
}

}  // namespace ista::timing_edit::test
