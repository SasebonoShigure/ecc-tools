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
#include "StaCsrSessionContext.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "CsrReferenceValidator.hpp"
#include "CsrSessionSnapshotAdapter.hpp"

namespace ista {
namespace {

using CaptureValue = decltype(StaCsrCaptureAttempt::value);
using ReferenceValue = decltype(StaCsrReferenceAttempt::value);

static_assert(std::is_nothrow_move_constructible_v<CaptureValue>);
static_assert(std::is_nothrow_destructible_v<CaptureValue>);
static_assert(std::is_nothrow_move_constructible_v<ReferenceValue>);
static_assert(std::is_nothrow_destructible_v<ReferenceValue>);
static_assert(std::is_nothrow_move_constructible_v<csr::CsrTimingResultScatterAttempt>);
static_assert(std::is_nothrow_destructible_v<csr::CsrTimingResultScatterAttempt>);
static_assert(noexcept(StaCsrCaptureAttempt{std::declval<CaptureValue>()}));
static_assert(noexcept(StaCsrReferenceAttempt{std::declval<ReferenceValue>()}));
static_assert(noexcept(StaCsrScatterAttempt{std::declval<csr::CsrTimingResultScatterAttempt>()}));
static_assert(std::is_nothrow_move_constructible_v<StaCsrCaptureAttempt>);
static_assert(std::is_nothrow_destructible_v<StaCsrCaptureAttempt>);
static_assert(std::is_nothrow_move_constructible_v<StaCsrReferenceAttempt>);
static_assert(std::is_nothrow_destructible_v<StaCsrReferenceAttempt>);
static_assert(std::is_nothrow_move_constructible_v<StaCsrScatterAttempt>);
static_assert(std::is_nothrow_destructible_v<StaCsrScatterAttempt>);
static_assert(std::is_nothrow_move_assignable_v<std::unique_ptr<csr::CsrSnapshotObservation>>);
static_assert(std::is_nothrow_destructible_v<std::unique_ptr<csr::CsrSnapshotObservation>>);

[[nodiscard]] StaCsrSessionOperationFailure invalidStateFailure() noexcept
{
  return {};
}

[[nodiscard]] StaCsrCaptureAttempt invalidCaptureAttempt() noexcept
{
  return StaCsrCaptureAttempt{CaptureValue{invalidStateFailure()}};
}

[[nodiscard]] StaCsrReferenceAttempt invalidReferenceAttempt() noexcept
{
  return StaCsrReferenceAttempt{ReferenceValue{invalidStateFailure()}};
}

[[nodiscard]] StaCsrScatterAttempt invalidScatterAttempt() noexcept
{
  return StaCsrScatterAttempt{csr::CsrTimingResultScatterAttempt{csr::CsrScatterResult{csr::CsrScatterCode::kInvalidState, 0, std::nullopt}, {}, std::nullopt}};
}

[[nodiscard]] std::string contextViolationMessage(const std::vector<csr::CsrContractViolation>& errors)
{
  if (errors.empty())
    return "analysis context validation failed without a violation";
  return errors.front().field + ": " + errors.front().message;
}

}  // namespace

StaCsrSessionContext::StaCsrSessionContext(csr::StaAnalysisContext context, std::uint64_t session_token)
    : _analysis_context(std::move(context)), _session_token(session_token)
{
  if (_session_token == 0)
    throw std::invalid_argument("CSR session token must be nonzero");
  const auto errors = csr::validateAnalysisContextIntrinsic(_analysis_context);
  if (!errors.empty())
    throw std::invalid_argument(contextViolationMessage(errors));
}

StaCsrSessionContext::~StaCsrSessionContext() noexcept = default;

StaCsrCaptureAttempt StaCsrSessionContext::capture(const Database& database)
{
  if (_substate != StaCsrSessionSubstate::kAwaitingCapture)
    return invalidCaptureAttempt();

  _substate = StaCsrSessionSubstate::kCaptureFailed;
  csr::CsrSessionSnapshotBuildResult build_result = csr::buildCsrSessionSnapshot(database, _analysis_context, _session_token);
  if (const auto* code = std::get_if<csr::CsrCaptureCode>(&build_result))
    return StaCsrCaptureAttempt{CaptureValue{csr::CsrCaptureResult{*code}}};

  auto snapshot_candidate = std::move(std::get<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>(build_result));
  if (!snapshot_candidate)
    throw std::logic_error("CSR session snapshot builder returned a null candidate");

  StaCsrCaptureAttempt success_attempt{CaptureValue{csr::CsrCaptureResult{std::nullopt}}};
  _substate = StaCsrSessionSubstate::kReady;
  _snapshot.swap(snapshot_candidate);
  return success_attempt;
}

StaCsrReferenceAttempt StaCsrSessionContext::computeReference()
{
  if (_substate != StaCsrSessionSubstate::kReady || !_snapshot)
    return invalidReferenceAttempt();

  csr::CsrComputationResult result = csr::computeCsrSessionReference(*_snapshot);
  StaCsrReferenceAttempt attempt{ReferenceValue{std::move(result)}};
  return attempt;
}

StaCsrScatterAttempt StaCsrSessionContext::scatter(Database& database, const csr::CsrTimingResult& result)
{
  if (_substate != StaCsrSessionSubstate::kReady || !_snapshot)
    return invalidScatterAttempt();

  csr::CsrTimingResultScatterAttempt result_attempt = csr::scatterCsrSessionTimingResult(database, *_snapshot, result, _analysis_context, _session_token);
  StaCsrScatterAttempt attempt{std::move(result_attempt)};
  if (attempt.value.scatter_result.code == csr::CsrScatterCode::kSnapshotInvalidated)
    _substate = StaCsrSessionSubstate::kInvalidated;
  return attempt;
}

void StaCsrSessionContext::release(std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) noexcept
{
  if (detached_out == nullptr || *detached_out || _substate == StaCsrSessionSubstate::kReleased)
    return;

  if (_substate == StaCsrSessionSubstate::kReady || _substate == StaCsrSessionSubstate::kInvalidated) {
    if (_snapshot) {
      csr::releaseCsrSessionSnapshot(*_snapshot);
      *detached_out = std::move(_snapshot->active_observation);
    }
  }
  _substate = StaCsrSessionSubstate::kReleased;
}

bool StaCsrSessionContext::copyObservation(std::unique_ptr<csr::CsrSnapshotObservation>* detached_out)
{
  if (detached_out == nullptr || *detached_out)
    return false;
  if (_substate == StaCsrSessionSubstate::kAwaitingCapture || _substate == StaCsrSessionSubstate::kCaptureFailed)
    return true;
  if (_substate == StaCsrSessionSubstate::kReleased || !_snapshot || !_snapshot->active_observation)
    return false;

  auto candidate = std::make_unique<csr::CsrSnapshotObservation>(*_snapshot->active_observation);
  detached_out->swap(candidate);
  return true;
}

}  // namespace ista
