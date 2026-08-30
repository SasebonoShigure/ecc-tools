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
#include "CsrSessionSnapshotAdapter.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrFifoLevelization.hpp"
#include "CsrSessionSnapshotAdapterInternal.hpp"
#include "CsrSnapshotObservationTransition.hpp"
#include "CsrSnapshotObservationValidator.hpp"

namespace ista::csr {
namespace {

using Clock = std::chrono::steady_clock;
using Violations = std::vector<CsrContractViolation>;

static_assert(std::is_nothrow_move_constructible_v<std::string>);
static_assert(std::is_nothrow_move_constructible_v<std::vector<std::uint64_t>>);
static_assert(std::is_nothrow_move_constructible_v<CsrReferenceInput>);
static_assert(std::is_nothrow_move_constructible_v<CsrCaptureMetadata>);
static_assert(std::is_nothrow_move_constructible_v<CsrSnapshotObservation>);
static_assert(std::is_nothrow_move_constructible_v<CsrCommittedSessionSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<CsrComputationResult>);
static_assert(std::is_nothrow_move_constructible_v<CsrTimingResultScatterAttempt>);
static_assert(std::is_nothrow_move_constructible_v<detail::PreparedSessionObservationPair>);
static_assert(std::is_nothrow_destructible_v<CsrCommittedSessionSnapshot>);
static_assert(std::is_nothrow_destructible_v<CsrComputationResult>);
static_assert(std::is_nothrow_destructible_v<CsrTimingResultScatterAttempt>);
static_assert(std::is_nothrow_destructible_v<detail::PreparedScatterEnvelope>);
static_assert(std::is_nothrow_destructible_v<detail::PreparedSessionObservationPair>);
static_assert(std::is_nothrow_swappable_v<std::unique_ptr<CsrSnapshotObservation>>);

[[noreturn]] void throwContractFailure(const Violations& errors, std::string_view operation)
{
  if (errors.empty())
    throw std::logic_error(std::string(operation) + " failed without a contract violation");
  throw std::logic_error(std::string(operation) + ": " + errors.front().field + ": " + errors.front().message);
}

[[nodiscard]] CsrSnapshotObservation unwrapObservation(CsrObservationResult result, std::string_view operation)
{
  if (const auto* errors = std::get_if<Violations>(&result))
    throwContractFailure(*errors, operation);
  return std::move(std::get<CsrSnapshotObservation>(result));
}

[[nodiscard]] CsrSnapshotObservation unwrapOptionalObservation(CsrOptionalObservationResult result, std::string_view operation)
{
  if (const auto* errors = std::get_if<Violations>(&result))
    throwContractFailure(*errors, operation);
  auto observation = std::move(std::get<CsrOptionalObservation>(result));
  if (!observation)
    throw std::logic_error(std::string(operation) + " returned no observation");
  return std::move(*observation);
}

[[nodiscard]] double durationMilliseconds(Clock::time_point start, Clock::time_point stop)
{
  const double duration = std::chrono::duration<double, std::milli>(stop - start).count();
  if (!std::isfinite(duration) || duration < 0.0)
    throw std::logic_error("steady clock produced an invalid CSR duration");
  return duration;
}

[[nodiscard]] CsrSnapshotObservation makeReleasedObservation(const CsrSnapshotObservation& active)
{
  CsrOptionalObservation before{active};
  return unwrapOptionalObservation(reduceSnapshotObservation(before, ReleaseAction{}), "CSR release observation reduction");
}

[[nodiscard]] detail::PreparedSessionObservationPair prepareObservationPair(const CsrSnapshotObservation& before, const CsrObservationAction& action)
{
  CsrOptionalObservation optional_before{before};
  CsrSnapshotObservation active = unwrapOptionalObservation(reduceSnapshotObservation(optional_before, action), "CSR active observation reduction");
  CsrSnapshotObservation released = makeReleasedObservation(active);
  return detail::PreparedSessionObservationPair{std::make_unique<CsrSnapshotObservation>(std::move(active)),
                                                std::make_unique<CsrSnapshotObservation>(std::move(released))};
}

void requireLiveReadySnapshot(const CsrCommittedSessionSnapshot& snapshot)
{
  if (!snapshot.active_observation || !snapshot.released_observation)
    throw std::logic_error("CSR committed session snapshot is missing an observation");
  if (snapshot.active_observation->state != CsrSnapshotState::kReady || snapshot.released_observation->state != CsrSnapshotState::kReleased)
    throw std::logic_error("CSR committed session snapshot is not ready");
  if (snapshot.input.source_identity != snapshot.active_observation->identity || snapshot.input.source_identity != snapshot.released_observation->identity)
    throw std::logic_error("CSR committed session snapshot identity drifted");
}

[[nodiscard]] CsrTimingResultScatterAttempt scatterFailureAttempt(CsrScatterCode code, CsrCurrentFingerprints current_fingerprints)
{
  return CsrTimingResultScatterAttempt{CsrScatterResult{code, 0, std::nullopt}, std::move(current_fingerprints), std::nullopt};
}

[[nodiscard]] CsrObservationAction scatterObservationAction(const CsrTimingResultScatterAttempt& attempt, const CsrTimingResult& result, double duration_ms)
{
  const CsrScatterCode code = attempt.scatter_result.code;
  switch (code) {
    case CsrScatterCode::kSuccess:
      return ScatterSuccessAction{duration_ms};
    case CsrScatterCode::kSnapshotInvalidated:
      return InvalidationAction{attempt.current_fingerprints, duration_ms};
    case CsrScatterCode::kSchemaMismatch:
      return ScatterRejectionAction{code, std::nullopt, duration_ms};
    case CsrScatterCode::kSessionMismatch:
    case CsrScatterCode::kGenerationMismatch:
    case CsrScatterCode::kSourceIdentityMismatch:
    case CsrScatterCode::kShapeMismatch:
    case CsrScatterCode::kResultNumericInvalid:
    case CsrScatterCode::kIdMappingMismatch:
    case CsrScatterCode::kPredecessorMismatch:
      return ScatterRejectionAction{code, result.source_identity, duration_ms};
    case CsrScatterCode::kInvalidState:
      throw std::logic_error("invalid-state scatter must not form an observation event");
  }
  throw std::logic_error("unknown CSR scatter code");
}

}  // namespace

CsrSessionSnapshotBuildResult buildCsrSessionSnapshot(const Database& database, const StaAnalysisContext& context, std::uint64_t session_token)
{
  const Clock::time_point start = Clock::now();
  CsrReferenceInputExportResult export_result = exportCsrReferenceInputCandidate(database, context, session_token);
  if (const auto* failure = std::get_if<CsrReferenceInputExportFailure>(&export_result))
    return failure->code;

  CsrReferenceInput input = std::move(std::get<CsrReferenceInput>(export_result));
  CsrFifoLevelization levelization = replayFifoLevelization(input);
  if (!levelization.complete)
    return CsrCaptureCode::kActiveCycle;
  const Clock::time_point stop = Clock::now();
  const double export_ms = durationMilliseconds(start, stop);

  CsrCaptureMetadata metadata{std::move(levelization.level_offsets), std::move(levelization.level_nodes), std::move(levelization.node_to_level)};
  CsrSnapshotObservation active = unwrapObservation(makeReadySnapshotObservation(input, metadata, export_ms), "CSR ready observation construction");
  CsrSnapshotObservation released = makeReleasedObservation(active);
  auto active_observation = std::make_unique<CsrSnapshotObservation>(std::move(active));
  auto released_observation = std::make_unique<CsrSnapshotObservation>(std::move(released));
  auto candidate = std::make_unique<CsrCommittedSessionSnapshot>(
      CsrCommittedSessionSnapshot{std::move(input), std::move(metadata), std::move(active_observation), std::move(released_observation)});
  return candidate;
}

CsrComputationResult computeCsrSessionReference(CsrCommittedSessionSnapshot& snapshot)
{
  requireLiveReadySnapshot(snapshot);

  CsrCpuReferenceBackend backend;
  const Clock::time_point start = Clock::now();
  CsrComputationResult result = backend.compute(snapshot.input);
  const Clock::time_point stop = Clock::now();
  const double reference_ms = durationMilliseconds(start, stop);

  detail::PreparedSessionObservationPair observation_pair = prepareObservationPair(*snapshot.active_observation, ReferenceMeasurementAction{reference_ms});
  detail::publishPreparedSessionObservationPair(snapshot, observation_pair);
  return result;
}

CsrTimingResultScatterAttempt scatterCsrSessionTimingResult(Database& database, CsrCommittedSessionSnapshot& snapshot, const CsrTimingResult& result,
                                                            const StaAnalysisContext& current_context, std::uint64_t active_session_token)
{
  requireLiveReadySnapshot(snapshot);

  const Clock::time_point start = Clock::now();
  detail::PrepublishScatterResult prepublish
      = detail::prepareScatterAttempt(database, snapshot.input, result, current_context, active_session_token, snapshot.input.source_identity.graph_generation);
  if (auto* failure = std::get_if<CsrTimingResultScatterAttempt>(&prepublish)) {
    const Clock::time_point stop = Clock::now();
    const double scatter_ms = durationMilliseconds(start, stop);
    CsrTimingResultScatterAttempt attempt = std::move(*failure);
    if (attempt.scatter_result.code == CsrScatterCode::kInvalidState)
      return attempt;

    CsrObservationAction action = scatterObservationAction(attempt, result, scatter_ms);
    detail::PreparedSessionObservationPair observation_pair = prepareObservationPair(*snapshot.active_observation, action);
    detail::publishPreparedSessionObservationPair(snapshot, observation_pair);
    return attempt;
  }

  detail::PreparedScatterEnvelope envelope = std::move(std::get<detail::PreparedScatterEnvelope>(prepublish));
  const auto checked_count = detail::timingScatterWriteCount(envelope.prepared.nodes.size(), envelope.prepared.arcs.size());
  if (!checked_count) {
    const Clock::time_point stop = Clock::now();
    const double scatter_ms = durationMilliseconds(start, stop);
    CsrTimingResultScatterAttempt attempt = scatterFailureAttempt(CsrScatterCode::kShapeMismatch, std::move(envelope.current_fingerprints));
    CsrObservationAction action = scatterObservationAction(attempt, result, scatter_ms);
    detail::PreparedSessionObservationPair observation_pair = prepareObservationPair(*snapshot.active_observation, action);
    detail::publishPreparedSessionObservationPair(snapshot, observation_pair);
    return attempt;
  }

  CsrTimingResultScatterAttempt success_attempt{CsrScatterResult{CsrScatterCode::kSuccess, *checked_count, std::nullopt},
                                                std::move(envelope.current_fingerprints), std::nullopt};
  const Clock::time_point stop = Clock::now();
  const double scatter_ms = durationMilliseconds(start, stop);
  CsrObservationAction action = scatterObservationAction(success_attempt, result, scatter_ms);
  detail::PreparedSessionObservationPair observation_pair = prepareObservationPair(*snapshot.active_observation, action);
  detail::publishPreparedTimingResultAndSessionObservationPair(envelope.prepared, snapshot, observation_pair);
  return success_attempt;
}

void releaseCsrSessionSnapshot(CsrCommittedSessionSnapshot& snapshot) noexcept
{
  detail::publishReleasedSessionObservation(snapshot);
}

}  // namespace ista::csr
