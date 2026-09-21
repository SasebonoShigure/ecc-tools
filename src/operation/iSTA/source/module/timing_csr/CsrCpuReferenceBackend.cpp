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
#include "CsrCpuReferenceBackend.hpp"

#include <cassert>
#include <chrono>
#include <utility>

#include "CsrCpuReferenceBackendInternal.hpp"
#include "CsrFifoLevelizationValidation.hpp"
#include "CsrReferenceValidator.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::csr {
namespace {

[[nodiscard]] CsrComputationResult stageFailure(CsrCpuReferenceStageStatus status)
{
  assert(status != CsrCpuReferenceStageStatus::kSuccess);
  if (status == CsrCpuReferenceStageStatus::kNumericFailure)
    return CsrComputationResult{CsrComputationCode::kNumericFailure};
  return CsrComputationResult{CsrComputationCode::kInvariantViolation};
}

using SteadyClock = std::chrono::steady_clock;

double elapsedMilliseconds(SteadyClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(
             SteadyClock::now() - start)
      .count();
}

}  // namespace

CsrComputationWithTelemetry detail::propagateCsrCpuReferenceWithLevels(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                                       CsrFifoLevelization fifo, CsrComputationWithTelemetry output)
{
  if (edges.nodeCount() != payloads.nodeCount() || fifo.view_error || fifo.level_nodes.size() != edges.nodeCount()
      || fifo.node_to_level.size() != edges.nodeCount()) {
    output.computation = CsrComputationResult{CsrComputationCode::kInvariantViolation};
    return output;
  }
  if (!payloads.unsupportedReasons().empty()) {
    output.computation = CsrComputationResult{CsrReferenceSupportResult{payloads.unsupportedReasons()}};
    return output;
  }
  if (!fifo.complete) {
    output.computation = CsrComputationResult{CsrComputationCode::kCycle};
    return output;
  }
  const auto propagation_started = SteadyClock::now();
  CsrCpuReferenceWorkState work;
  work.telemetry = output.telemetry;
  work.result.schema_version = payloads.schemaVersion();
  work.result.source_identity = payloads.sourceIdentity();

  CsrCpuReferenceStageStatus status = initializeCsrCpuReferenceResult(payloads, std::move(fifo), work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    output.computation = stageFailure(status);

  if (status != CsrCpuReferenceStageStatus::kSuccess) {
    output.phase_timings.propagation_ms = elapsedMilliseconds(propagation_started);
    return output;
  }
  status = initializeCsrCpuReferenceArcs(edges, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    output.computation = stageFailure(status);

  if (status != CsrCpuReferenceStageStatus::kSuccess) {
    output.phase_timings.propagation_ms = elapsedMilliseconds(propagation_started);
    output.telemetry = work.telemetry;
    return output;
  }
  status = propagateCsrCpuReferenceClocks(edges, payloads, work);
  if (status == CsrCpuReferenceStageStatus::kSuccess)
    status = initializeCsrCpuSequentialSeeds(edges, payloads, work);
  if (status == CsrCpuReferenceStageStatus::kSuccess)
    status = propagateCsrCpuReferenceForward(edges, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    output.computation = stageFailure(status);

  if (status != CsrCpuReferenceStageStatus::kSuccess) {
    output.phase_timings.propagation_ms = elapsedMilliseconds(propagation_started);
    output.telemetry = work.telemetry;
    return output;
  }
  status = propagateCsrCpuReferenceRequired(edges, payloads, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    output.computation = stageFailure(status);
  else
    output.computation = CsrComputationResult{std::move(work.result)};

  output.phase_timings.propagation_ms = elapsedMilliseconds(propagation_started);
  output.telemetry = work.telemetry;
  return output;
}

namespace {

[[nodiscard]] bool isIncomplete(const CsrFifoLevelizationValidationResult& validation) noexcept
{
  return validation.code == CsrFifoLevelizationValidationCode::kIncomplete;
}

}  // namespace

CsrComputationWithTelemetry computeCsrCpuReferenceWithReplay(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads, ActiveCsrFifoReplayFunction replay)
{
  assert(replay != nullptr);

  CsrComputationWithTelemetry output;
  if (edges.nodeCount() != payloads.nodeCount()) {
    output.computation = CsrComputationResult{CsrComputationCode::kInvariantViolation};
    return output;
  }

  if (!payloads.unsupportedReasons().empty()) {
    output.computation = CsrComputationResult{CsrReferenceSupportResult{payloads.unsupportedReasons()}};
    return output;
  }

  const auto levelization_started = SteadyClock::now();
  CsrFifoLevelization fifo = replay(edges, payloads, &output.telemetry);
  output.phase_timings.levelization_ms
      = elapsedMilliseconds(levelization_started);
  const auto validation = validateCsrFifoLevelization(edges, fifo);
  if (!validation.valid()) {
    output.computation = CsrComputationResult{isIncomplete(validation) ? CsrComputationCode::kCycle : CsrComputationCode::kInvariantViolation};
    return output;
  }
  return detail::propagateCsrCpuReferenceWithLevels(edges, payloads, std::move(fifo), std::move(output));
}

CsrComputationWithTelemetry detail::computeCsrCpuReferenceForStage(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads)
{
  CsrComputationWithTelemetry output;
  if (edges.nodeCount() != payloads.nodeCount()) {
    output.computation = CsrComputationResult{CsrComputationCode::kInvariantViolation};
    return output;
  }
  if (!payloads.unsupportedReasons().empty()) {
    output.computation = CsrComputationResult{CsrReferenceSupportResult{payloads.unsupportedReasons()}};
    return output;
  }
  const auto started = SteadyClock::now();
  auto fifo = replayFifoLevelization(edges, payloads, &output.telemetry);
  output.phase_timings.levelization_ms = elapsedMilliseconds(started);
  if (fifo.view_error || !fifo.complete) {
    output.computation = CsrComputationResult{fifo.view_error ? CsrComputationCode::kInvariantViolation : CsrComputationCode::kCycle};
    return output;
  }
  if constexpr (incregpusta_debug_checks_enabled) {
    if (!validateCsrFifoLevelization(edges, fifo).valid()) {
      output.computation = CsrComputationResult{CsrComputationCode::kInvariantViolation};
      return output;
    }
  }
  return propagateCsrCpuReferenceWithLevels(edges, payloads, std::move(fifo), std::move(output));
}

CsrComputationResult CsrCpuReferenceBackend::compute(const CsrReferenceInput& input) const
{
  return computeWithTelemetry(input).computation;
}

CsrComputationResult CsrCpuReferenceBackend::compute(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads) const
{
  return computeWithTelemetry(edges, payloads).computation;
}

CsrComputationResult CsrCpuReferenceBackend::compute(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                     const CsrFifoLevelization& levels) const
{
  return computeWithTelemetry(edges, payloads, levels).computation;
}

CsrComputationWithTelemetry CsrCpuReferenceBackend::computeWithTelemetry(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads) const
{
  return computeCsrCpuReferenceWithReplay(edges, payloads, static_cast<ActiveCsrFifoReplayFunction>(&replayFifoLevelization));
}

CsrComputationWithTelemetry CsrCpuReferenceBackend::computeWithTelemetry(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                                         const CsrFifoLevelization& levels) const
{
  CsrComputationWithTelemetry output;
  if (edges.nodeCount() != payloads.nodeCount()) {
    output.computation = CsrComputationResult{CsrComputationCode::kInvariantViolation};
    return output;
  }
  if (!payloads.unsupportedReasons().empty()) {
    output.computation = CsrComputationResult{CsrReferenceSupportResult{payloads.unsupportedReasons()}};
    return output;
  }
  if (!validateCsrFifoLevelization(edges, levels).valid()) {
    output.computation = CsrComputationResult{CsrComputationCode::kInvariantViolation};
    return output;
  }
  return detail::propagateCsrCpuReferenceWithLevels(edges, payloads, levels, std::move(output));
}

CsrComputationResult computeCsrCpuReferenceWithReplay(const CsrReferenceInput& input, CsrFifoReplayFunction replay)
{
  assert(replay != nullptr);
  if (!validateReferenceInput(input).empty())
    return CsrComputationResult{CsrComputationCode::kInvariantViolation};
  if (!input.unsupported_reasons.empty())
    return CsrComputationResult{CsrReferenceSupportResult{input.unsupported_reasons}};
  CsrFifoLevelization fifo = replay(input);
  FlatCsrEdgeView edges(input);
  FlatTimingPayloadView payloads(input);
  const auto validation = validateCsrFifoLevelization(edges, fifo);
  if (!validation.valid())
    return CsrComputationResult{isIncomplete(validation) ? CsrComputationCode::kCycle : CsrComputationCode::kInvariantViolation};
  return detail::propagateCsrCpuReferenceWithLevels(edges, payloads, std::move(fifo)).computation;
}


CsrComputationWithTelemetry CsrCpuReferenceBackend::computeWithTelemetry(const CsrReferenceInput& input) const
{
  CsrComputationWithTelemetry output;
  if (!validateReferenceInput(input).empty()) {
    output.computation = CsrComputationResult{CsrComputationCode::kInvariantViolation};
    return output;
  }
  FlatCsrEdgeView edges(input);
  FlatTimingPayloadView payloads(input);
  return computeWithTelemetry(edges, payloads);
}

}  // namespace ista::csr
