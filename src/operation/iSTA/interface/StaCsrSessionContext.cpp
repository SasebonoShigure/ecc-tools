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
#include "PhysicalStaSessionState.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "CsrReferenceValidator.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#if ISTA_ENABLE_CUDA
#include "StaCudaSessionState.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "StaProductionLifecycle.hpp"
#endif

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

csr::detail::NativeLibraryModels& StaCsrSessionContext::libraryModels(Database& database)
{
  if (!_library_models)
    _library_models = std::make_unique<csr::detail::NativeLibraryModels>(database);
  _library_models->requireDatabase(database);
  return *_library_models;
}

#if ISTA_ENABLE_CUDA
StaCudaSessionState& StaCsrSessionContext::cudaState(const timing_edit::NativeExecutionAuthority& authority)
{
  if (!authority.active() || authority.sessionToken().value() != _session_token
      || !_cuda || _cuda->_poisoned || _cuda->token() != _session_token || _substate == StaCsrSessionSubstate::kReleased)
    throw std::logic_error("CUDA result access requires the active session operation guard");
  return *_cuda;
}
void StaCsrSessionContext::materializeCudaTiming(Database& database, const timing_edit::NativeExecutionAuthority& authority)
{
  if (!authority.readyForQuery() || &database != &STADM.getDatabase())
    throw std::logic_error("CUDA query requires a committed canonical Database");
  try {
    cudaState(authority).materialize(database, authority);
  } catch (...) {
    if (_cuda && (_cuda->_poisoned || _cuda->device.poisoned())) authority.poison();
    throw;
  }
}
const std::pair<std::string, std::string>& StaCsrSessionContext::readCudaReport(Database& database,
    const timing_edit::NativeExecutionAuthority& authority)
{
  if (!authority.readyForQuery() || &database != &STADM.getDatabase())
    throw std::logic_error("CUDA report requires the committed session owner");
  try {
    return cudaState(authority).readReport(database, authority);
  } catch (...) {
    if (_cuda && (_cuda->_poisoned || _cuda->device.poisoned())) authority.poison();
    throw;
  }
}
void StaCsrSessionContext::prepareCudaReport(Database& database, const timing_edit::NativeExecutionAuthority& authority)
{
  materializeCudaTiming(database, authority);
  auto& state = cudaState(authority);
  if (state._report_generation == state.device.generation())
    return;
  const auto result = executeStaLifecyclePipeline(
      {{"CUDA report preparation", [] {}, [&] { state.prepareReport(); }, [] {}}},
      StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
  if (result.cleanup_failed) state._poisoned = true;
  if (state._poisoned || state.device.poisoned()) authority.poison();
  if (!result.succeeded)
    throw std::runtime_error("CUDA report query failed: " + result.message);
}
#endif


StaCsrCaptureAttempt StaCsrSessionContext::capture(const Database& database)
{
  if (_physical || _substate != StaCsrSessionSubstate::kAwaitingCapture)
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
  if (_physical || _substate != StaCsrSessionSubstate::kReady || !_snapshot)
    return invalidReferenceAttempt();

  csr::CsrComputationResult result = csr::computeCsrSessionReference(*_snapshot);
  StaCsrReferenceAttempt attempt{ReferenceValue{std::move(result)}};
  return attempt;
}

StaCsrScatterAttempt StaCsrSessionContext::scatter(Database& database, const csr::CsrTimingResult& result)
{
  if (_physical || _substate != StaCsrSessionSubstate::kReady || !_snapshot)
    return invalidScatterAttempt();

  csr::CsrTimingResultScatterAttempt result_attempt = csr::scatterCsrSessionTimingResult(database, *_snapshot, result, _analysis_context, _session_token);
  StaCsrScatterAttempt attempt{std::move(result_attempt)};
  if (attempt.value.scatter_result.code == csr::CsrScatterCode::kSnapshotInvalidated)
    _substate = StaCsrSessionSubstate::kInvalidated;
  return attempt;
}

bool StaCsrSessionContext::release(std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) noexcept
{
  if (detached_out == nullptr || *detached_out)
    return false;
  if (_substate == StaCsrSessionSubstate::kReleased)
    return true;
  bool clean = true;
  _physical.reset();

  if (_substate == StaCsrSessionSubstate::kReady || _substate == StaCsrSessionSubstate::kInvalidated) {
    if (_snapshot) {
      csr::releaseCsrSessionSnapshot(*_snapshot);
      if constexpr (incregpusta_debug_checks_enabled)
        *detached_out = std::move(_snapshot->active_observation);
    }
  }
#if ISTA_ENABLE_CUDA
  if (_cuda)
    clean = _cuda->device.close();
  _cuda.reset();
#endif
  _substate = StaCsrSessionSubstate::kReleased;
  return clean;
}

bool StaCsrSessionContext::copyObservation(std::unique_ptr<csr::CsrSnapshotObservation>* detached_out)
{
  if (_physical || detached_out == nullptr || *detached_out)
    return false;
  if constexpr (!incregpusta_debug_checks_enabled)
    return true;
  if (_substate == StaCsrSessionSubstate::kAwaitingCapture || _substate == StaCsrSessionSubstate::kCaptureFailed)
    return true;
  if (_substate == StaCsrSessionSubstate::kReleased || !_snapshot || !_snapshot->active_observation)
    return false;

  auto candidate = std::make_unique<csr::CsrSnapshotObservation>(*_snapshot->active_observation);
  detached_out->swap(candidate);
  return true;
}

}  // namespace ista
