// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "StaTimingEditSessionProbeAccess.hpp"

#include <exception>
#include <string>

#include "STAInterface.hpp"
#include "NativeCandidateExecutor.hpp"
#include "StaCsrSessionContext.hpp"
#include "CsrSnapshotObservation.hpp"

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

}  // namespace

StaStatus StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(IncrementalStaSession& session, const GuardedMutation& mutation)
{
  const StaSessionState state = session._state.load(std::memory_order_acquire);
  if (state != StaSessionState::kBaselineExecuted) {
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, state,
                              "timing edit requires a baseline-executed incremental session");
  }
  const std::uint64_t token = session._token.load(std::memory_order_acquire);
  if (token == 0 || session._context == nullptr) {
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, state,
                              "timing edit session token/context is unavailable");
  }
  if (!mutation) {
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, state,
                              "timing edit guarded mutation callback is empty");
  }

  StaStatus failure;
  auto guard = STAInterface::getInst().acquireUse(StaOwner::session(token), session._context.get(), true,
                                                   StaSessionState::kBaselineExecuted, &failure);
  if (!guard) {
    return failure;
  }
  try {
    const timing_edit::NativeExecutionScopeViolationSignal violation_signal{
        &session, &StaTimingEditSessionProbeAccess::poisonSessionOnScopeViolation};
    const timing_edit::NativeExecutionAuthority authority{
        timing_edit::TimingEditSessionToken{token}, violation_signal};
    return mutation(token, *session._context, authority);
  } catch (...) {
    return StaStatus::failure(StaSessionStatusCode::kRunFailed, session._state.load(std::memory_order_acquire),
                              "timing edit guarded mutation threw: " + exceptionMessage());
  }
}

void StaTimingEditSessionProbeAccess::releaseSnapshotForMutation(
    StaCsrSessionContext& context, std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) noexcept
{
  context.release(detached_out);
}

StaCsrCaptureAttempt StaTimingEditSessionProbeAccess::captureSnapshotForTesting(
    StaCsrSessionContext& context, const Database& database)
{
  return context.capture(database);
}

bool StaTimingEditSessionProbeAccess::copyObservationForTesting(
    StaCsrSessionContext& context,
    std::unique_ptr<csr::CsrSnapshotObservation>* detached_out)
{
  return context.copyObservation(detached_out);
}

StaCsrReferenceAttempt StaTimingEditSessionProbeAccess::computeReferenceForTesting(
    StaCsrSessionContext& context)
{
  return context.computeReference();
}

StaCsrScatterAttempt StaTimingEditSessionProbeAccess::scatterForTesting(
    StaCsrSessionContext& context, Database& database,
    const csr::CsrTimingResult& result)
{
  return context.scatter(database, result);
}

timing_edit::SessionStateSignal StaTimingEditSessionProbeAccess::stateSignal(IncrementalStaSession& session) noexcept
{
  return timing_edit::SessionStateSignal{&session, &StaTimingEditSessionProbeAccess::writeSessionState};
}

void StaTimingEditSessionProbeAccess::writeSessionState(void* opaque_session,
                                                        timing_edit::SessionTransactionState state) noexcept
{
  auto& session = *static_cast<IncrementalStaSession*>(opaque_session);
  switch (state) {
    case timing_edit::SessionTransactionState::kBaselineExecuted:
      session._state.store(StaSessionState::kBaselineExecuted, std::memory_order_release);
      break;
    case timing_edit::SessionTransactionState::kPoisoned:
      session._state.store(StaSessionState::kPoisoned, std::memory_order_release);
      break;
  }
}

void StaTimingEditSessionProbeAccess::poisonSessionOnScopeViolation(
    void* opaque_session) noexcept
{
  auto& session = *static_cast<IncrementalStaSession*>(opaque_session);
  session._state.store(StaSessionState::kPoisoned, std::memory_order_release);
}

}  // namespace ista
