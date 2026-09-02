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
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "CsrFifoLevelization.hpp"
#include "CsrCpuReferenceBackend.hpp"

namespace ista::csr {

using CsrFifoReplayFunction = CsrFifoLevelization (*)(const CsrReferenceInput&);
using ActiveCsrFifoReplayFunction = CsrFifoLevelization (*)(const ActiveCsrEdgeView&, const TimingPayloadView&, CsrTraversalTelemetry*);

[[nodiscard]] CsrComputationResult computeCsrCpuReferenceWithReplay(const CsrReferenceInput& input, CsrFifoReplayFunction replay);
[[nodiscard]] CsrComputationWithTelemetry computeCsrCpuReferenceWithReplay(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                                          ActiveCsrFifoReplayFunction replay);

enum class CsrCpuReferenceStageStatus : std::uint8_t
{
  kSuccess,
  kInvariantViolation,
  kNumericFailure
};

struct CsrCpuReferencePathState
{
  ClockTagId clock_tag;
  double arrival{-std::numeric_limits<double>::infinity()};
  double slew{0.0};
  double launch{0.0};
  NodeId start_node;
  std::optional<NodeId> predecessor_node;
  std::optional<ArcId> predecessor_arc;
  double predecessor_arc_delay{0.0};
  Transition predecessor_transition{Transition::kNone};
  std::optional<NodeId> crpr_node;
  std::optional<Transition> crpr_transition;
};

struct CsrCpuReferenceWorkState
{
  CsrTimingResult result;
  std::vector<std::vector<CsrCpuReferencePathState>> path_rows;
  std::size_t node_count{0};
  std::size_t node_lane_count{0};
  std::size_t path_row_count{0};
  std::size_t clock_tag_count{0};
  std::size_t arc_count{0};
  std::size_t arc_transition_lane_count{0};
  CsrTraversalTelemetry telemetry;
  std::size_t arc_io_lane_count{0};
  std::size_t timing_arc_count{0};
  std::size_t timing_arc_lane_count{0};
};

struct CsrDynamicArcValue
{
  bool present{false};
  double delay{0.0};
  double slew{0.0};
};

[[nodiscard]] constexpr std::size_t csrAnalysisIndex(AnalysisMode analysis)
{
  return analysis == AnalysisMode::kMax ? 0 : 1;
}

[[nodiscard]] constexpr std::size_t csrTransitionIndex(Transition transition)
{
  return transition == Transition::kRise ? 0 : 1;
}

[[nodiscard]] constexpr std::size_t csrPathSourceIndex(PathSource source)
{
  return source == PathSource::kInput ? 0 : 1;
}

[[nodiscard]] constexpr std::size_t csrNodeLane(std::size_t node, AnalysisMode analysis, Transition transition)
{
  return (node * kAnalysisCount + csrAnalysisIndex(analysis)) * kTransitionCount + csrTransitionIndex(transition);
}

[[nodiscard]] constexpr std::size_t csrPathRow(std::size_t node, AnalysisMode analysis, PathSource source, Transition transition)
{
  return ((node * kAnalysisCount + csrAnalysisIndex(analysis)) * kPathSourceCount + csrPathSourceIndex(source)) * kTransitionCount
         + csrTransitionIndex(transition);
}

[[nodiscard]] constexpr std::size_t csrArcTransitionLane(std::size_t arc, AnalysisMode analysis, Transition input_transition)
{
  return (arc * kAnalysisCount + csrAnalysisIndex(analysis)) * kTransitionCount + csrTransitionIndex(input_transition);
}

[[nodiscard]] constexpr std::size_t csrArcIoLane(std::size_t arc, AnalysisMode analysis, Transition input_transition, Transition output_transition)
{
  return csrArcTransitionLane(arc, analysis, input_transition) * kTransitionCount + csrTransitionIndex(output_transition);
}

[[nodiscard]] constexpr std::size_t csrArcTypeLane(std::size_t arc, Transition input_transition)
{
  return arc * kTransitionCount + csrTransitionIndex(input_transition);
}

[[nodiscard]] constexpr std::size_t csrTimingArcLane(std::size_t child, AnalysisMode analysis, Transition input_transition, Transition output_transition)
{
  return ((child * kAnalysisCount + csrAnalysisIndex(analysis)) * kTransitionCount + csrTransitionIndex(input_transition)) * kTransitionCount
         + csrTransitionIndex(output_transition);
}

[[nodiscard]] CsrCpuReferenceStageStatus initializeCsrCpuReferenceResult(const TimingPayloadView& payloads, const CsrFifoLevelization& replay,
                                                                         CsrCpuReferenceWorkState& work);
[[nodiscard]] CsrCpuReferenceStageStatus initializeCsrCpuReferenceArcs(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work);
[[nodiscard]] CsrCpuReferenceStageStatus evaluateCsrDynamicArc(const ActiveCsrEdgeView& edges, ArcId arc_id, AnalysisMode analysis, Transition input_transition,
                                                               Transition output_transition, double source_slew, CsrCpuReferenceWorkState& work,
                                                               CsrDynamicArcValue& value);
[[nodiscard]] CsrCpuReferenceStageStatus propagateCsrCpuReferenceForward(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work);
[[nodiscard]] CsrCpuReferenceStageStatus propagateCsrCpuReferenceRequired(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                                          CsrCpuReferenceWorkState& work);

}  // namespace ista::csr
