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
#include "TimingResultScatter.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "CsrReferenceValidator.hpp"
#include "Database.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista::csr {
namespace {

static_assert(std::is_nothrow_move_constructible_v<std::string>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<std::string>>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<CsrInvalidationReason>>);
static_assert(std::is_nothrow_move_constructible_v<CsrScatterResult>);
static_assert(std::is_nothrow_move_constructible_v<CsrCurrentFingerprints>);
static_assert(std::is_nothrow_move_constructible_v<CsrReferenceInputExportFailure>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<CsrReferenceInputExportFailure>>);
static_assert(std::is_nothrow_move_constructible_v<CsrTimingResultScatterAttempt>);
static_assert(std::is_nothrow_move_assignable_v<detail::PreparedScatterEnvelope>);
static_assert(std::is_nothrow_destructible_v<detail::PreparedClockState>);
static_assert(std::is_nothrow_destructible_v<detail::PreparedTimingPointState>);
static_assert(std::is_nothrow_destructible_v<detail::PreparedArcState>);
static_assert(std::is_nothrow_destructible_v<detail::PreparedTimingResult>);
static_assert(std::is_nothrow_destructible_v<detail::PreparedScatterEnvelope>);
static_assert(std::is_nothrow_destructible_v<CsrTimingResultScatterAttempt>);

[[nodiscard]] CsrTimingResultScatterAttempt failureAttempt(CsrScatterCode code, CsrCurrentFingerprints current_fingerprints = {},
                                                           std::optional<CsrReferenceInputExportFailure> capture_error = std::nullopt,
                                                           std::optional<CsrInvalidationReason> invalidation_reason = std::nullopt)
{
  return CsrTimingResultScatterAttempt{CsrScatterResult{code, 0, invalidation_reason}, std::move(current_fingerprints), std::move(capture_error)};
}

}  // namespace

namespace detail {

CsrScatterCode publishComputedTimingResult(Database& database, const CsrReferenceInput& input, const CsrTimingResult& result, std::uint64_t session_token)
{
  if (!validTimingScatterGuard(input, session_token, input.source_identity.graph_generation))
    return CsrScatterCode::kInvalidState;
  if (result.source_identity != input.source_identity)
    return CsrScatterCode::kSourceIdentityMismatch;
  if constexpr (incregpusta_debug_checks_enabled)
    if (const auto error = firstTimingResultScatterFailure(input, result))
      return *error;
  auto preparation = prepareTimingResultScatter(database, input, input, result);
  if (const auto* error = std::get_if<TimingResultPreparationFailure>(&preparation))
    return error->code;
  auto& prepared = std::get<PreparedTimingResult>(preparation);
  if (!timingScatterWriteCount(prepared.nodes.size(), prepared.arcs.size()))
    return CsrScatterCode::kShapeMismatch;
  publishPreparedTimingResult(prepared);
  return CsrScatterCode::kSuccess;
}

[[nodiscard]] PrepublishScatterResult prepareScatterAttempt(ista::Database& database, const CsrReferenceInput& committed_input, const CsrTimingResult& result,
                                                            const StaAnalysisContext& current_context, std::uint64_t active_session_token,
                                                            std::uint64_t active_graph_generation)
{
  if (!detail::validTimingScatterGuard(committed_input, active_session_token, active_graph_generation))
    return failureAttempt(CsrScatterCode::kInvalidState);

  CsrReferenceInputExportResult capture = exportCsrReferenceInputCandidate(database, current_context, active_session_token);
  if (auto* failure = std::get_if<CsrReferenceInputExportFailure>(&capture)) {
    CsrCurrentFingerprints current_fingerprints = failure->current_fingerprints;
    if (failure->failed_phase == CsrReferenceInputExportPhase::kAnalysisContextPreflight)
      return failureAttempt(CsrScatterCode::kInvalidState, std::move(current_fingerprints), std::optional<CsrReferenceInputExportFailure>{std::move(*failure)});

    const auto invalidation_reason = firstCsrInvalidationReason(committed_input.source_identity, current_fingerprints);
    if (!invalidation_reason)
      throw std::logic_error("non-preflight CSR current capture failure did not identify an invalidated fingerprint family");
    return failureAttempt(CsrScatterCode::kSnapshotInvalidated, std::move(current_fingerprints),
                          std::optional<CsrReferenceInputExportFailure>{std::move(*failure)}, invalidation_reason);
  }

  CsrReferenceInput current_input = std::move(std::get<CsrReferenceInput>(capture));
  CsrCurrentFingerprints current_fingerprints = detail::currentFingerprints(current_input.source_identity);
  if (const auto invalidation_reason = firstCsrInvalidationReason(committed_input.source_identity, current_fingerprints))
    return failureAttempt(CsrScatterCode::kSnapshotInvalidated, std::move(current_fingerprints), std::nullopt, invalidation_reason);

  const auto result_failure = detail::firstTimingResultScatterFailure(committed_input, result);
  if (result_failure && *result_failure != CsrScatterCode::kPredecessorMismatch)
    return failureAttempt(*result_failure, std::move(current_fingerprints));

  detail::TimingResultPreparationResult preparation = detail::prepareTimingResultScatter(database, committed_input, current_input, result);
  if (const auto* failure = std::get_if<detail::TimingResultPreparationFailure>(&preparation))
    return failureAttempt(failure->code, std::move(current_fingerprints));
  if (result_failure)
    return failureAttempt(*result_failure, std::move(current_fingerprints));
  return PreparedScatterEnvelope{std::move(std::get<detail::PreparedTimingResult>(preparation)), std::move(current_fingerprints)};
}

}  // namespace detail

CsrTimingResultScatterAttempt scatterCsrTimingResult(ista::Database& database, const CsrReferenceInput& committed_input, const CsrTimingResult& result,
                                                     const StaAnalysisContext& current_context, std::uint64_t active_session_token,
                                                     std::uint64_t active_graph_generation)
{
  detail::PreparedScatterEnvelope envelope;
  {
    detail::PrepublishScatterResult prepublish
        = detail::prepareScatterAttempt(database, committed_input, result, current_context, active_session_token, active_graph_generation);
    if (auto* failure = std::get_if<CsrTimingResultScatterAttempt>(&prepublish))
      return std::move(*failure);
    envelope = std::move(std::get<detail::PreparedScatterEnvelope>(prepublish));
  }

  std::uint64_t writes_applied = 0;
  {
    const auto checked_count = detail::timingScatterWriteCount(envelope.prepared.nodes.size(), envelope.prepared.arcs.size());
    if (!checked_count)
      return failureAttempt(CsrScatterCode::kShapeMismatch, std::move(envelope.current_fingerprints));
    writes_applied = *checked_count;
  }

  CsrTimingResultScatterAttempt success_attempt{CsrScatterResult{CsrScatterCode::kSuccess, writes_applied, std::nullopt},
                                                std::move(envelope.current_fingerprints), std::nullopt};
  detail::publishPreparedTimingResult(envelope.prepared);
  return success_attempt;
}

}  // namespace ista::csr
