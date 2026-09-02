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
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

#include "CsrCpuReferenceBackendInternal.hpp"

namespace ista::csr {
namespace {

constexpr double kScalarWinnerEpsilon = 1.0e-9;
constexpr double kPathWinnerEpsilon = 1.0e-6;
constexpr double kPathRoundScale = 1.0e15;
constexpr std::array<AnalysisMode, kAnalysisCount> kAnalyses{AnalysisMode::kMax, AnalysisMode::kMin};
constexpr std::array<PathSource, kPathSourceCount> kPathSources{PathSource::kInput, PathSource::kRegister};
constexpr std::array<Transition, kTransitionCount> kTransitions{Transition::kRise, Transition::kFall};

[[nodiscard]] CsrCpuReferenceStageStatus epsilonBetter(AnalysisMode analysis, double candidate, double current, double epsilon, bool& better)
{
  const double threshold = analysis == AnalysisMode::kMax ? current + epsilon : current - epsilon;
  if (!std::isfinite(threshold))
    return CsrCpuReferenceStageStatus::kNumericFailure;
  better = analysis == AnalysisMode::kMax ? candidate > threshold : candidate < threshold;
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] bool isNegativeInfinity(double value)
{
  return std::isinf(value) && value < 0.0;
}

[[nodiscard]] CsrCpuReferenceStageStatus updateSinkSlew(CsrCpuReferenceWorkState& work, NodeId sink, AnalysisMode analysis, Transition output_transition,
                                                        double candidate)
{
  if (!std::isfinite(candidate) || candidate < 0.0)
    return CsrCpuReferenceStageStatus::kNumericFailure;
  if (sink.value() >= work.node_count)
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  const std::size_t lane = csrNodeLane(static_cast<std::size_t>(sink.value()), analysis, output_transition);
  auto& slews = work.result.data_slews;
  if (slews.presence[lane] == 0) {
    slews.presence[lane] = 1;
    slews.values[lane] = candidate;
    return CsrCpuReferenceStageStatus::kSuccess;
  }

  bool better = false;
  CsrCpuReferenceStageStatus status = epsilonBetter(analysis, candidate, slews.values[lane], kPathWinnerEpsilon, better);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  if (better)
    slews.values[lane] = candidate;
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus dynamicPass(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work)
{
  for (NodeId source : work.result.timing_order) {
    if (source.value() >= work.node_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const std::size_t source_index = static_cast<std::size_t>(source.value());
    std::optional<CsrCpuReferenceStageStatus> callback_status;
    const EdgeVisitResult visit = edges.forEachOutgoing(source, [&](const EdgeEntry& entry) {
      recordStorageVisit(work.telemetry.forward_propagation, entry.storage_kind);
      const auto payload = edges.arc(entry.arc_id);
      if (!payload) {
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }
      const ArcSemanticPayload& arc = payload.value();
      if (arc.source != source || arc.sink.value() >= work.node_count)
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
      if (!arc.eligibility.data_slew)
        return callback_status ? EdgeVisitResult::fail(CsrViewError::kCallbackError) : EdgeVisitResult::continueVisit();

      for (AnalysisMode analysis : kAnalyses) {
        for (Transition input_transition : kTransitions) {
          const std::size_t source_lane = csrNodeLane(source_index, analysis, input_transition);
          if (work.result.data_slews.presence[source_lane] == 0)
            continue;
          const double source_slew = work.result.data_slews.values[source_lane];
          for (Transition output_transition : kTransitions) {
            CsrDynamicArcValue value;
            CsrCpuReferenceStageStatus status
                = evaluateCsrDynamicArc(edges, entry.arc_id, analysis, input_transition, output_transition, source_slew, work, value);
            if (status != CsrCpuReferenceStageStatus::kSuccess) {
              callback_status = status;
              return EdgeVisitResult::fail(CsrViewError::kCallbackError);
            }
            if (!value.present)
              continue;
            status = updateSinkSlew(work, arc.sink, analysis, output_transition, value.slew);
            if (status != CsrCpuReferenceStageStatus::kSuccess) {
              callback_status = status;
              return EdgeVisitResult::fail(CsrViewError::kCallbackError);
            }
          }
        }
      }
      return EdgeVisitResult::continueVisit();
    });
    if (visit.control == EdgeVisitControl::kError)
      return callback_status.value_or(CsrCpuReferenceStageStatus::kInvariantViolation);
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus propagateScalar(NodeId source, ArcId arc_id, const ArcSemanticPayload& arc, CsrCpuReferenceWorkState& work)
{
  const std::size_t source_index = static_cast<std::size_t>(source.value());
  const std::size_t sink_index = static_cast<std::size_t>(arc.sink.value());
  const double source_arrival = work.result.nodes.arrivals.values[source_index];
  if (!std::isfinite(source_arrival))
    return isNegativeInfinity(source_arrival) ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kInvariantViolation;

  if (arc_id.value() >= work.arc_count || work.result.arcs.delays.presence[arc_id.value()] == 0)
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  const double delay = work.result.arcs.delays.values[arc_id.value()];
  const double candidate = source_arrival + delay;
  if (!std::isfinite(delay) || !std::isfinite(candidate))
    return CsrCpuReferenceStageStatus::kNumericFailure;

  const double sink_arrival = work.result.nodes.arrivals.values[sink_index];
  bool replace = isNegativeInfinity(sink_arrival);
  if (!replace) {
    if (!std::isfinite(sink_arrival))
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const double threshold = sink_arrival + kScalarWinnerEpsilon;
    if (!std::isfinite(threshold))
      return CsrCpuReferenceStageStatus::kNumericFailure;
    replace = candidate > threshold;
  }
  if (!replace)
    return CsrCpuReferenceStageStatus::kSuccess;

  work.result.nodes.arrivals.values[sink_index] = candidate;
  work.result.nodes.launches.values[sink_index] = work.result.nodes.launches.values[source_index];
  work.result.nodes.predecessor_nodes.presence[sink_index] = 1;
  work.result.nodes.predecessor_nodes.values[sink_index] = source;
  work.result.nodes.predecessor_arcs.presence[sink_index] = 1;
  work.result.nodes.predecessor_arcs.values[sink_index] = arc_id;
  if (work.result.nodes.clock_names.presence[source_index] != 0) {
    work.result.nodes.clock_names.presence[sink_index] = 1;
    work.result.nodes.clock_names.values[sink_index] = work.result.nodes.clock_names.values[source_index];
  } else {
    work.result.nodes.clock_names.presence[sink_index] = 0;
    work.result.nodes.clock_names.values[sink_index].clear();
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus selectPathDelay(const CsrCpuReferenceWorkState& work, std::size_t arc, AnalysisMode analysis,
                                                         Transition input_transition, Transition output_transition, double& delay)
{
  const std::size_t io_lane = csrArcIoLane(arc, analysis, input_transition, output_transition);
  const auto& result = work.result.arcs;
  if (result.graph_delays.presence[io_lane] != 0) {
    delay = result.graph_delays.values[io_lane];
  } else if (result.input_output_delays.presence[io_lane] != 0) {
    delay = result.input_output_delays.values[io_lane];
  } else {
    const std::size_t transition_lane = csrArcTransitionLane(arc, analysis, input_transition);
    if (result.transition_delays.presence[transition_lane] != 0) {
      delay = result.transition_delays.values[transition_lane];
    } else {
      const auto& scalar = analysis == AnalysisMode::kMax ? result.max_delays : result.min_delays;
      if (scalar.presence[arc] == 0)
        return CsrCpuReferenceStageStatus::kInvariantViolation;
      delay = scalar.values[arc];
    }
  }
  return std::isfinite(delay) ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kNumericFailure;
}

[[nodiscard]] CsrCpuReferenceStageStatus roundedPathArrival(double source_arrival, double delay, double& candidate)
{
  if (!std::isfinite(source_arrival))
    return isNegativeInfinity(source_arrival) ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kInvariantViolation;
  const double sum = source_arrival + delay;
  if (!std::isfinite(sum))
    return CsrCpuReferenceStageStatus::kNumericFailure;
  const double scaled = sum * kPathRoundScale;
  if (!std::isfinite(scaled))
    return CsrCpuReferenceStageStatus::kNumericFailure;
  const double rounded = std::round(scaled);
  if (!std::isfinite(rounded))
    return CsrCpuReferenceStageStatus::kNumericFailure;
  candidate = rounded / kPathRoundScale;
  return std::isfinite(candidate) ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kNumericFailure;
}

[[nodiscard]] CsrCpuReferenceStageStatus propagatePathLane(NodeId source, ArcId arc_id, const ArcSemanticPayload& arc, AnalysisMode analysis, PathSource path_source,
                                                           Transition input_transition, Transition output_transition, CsrCpuReferenceWorkState& work)
{
  const std::size_t arc_index = static_cast<std::size_t>(arc_id.value());
  const std::size_t io_lane = csrArcIoLane(arc_index, analysis, input_transition, output_transition);
  if (work.result.arcs.input_output_delays.presence[io_lane] == 0)
    return CsrCpuReferenceStageStatus::kSuccess;

  double selected_delay = 0.0;
  CsrCpuReferenceStageStatus status = selectPathDelay(work, arc_index, analysis, input_transition, output_transition, selected_delay);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;

  const std::size_t source_row = csrPathRow(static_cast<std::size_t>(source.value()), analysis, path_source, input_transition);
  const std::size_t sink_row = csrPathRow(static_cast<std::size_t>(arc.sink.value()), analysis, path_source, output_transition);
  const auto& source_paths = work.path_rows[source_row];
  auto& sink_paths = work.path_rows[sink_row];
  for (const auto& source_path : source_paths) {
    if (!std::isfinite(source_path.arrival)) {
      if (isNegativeInfinity(source_path.arrival))
        continue;
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    }

    double candidate_arrival = 0.0;
    status = roundedPathArrival(source_path.arrival, selected_delay, candidate_arrival);
    if (status != CsrCpuReferenceStageStatus::kSuccess)
      return status;

    const auto found = std::lower_bound(sink_paths.begin(), sink_paths.end(), source_path.clock_tag,
                                        [](const CsrCpuReferencePathState& path, ClockTagId tag) { return path.clock_tag.value() < tag.value(); });
    bool replace = found == sink_paths.end() || found->clock_tag != source_path.clock_tag;
    if (!replace) {
      status = epsilonBetter(analysis, candidate_arrival, found->arrival, kPathWinnerEpsilon, replace);
      if (status != CsrCpuReferenceStageStatus::kSuccess)
        return status;
    }
    if (!replace)
      continue;

    CsrCpuReferencePathState candidate = source_path;
    candidate.arrival = candidate_arrival;
    const std::size_t slew_lane = csrNodeLane(static_cast<std::size_t>(arc.sink.value()), analysis, output_transition);
    candidate.slew = work.result.data_slews.presence[slew_lane] == 0 ? 0.0 : work.result.data_slews.values[slew_lane];
    candidate.predecessor_node = source;
    candidate.predecessor_arc = arc_id;
    candidate.predecessor_arc_delay = selected_delay;
    candidate.predecessor_transition = input_transition;
    if (found == sink_paths.end() || found->clock_tag != source_path.clock_tag)
      sink_paths.insert(found, std::move(candidate));
    else
      *found = std::move(candidate);
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus arrivalAndPathPass(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work)
{
  for (NodeId source : work.result.timing_order) {
    std::optional<CsrCpuReferenceStageStatus> callback_status;
    const EdgeVisitResult visit = edges.forEachOutgoing(source, [&](const EdgeEntry& entry) {
      recordStorageVisit(work.telemetry.forward_propagation, entry.storage_kind);
      const auto payload = edges.arc(entry.arc_id);
      if (!payload) {
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }
      const ArcSemanticPayload& arc = payload.value();
      if (arc.source != source || arc.sink.value() >= work.node_count) {
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }
      if (!arc.eligibility.data_arrival)
        return EdgeVisitResult::continueVisit();

      CsrCpuReferenceStageStatus status = propagateScalar(source, entry.arc_id, arc, work);
      if (status != CsrCpuReferenceStageStatus::kSuccess) {
        callback_status = status;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }
      for (AnalysisMode analysis : kAnalyses) {
        for (PathSource path_source : kPathSources) {
          for (Transition input_transition : kTransitions) {
            for (Transition output_transition : kTransitions) {
              status = propagatePathLane(source, entry.arc_id, arc, analysis, path_source, input_transition, output_transition, work);
              if (status != CsrCpuReferenceStageStatus::kSuccess) {
                callback_status = status;
                return EdgeVisitResult::fail(CsrViewError::kCallbackError);
              }
            }
          }
        }
      }
      return EdgeVisitResult::continueVisit();
    });
    if (visit.control == EdgeVisitControl::kError)
      return callback_status.value_or(CsrCpuReferenceStageStatus::kInvariantViolation);
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

}  // namespace

CsrCpuReferenceStageStatus propagateCsrCpuReferenceForward(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work)
{
  CsrCpuReferenceStageStatus status = dynamicPass(edges, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  return arrivalAndPathPass(edges, work);
}

}  // namespace ista::csr
