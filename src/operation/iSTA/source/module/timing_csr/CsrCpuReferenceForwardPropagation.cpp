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

[[nodiscard]] CsrCpuReferenceStageStatus dynamicPass(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work)
{
  struct Candidate
  {
    std::size_t lane;
    double value;
  };
  // Reuse only one node's candidates. Their IDs are seed first, then incoming
  // row ordinal and input transition; physical ArcIds are not tie identities.
  std::vector<Candidate> candidates;
  for (NodeId sink : work.result.level_nodes) {
    if (sink.value() >= work.node_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    candidates.clear();
    std::array<std::optional<double>, 4> extremes;
    const auto add = [&](AnalysisMode analysis, Transition output, double value) {
      const std::size_t lane = csrAnalysisIndex(analysis) * 2 + csrTransitionIndex(output);
      candidates.push_back({lane, value});
      if (!extremes[lane] || (analysis == AnalysisMode::kMax ? value > *extremes[lane] : value < *extremes[lane]))
        extremes[lane] = value;
    };
    for (AnalysisMode analysis : kAnalyses)
      for (Transition output : kTransitions) {
        const auto lane = csrNodeLane(sink.value(), analysis, output);
        if (work.result.data_slews.presence[lane])
          add(analysis, output, work.result.data_slews.values[lane]);
      }

    CsrCpuReferenceStageStatus status = CsrCpuReferenceStageStatus::kSuccess;
    const auto visit = edges.forEachIncoming(sink, [&](const EdgeEntry& entry) {
      recordStorageVisit(work.telemetry.forward_propagation, entry.storage_kind);
      const auto payload = edges.arc(entry.arc_id);
      if (!payload || payload.value().sink != sink || payload.value().source.value() >= work.node_count)
        return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
      const auto& arc = payload.value();
      if (!arc.eligibility.data_slew)
        return EdgeVisitResult::continueVisit();
      for (AnalysisMode analysis : kAnalyses)
        for (Transition input : kTransitions) {
          const auto source_lane = csrNodeLane(arc.source.value(), analysis, input);
          if (!work.result.data_slews.presence[source_lane])
            continue;
          for (Transition output : kTransitions) {
            CsrDynamicArcValue value;
            status = evaluateCsrDynamicArc(edges, entry.arc_id, analysis, input, output,
                                           work.result.data_slews.values[source_lane], work, value);
            if (status != CsrCpuReferenceStageStatus::kSuccess)
              return EdgeVisitResult::fail(CsrViewError::kCallbackError);
            if (value.present)
              add(analysis, output, value.slew);
          }
        }
      return EdgeVisitResult::continueVisit();
    });
    if (visit.control == EdgeVisitControl::kError)
      return status == CsrCpuReferenceStageStatus::kSuccess ? CsrCpuReferenceStageStatus::kInvariantViolation : status;

    std::array<bool, 4> selected{};
    for (const auto& candidate : candidates) {
      if (selected[candidate.lane])
        continue;
      bool outside = false;
      status = epsilonBetter(candidate.lane < 2 ? AnalysisMode::kMax : AnalysisMode::kMin,
                             *extremes[candidate.lane], candidate.value, kPathWinnerEpsilon, outside);
      if (status != CsrCpuReferenceStageStatus::kSuccess)
        return status;
      if (!outside) {
        const auto lane = sink.value() * 4 + candidate.lane;
        work.result.data_slews.presence[lane] = 1;
        work.result.data_slews.values[lane] = candidate.value;
        selected[candidate.lane] = true;
      }
    }
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

[[nodiscard]] CsrCpuReferenceStageStatus arrivalAndPathPass(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work)
{
  struct ScalarCandidate { NodeId source; ArcId arc; double arrival; };
  struct PathCandidate {
    const CsrCpuReferencePathState* source;
    NodeId predecessor;
    ArcId arc;
    Transition input;
    double arrival;
    double delay;
  };
  struct Extreme { ClockTagId tag; double arrival; bool selected; };
  std::vector<ScalarCandidate> scalars;
  std::vector<PathCandidate> candidates;
  std::vector<Extreme> extremes;
  std::vector<CsrCpuReferencePathState> selected;
  std::vector<EdgeEntry> incoming;

  for (NodeId sink : work.result.level_nodes) {
    if (sink.value() >= work.node_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    incoming.clear();
    const auto visit = edges.forEachIncoming(sink, [&](const EdgeEntry& entry) {
      recordStorageVisit(work.telemetry.forward_propagation, entry.storage_kind);
      const auto arc = edges.arc(entry.arc_id);
      if (!arc || entry.arc_id.value() >= work.arc_count || arc.value().sink != sink
          || arc.value().source.value() >= work.node_count)
        return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
      if (arc.value().eligibility.data_arrival)
        incoming.push_back(entry);
      return EdgeVisitResult::continueVisit();
    });
    if (visit.control == EdgeVisitControl::kError)
      return CsrCpuReferenceStageStatus::kInvariantViolation;

    double extreme = work.result.nodes.arrivals.values[sink.value()];
    if (!std::isfinite(extreme) && !isNegativeInfinity(extreme))
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const double seed = extreme;
    scalars.clear();
    for (const auto& entry : incoming) {
      const auto& arc = edges.arc(entry.arc_id).value();
      const auto arrival = work.result.nodes.arrivals.values[arc.source.value()];
      if (!std::isfinite(arrival)) {
        if (isNegativeInfinity(arrival))
          continue;
        return CsrCpuReferenceStageStatus::kInvariantViolation;
      }
      if (!work.result.arcs.delays.presence[entry.arc_id.value()])
        return CsrCpuReferenceStageStatus::kInvariantViolation;
      const auto candidate = arrival + work.result.arcs.delays.values[entry.arc_id.value()];
      if (!std::isfinite(candidate))
        return CsrCpuReferenceStageStatus::kNumericFailure;
      scalars.push_back({arc.source, entry.arc_id, candidate});
      extreme = std::max(extreme, candidate);
    }
    bool outside = isNegativeInfinity(seed);
    if (!outside) {
      const auto status = epsilonBetter(AnalysisMode::kMax, extreme, seed, kScalarWinnerEpsilon, outside);
      if (status != CsrCpuReferenceStageStatus::kSuccess)
        return status;
    }
    if (outside) {
      for (const auto& candidate : scalars) {
        const auto status = epsilonBetter(AnalysisMode::kMax, extreme, candidate.arrival, kScalarWinnerEpsilon, outside);
        if (status != CsrCpuReferenceStageStatus::kSuccess)
          return status;
        if (outside)
          continue;
        const auto node = sink.value(), source = candidate.source.value();
        work.result.nodes.arrivals.values[node] = candidate.arrival;
        work.result.nodes.launches.values[node] = work.result.nodes.launches.values[source];
        work.result.nodes.predecessor_nodes.presence[node] = 1;
        work.result.nodes.predecessor_nodes.values[node] = candidate.source;
        work.result.nodes.predecessor_arcs.presence[node] = 1;
        work.result.nodes.predecessor_arcs.values[node] = candidate.arc;
        work.result.nodes.clock_names.presence[node] = work.result.nodes.clock_names.presence[source];
        work.result.nodes.clock_names.values[node] = work.result.nodes.clock_names.presence[source]
                                                       ? work.result.nodes.clock_names.values[source] : std::string{};
        break;
      }
    }

    for (AnalysisMode analysis : kAnalyses)
      for (PathSource source_kind : kPathSources)
        for (Transition output : kTransitions) {
          auto& row = work.path_rows[csrPathRow(sink.value(), analysis, source_kind, output)];
          candidates.clear();
          extremes.clear();
          selected.clear();
          const auto add = [&](PathCandidate candidate) {
            candidates.push_back(candidate);
            const auto tag = candidate.source->clock_tag;
            const auto found = std::lower_bound(extremes.begin(), extremes.end(), tag,
                                                [](const Extreme& value, ClockTagId key) { return value.tag.value() < key.value(); });
            if (found == extremes.end() || found->tag != tag)
              extremes.insert(found, {tag, candidate.arrival, false});
            else if (analysis == AnalysisMode::kMax ? candidate.arrival > found->arrival : candidate.arrival < found->arrival)
              found->arrival = candidate.arrival;
          };
          for (const auto& path : row) {
            if (!std::isfinite(path.arrival)) {
              if (isNegativeInfinity(path.arrival))
                continue;
              return CsrCpuReferenceStageStatus::kInvariantViolation;
            }
            add({&path, sink, {}, output, path.arrival, 0.0});
          }
          for (const auto& entry : incoming) {
            const auto& arc = edges.arc(entry.arc_id).value();
            for (Transition input : kTransitions) {
              if (!work.result.arcs.input_output_delays.presence[csrArcIoLane(entry.arc_id.value(), analysis, input, output)])
                continue;
              double delay = 0.0;
              auto status = selectPathDelay(work, entry.arc_id.value(), analysis, input, output, delay);
              if (status != CsrCpuReferenceStageStatus::kSuccess)
                return status;
              for (const auto& path : work.path_rows[csrPathRow(arc.source.value(), analysis, source_kind, input)]) {
                if (!std::isfinite(path.arrival)) {
                  if (isNegativeInfinity(path.arrival))
                    continue;
                  return CsrCpuReferenceStageStatus::kInvariantViolation;
                }
                double arrival = 0.0;
                status = roundedPathArrival(path.arrival, delay, arrival);
                if (status != CsrCpuReferenceStageStatus::kSuccess)
                  return status;
                add({&path, arc.source, entry.arc_id, input, arrival, delay});
              }
            }
          }
          selected.resize(extremes.size());
          for (const auto& candidate : candidates) {
            const auto found = std::lower_bound(extremes.begin(), extremes.end(), candidate.source->clock_tag,
                                                [](const Extreme& value, ClockTagId key) { return value.tag.value() < key.value(); });
            if (found->selected)
              continue;
            const auto status = epsilonBetter(analysis, found->arrival, candidate.arrival, kPathWinnerEpsilon, outside);
            if (status != CsrCpuReferenceStageStatus::kSuccess)
              return status;
            if (outside)
              continue;
            auto& value = selected[static_cast<std::size_t>(found - extremes.begin())];
            value = *candidate.source;
            if (candidate.predecessor != sink) {
              value.arrival = candidate.arrival;
              const auto lane = csrNodeLane(sink.value(), analysis, output);
              value.slew = work.result.data_slews.presence[lane] ? work.result.data_slews.values[lane] : 0.0;
              value.predecessor_node = candidate.predecessor;
              value.predecessor_arc = candidate.arc;
              value.predecessor_arc_delay = candidate.delay;
              value.predecessor_transition = candidate.input;
            }
            found->selected = true;
          }
          row.swap(selected);
        }
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
