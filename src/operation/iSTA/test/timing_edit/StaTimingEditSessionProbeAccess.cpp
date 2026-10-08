// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "StaTimingEditSessionProbeAccess.hpp"
#include "StaEditOperation.hpp"

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

StaStatus StaTimingEditSessionProbeAccess::runTimingOnlyBaseline(
    std::uint64_t token, StaCsrSessionContext* context, StaBackend backend)
{
  auto& interface = STAInterface::getInst();
  return backend == StaBackend::kCuda
      ? interface.runOwnedCudaBaseline(token, context, false)
      : interface.runOwned(StaOwner::session(token), context, false);
}

StaStatus StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(IncrementalStaSession& session, const GuardedMutation& mutation)
{
  if (!mutation)
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, session.state(),
                              "timing edit guarded mutation callback is empty");
  try {
    StaEditOperation operation(session);
    return mutation(operation.token(), operation.context(), operation.authority());
  } catch (const StaSessionException& error) {
    return error.status();
  } catch (...) {
    return StaStatus::failure(StaSessionStatusCode::kRunFailed, session._state.load(std::memory_order_acquire),
                              "timing edit guarded mutation threw: " + exceptionMessage());
  }
}

void StaTimingEditSessionProbeAccess::inspectDatabase(std::uint64_t token, const std::function<void(const Database&)>& callback)
{
  STAInterface::getInst().inspectSessionDatabaseForTesting(token, callback);
}
std::unique_lock<std::mutex> StaTimingEditSessionProbeAccess::lockSessionApiForTesting(IncrementalStaSession& session)
{
  return std::unique_lock(session._api_mutex);
}

bool StaTimingEditSessionProbeAccess::snapshotReleasedForTesting(const StaCsrSessionContext& context) noexcept
{
  return context._substate == StaCsrSessionSubstate::kReleased;
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


}  // namespace ista
