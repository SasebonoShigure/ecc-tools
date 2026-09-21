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
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "CsrCpuReferenceBackendInternal.hpp"

namespace ista::csr {
namespace {

[[nodiscard]] bool isNegativeInfinity(double value)
{
  return std::isinf(value) && value < 0.0;
}

[[nodiscard]] bool isPositiveInfinity(double value)
{
  return std::isinf(value) && value > 0.0;
}

[[nodiscard]] bool isPositiveZero(double value)
{
  return std::bit_cast<std::uint64_t>(value) == std::uint64_t{0};
}

[[nodiscard]] bool canonicalAbsent(double value)
{
  return isPositiveZero(value);
}

[[nodiscard]] bool canonicalAbsent(std::uint8_t value)
{
  return value == 0;
}

[[nodiscard]] bool canonicalAbsent(Transition value)
{
  return value == Transition::kNone;
}

[[nodiscard]] bool canonicalAbsent(NodeId value)
{
  return value.value() == 0;
}

[[nodiscard]] bool canonicalAbsent(ArcId value)
{
  return value.value() == 0;
}

[[nodiscard]] bool canonicalAbsent(ClockTagId value)
{
  return value.value() == 0;
}

[[nodiscard]] bool canonicalAbsent(const std::string& value)
{
  return value.empty();
}

template <typename T>
void resetOptionalSoa(OptionalSoa<T>& soa, std::size_t size, const T& absent_value)
{
  soa.presence.assign(size, 0);
  soa.values.assign(size, absent_value);
}

template <typename T>
void resetRequiredSoa(OptionalSoa<T>& soa, std::size_t size, const T& value)
{
  soa.presence.assign(size, 1);
  soa.values.assign(size, value);
}

template <typename T>
[[nodiscard]] bool validateOptionalSoa(const OptionalSoa<T>& soa, std::size_t size, bool require_present)
{
  if (soa.presence.size() != size || soa.values.size() != size)
    return false;
  for (std::size_t index = 0; index < size; ++index) {
    if (soa.presence[index] > 1 || (require_present && soa.presence[index] == 0) || (soa.presence[index] == 0 && !canonicalAbsent(soa.values[index])))
      return false;
  }
  return true;
}

[[nodiscard]] CsrCpuReferenceStageStatus seedEndpointRequired(const TimingPayloadView& payloads, CsrCpuReferenceWorkState& work)
{
  double default_required = 0.0;
  for (NodeId endpoint : payloads.orderedEndIds()) {
    if (endpoint.value() >= work.node_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const double arrival = work.result.nodes.arrivals.values[endpoint.value()];
    if (std::isfinite(arrival)) {
      if (arrival > default_required)
        default_required = arrival;
    } else if (!isNegativeInfinity(arrival)) {
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    }
  }

  for (NodeId endpoint : payloads.orderedEndIds())
    work.result.nodes.requireds.values[endpoint.value()] = default_required;

  for (const auto& endpoint : payloads.endpointRequired()) {
    if (endpoint.node.value() >= work.node_count
        || std::find(payloads.orderedEndIds().begin(), payloads.orderedEndIds().end(), endpoint.node) == payloads.orderedEndIds().end())
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const auto& output_delay = endpoint.output_delay[csrAnalysisIndex(AnalysisMode::kMax)];
    if (!output_delay)
      continue;
    const double required = endpoint.clock_period - *output_delay;
    if (!std::isfinite(endpoint.clock_period) || !std::isfinite(*output_delay) || !std::isfinite(required))
      return CsrCpuReferenceStageStatus::kNumericFailure;
    work.result.nodes.requireds.values[endpoint.node.value()] = required;
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus reverseRequired(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work)
{
  for (auto node = work.result.level_nodes.rbegin(); node != work.result.level_nodes.rend(); ++node) {
    if (node->value() >= work.node_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const std::size_t source = static_cast<std::size_t>(node->value());
    std::optional<CsrCpuReferenceStageStatus> callback_status;
    const EdgeVisitResult visit = edges.forEachOutgoing(*node, [&](const EdgeEntry& entry) {
      recordStorageVisit(work.telemetry.required_propagation, entry.storage_kind);
      const auto payload = edges.arc(entry.arc_id);
      if (!payload || entry.arc_id.value() >= work.arc_count) {
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }
      const ArcSemanticPayload& arc = payload.value();
      if (arc.source != *node || arc.sink.value() >= work.node_count)
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
      if (!arc.eligibility.data_arrival)
        return callback_status ? EdgeVisitResult::fail(CsrViewError::kCallbackError) : EdgeVisitResult::continueVisit();

      const double sink_required = work.result.nodes.requireds.values[arc.sink.value()];
      if (!std::isfinite(sink_required)) {
        if (isPositiveInfinity(sink_required))
          return EdgeVisitResult::continueVisit();
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }
      if (work.result.arcs.delays.presence[entry.arc_id.value()] == 0) {
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }
      const double delay = work.result.arcs.delays.values[entry.arc_id.value()];
      const double candidate = sink_required - delay;
      if (!std::isfinite(delay) || !std::isfinite(candidate)) {
        callback_status = CsrCpuReferenceStageStatus::kNumericFailure;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }

      double& source_required = work.result.nodes.requireds.values[source];
      if (isPositiveInfinity(source_required)) {
        source_required = candidate;
      } else if (!std::isfinite(source_required)) {
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      } else if (candidate < source_required) {
        source_required = candidate;
      }
      return EdgeVisitResult::continueVisit();
    });
    if (visit.control == EdgeVisitControl::kError)
      return callback_status.value_or(CsrCpuReferenceStageStatus::kInvariantViolation);
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus finalizeSlacks(CsrCpuReferenceWorkState& work)
{
  for (std::size_t node = 0; node < work.node_count; ++node) {
    const double arrival = work.result.nodes.arrivals.values[node];
    const double required = work.result.nodes.requireds.values[node];
    if (std::isfinite(arrival) && std::isfinite(required)) {
      const double slack = required - arrival;
      if (!std::isfinite(slack))
        return CsrCpuReferenceStageStatus::kNumericFailure;
      work.result.nodes.slacks.values[node] = slack;
    } else {
      if ((!isNegativeInfinity(arrival) && !std::isfinite(arrival)) || (!isPositiveInfinity(required) && !std::isfinite(required)))
        return CsrCpuReferenceStageStatus::kInvariantViolation;
      work.result.nodes.slacks.values[node] = 0.0;
    }
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus flattenPaths(CsrCpuReferenceWorkState& work)
{
  std::size_t entry_count = 0;
  for (const auto& row : work.path_rows) {
    if (row.size() > std::numeric_limits<std::size_t>::max() - entry_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    entry_count += row.size();
  }

  auto& paths = work.result.paths;
  paths.row_offsets.assign(work.path_row_count + 1, 0);
  resetRequiredSoa(paths.clock_tags, entry_count, ClockTagId{});
  resetRequiredSoa(paths.arrivals, entry_count, 0.0);
  resetRequiredSoa(paths.slews, entry_count, 0.0);
  resetRequiredSoa(paths.launches, entry_count, 0.0);
  resetRequiredSoa(paths.start_nodes, entry_count, NodeId{});
  resetOptionalSoa(paths.predecessor_nodes, entry_count, NodeId{});
  resetOptionalSoa(paths.predecessor_arcs, entry_count, ArcId{});
  resetOptionalSoa(paths.predecessor_arc_delays, entry_count, 0.0);
  resetOptionalSoa(paths.predecessor_transitions, entry_count, Transition::kNone);
  resetOptionalSoa(paths.crpr_nodes, entry_count, NodeId{});
  resetOptionalSoa(paths.crpr_transitions, entry_count, Transition::kNone);

  std::size_t flat = 0;
  for (std::size_t row_index = 0; row_index < work.path_rows.size(); ++row_index) {
    paths.row_offsets[row_index] = flat;
    const auto& row = work.path_rows[row_index];
    for (std::size_t position = 0; position < row.size(); ++position) {
      const auto& path = row[position];
      if ((position != 0 && !(row[position - 1].clock_tag < path.clock_tag)) || path.clock_tag.value() >= work.clock_tag_count
          || path.start_node.value() >= work.node_count || !std::isfinite(path.arrival) || !std::isfinite(path.slew) || path.slew < 0.0
          || !std::isfinite(path.launch) || path.crpr_node.has_value() != path.crpr_transition.has_value())
        return CsrCpuReferenceStageStatus::kInvariantViolation;

      paths.clock_tags.values[flat] = path.clock_tag;
      paths.arrivals.values[flat] = path.arrival;
      paths.slews.values[flat] = path.slew;
      paths.launches.values[flat] = path.launch;
      paths.start_nodes.values[flat] = path.start_node;
      if (path.crpr_node) {
        if (path.crpr_node->value() >= work.node_count || *path.crpr_transition == Transition::kNone)
          return CsrCpuReferenceStageStatus::kInvariantViolation;
        paths.crpr_nodes.presence[flat] = 1;
        paths.crpr_nodes.values[flat] = *path.crpr_node;
        paths.crpr_transitions.presence[flat] = 1;
        paths.crpr_transitions.values[flat] = *path.crpr_transition;
      }

      if (path.predecessor_node.has_value() != path.predecessor_arc.has_value())
        return CsrCpuReferenceStageStatus::kInvariantViolation;
      if (path.predecessor_node) {
        if (path.predecessor_node->value() >= work.node_count || path.predecessor_arc->value() >= work.arc_count || !std::isfinite(path.predecessor_arc_delay)
            || path.predecessor_transition == Transition::kNone)
          return CsrCpuReferenceStageStatus::kInvariantViolation;
        paths.predecessor_nodes.presence[flat] = 1;
        paths.predecessor_nodes.values[flat] = *path.predecessor_node;
        paths.predecessor_arcs.presence[flat] = 1;
        paths.predecessor_arcs.values[flat] = *path.predecessor_arc;
        paths.predecessor_arc_delays.presence[flat] = 1;
        paths.predecessor_arc_delays.values[flat] = path.predecessor_arc_delay;
        paths.predecessor_transitions.presence[flat] = 1;
        paths.predecessor_transitions.values[flat] = path.predecessor_transition;
      } else if (!isPositiveZero(path.predecessor_arc_delay) || path.predecessor_transition != Transition::kNone) {
        return CsrCpuReferenceStageStatus::kInvariantViolation;
      }
      ++flat;
    }
  }
  paths.row_offsets[work.path_row_count] = flat;
  return flat == entry_count ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kInvariantViolation;
}

[[nodiscard]] bool validateClockShape(const ClockResultSoa& clock, std::size_t node_count)
{
  const auto lane_count = node_count * kAnalysisCount * kTransitionCount;
  if (!validateOptionalSoa(clock.arrivals, lane_count, false) || !validateOptionalSoa(clock.slews, lane_count, false)
      || !validateOptionalSoa(clock.predecessor_nodes, lane_count, false) || !validateOptionalSoa(clock.predecessor_arc_delays, lane_count, false)
      || !validateOptionalSoa(clock.predecessor_transitions, lane_count, false))
    return false;
  for (std::size_t lane = 0; lane < lane_count; ++lane) {
    const auto predecessor = clock.predecessor_nodes.presence[lane];
    if (predecessor != clock.predecessor_arc_delays.presence[lane] || predecessor != clock.predecessor_transitions.presence[lane])
      return false;
    if ((clock.arrivals.presence[lane] != 0 && !std::isfinite(clock.arrivals.values[lane]))
        || (clock.slews.presence[lane] != 0 && (!std::isfinite(clock.slews.values[lane]) || clock.slews.values[lane] < 0.0)))
      return false;
    if (predecessor != 0 && (clock.arrivals.presence[lane] == 0 || clock.predecessor_nodes.values[lane].value() >= node_count
                            || !std::isfinite(clock.predecessor_arc_delays.values[lane]) || clock.predecessor_transitions.values[lane] == Transition::kNone))
      return false;
  }
  return true;
}

[[nodiscard]] bool validateNodeShape(const CsrCpuReferenceWorkState& work)
{
  const auto& nodes = work.result.nodes;
  if (!validateOptionalSoa(nodes.is_clock_point, work.node_count, true) || !validateOptionalSoa(nodes.arrivals, work.node_count, true)
      || !validateOptionalSoa(nodes.requireds, work.node_count, true) || !validateOptionalSoa(nodes.slacks, work.node_count, true)
      || !validateOptionalSoa(nodes.launches, work.node_count, true) || !validateOptionalSoa(nodes.clock_names, work.node_count, false)
      || !validateOptionalSoa(nodes.predecessor_nodes, work.node_count, false) || !validateOptionalSoa(nodes.predecessor_arcs, work.node_count, false))
    return false;

  for (std::size_t node = 0; node < work.node_count; ++node) {
    const double arrival = nodes.arrivals.values[node];
    const double required = nodes.requireds.values[node];
    if (nodes.is_clock_point.values[node] > 1 || (!std::isfinite(arrival) && !isNegativeInfinity(arrival))
        || (!std::isfinite(required) && !isPositiveInfinity(required)) || !std::isfinite(nodes.slacks.values[node])
        || !std::isfinite(nodes.launches.values[node]) || nodes.predecessor_nodes.presence[node] != nodes.predecessor_arcs.presence[node])
      return false;
    if (nodes.predecessor_nodes.presence[node] != 0
        && (nodes.predecessor_nodes.values[node].value() >= work.node_count || nodes.predecessor_arcs.values[node].value() >= work.arc_count))
      return false;
  }
  return true;
}

[[nodiscard]] bool validatePathShape(const CsrCpuReferenceWorkState& work)
{
  const auto& paths = work.result.paths;
  if (paths.row_offsets.size() != work.path_row_count + 1 || paths.row_offsets.empty() || paths.row_offsets.front() != 0)
    return false;
  const std::size_t entry_count = paths.clock_tags.values.size();
  if (paths.row_offsets.back() != entry_count || !validateOptionalSoa(paths.clock_tags, entry_count, true)
      || !validateOptionalSoa(paths.arrivals, entry_count, true) || !validateOptionalSoa(paths.slews, entry_count, true)
      || !validateOptionalSoa(paths.launches, entry_count, true) || !validateOptionalSoa(paths.start_nodes, entry_count, true)
      || !validateOptionalSoa(paths.predecessor_nodes, entry_count, false) || !validateOptionalSoa(paths.predecessor_arcs, entry_count, false)
      || !validateOptionalSoa(paths.predecessor_arc_delays, entry_count, false) || !validateOptionalSoa(paths.predecessor_transitions, entry_count, false)
      || !validateOptionalSoa(paths.crpr_nodes, entry_count, false) || !validateOptionalSoa(paths.crpr_transitions, entry_count, false))
    return false;
  for (std::size_t row = 0; row < work.path_row_count; ++row)
    if (paths.row_offsets[row] > paths.row_offsets[row + 1])
      return false;
  for (std::size_t entry = 0; entry < entry_count; ++entry) {
    const std::uint8_t predecessor_presence = paths.predecessor_nodes.presence[entry];
    if (!std::isfinite(paths.arrivals.values[entry]) || !std::isfinite(paths.slews.values[entry]) || paths.slews.values[entry] < 0.0
        || !std::isfinite(paths.launches.values[entry]) || paths.clock_tags.values[entry].value() >= work.clock_tag_count
        || paths.start_nodes.values[entry].value() >= work.node_count || paths.predecessor_arcs.presence[entry] != predecessor_presence
        || paths.predecessor_arc_delays.presence[entry] != predecessor_presence || paths.predecessor_transitions.presence[entry] != predecessor_presence
        || paths.crpr_nodes.presence[entry] != paths.crpr_transitions.presence[entry]
        || (paths.crpr_nodes.presence[entry] != 0 && (paths.crpr_nodes.values[entry].value() >= work.node_count
                                                    || paths.crpr_transitions.values[entry] == Transition::kNone)))
      return false;
  }
  return true;
}

[[nodiscard]] bool validateArcShape(const ActiveCsrEdgeView& edges, const CsrCpuReferenceWorkState& work)
{
  const auto& arcs = work.result.arcs;
  if (!validateOptionalSoa(arcs.delays, work.arc_count, false) || !validateOptionalSoa(arcs.min_delays, work.arc_count, false)
      || !validateOptionalSoa(arcs.max_delays, work.arc_count, false) || !validateOptionalSoa(arcs.transition_delays, work.arc_transition_lane_count, false)
      || !validateOptionalSoa(arcs.input_output_delays, work.arc_io_lane_count, false) || !validateOptionalSoa(arcs.graph_delays, work.arc_io_lane_count, false)
      || !validateOptionalSoa(arcs.transition_types, work.arc_count * kTransitionCount, false) || arcs.timing_arc_row_offsets.size() != work.arc_count + 1
      || arcs.timing_arc_indices.size() != work.timing_arc_count || !validateOptionalSoa(arcs.timing_arc_delays, work.timing_arc_lane_count, false)
      || !validateOptionalSoa(arcs.timing_arc_fallbacks, work.timing_arc_lane_count, false))
    return false;
  if (arcs.timing_arc_row_offsets.empty() || arcs.timing_arc_row_offsets.front() != 0 || arcs.timing_arc_row_offsets.back() != work.timing_arc_count)
    return false;
  for (std::size_t arc = 0; arc < work.arc_count; ++arc) {
    const std::uint8_t active = edges.isActive(ArcId{arc}) ? 1 : 0;
    if (arcs.delays.presence[arc] != active || arcs.min_delays.presence[arc] != active || arcs.max_delays.presence[arc] != active)
      return false;
    for (std::size_t lane = arc * kAnalysisCount * kTransitionCount; lane < (arc + 1) * kAnalysisCount * kTransitionCount; ++lane)
      if (arcs.transition_delays.presence[lane] != active || (active == 0 && arcs.transition_delays.values[lane] != 0.0))
        return false;
    if (active == 0) {
      for (std::size_t lane = arc * kTransitionCount; lane < (arc + 1) * kTransitionCount; ++lane)
        if (arcs.transition_types.presence[lane] != 0 || arcs.transition_types.values[lane] != Transition::kNone)
          return false;
      const std::size_t io_begin = arc * kAnalysisCount * kTransitionCount * kTransitionCount;
      const std::size_t io_end = (arc + 1) * kAnalysisCount * kTransitionCount * kTransitionCount;
      for (std::size_t lane = io_begin; lane < io_end; ++lane)
        if (arcs.input_output_delays.presence[lane] != 0 || arcs.input_output_delays.values[lane] != 0.0
            || arcs.graph_delays.presence[lane] != 0 || arcs.graph_delays.values[lane] != 0.0)
          return false;
    }
    if (active == 0
        && (arcs.delays.values[arc] != 0.0 || arcs.min_delays.values[arc] != 0.0 || arcs.max_delays.values[arc] != 0.0
            || arcs.timing_arc_row_offsets[arc] != arcs.timing_arc_row_offsets[arc + 1]))
      return false;
    if (arcs.timing_arc_row_offsets[arc] > arcs.timing_arc_row_offsets[arc + 1] || !std::isfinite(arcs.delays.values[arc])
        || !std::isfinite(arcs.min_delays.values[arc]) || !std::isfinite(arcs.max_delays.values[arc]))
      return false;
  }
  for (double delay : arcs.transition_delays.values)
    if (!std::isfinite(delay))
      return false;
  for (std::size_t lane = 0; lane < work.arc_io_lane_count; ++lane) {
    if ((arcs.input_output_delays.presence[lane] != 0 && !std::isfinite(arcs.input_output_delays.values[lane]))
        || (arcs.graph_delays.presence[lane] != 0 && !std::isfinite(arcs.graph_delays.values[lane])))
      return false;
  }
  for (std::size_t lane = 0; lane < work.timing_arc_lane_count; ++lane) {
    if (arcs.timing_arc_delays.presence[lane] != arcs.timing_arc_fallbacks.presence[lane]
        || (arcs.timing_arc_delays.presence[lane] != 0 && (!std::isfinite(arcs.timing_arc_delays.values[lane]) || arcs.timing_arc_fallbacks.values[lane] > 1)))
      return false;
  }
  return true;
}

[[nodiscard]] bool validateFinalShape(const ActiveCsrEdgeView& edges, const CsrCpuReferenceWorkState& work)
{
  const auto& result = work.result;
  if (result.level_offsets.empty() || result.level_offsets.front() != 0 || result.level_offsets.back() != work.node_count
      || result.level_nodes.size() != work.node_count || result.node_to_level.size() != work.node_count || !validateNodeShape(work) || !validateClockShape(result.effective_clock, work.node_count)
      || !validateClockShape(result.physical_clock, work.node_count) || !validateOptionalSoa(result.data_slews, work.node_lane_count, false)
      || !validatePathShape(work) || !validateArcShape(edges, work))
    return false;
  for (std::size_t lane = 0; lane < work.node_lane_count; ++lane)
    if (result.data_slews.presence[lane] != 0 && (!std::isfinite(result.data_slews.values[lane]) || result.data_slews.values[lane] < 0.0))
      return false;
  return true;
}

}  // namespace

CsrCpuReferenceStageStatus propagateCsrCpuReferenceRequired(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads, CsrCpuReferenceWorkState& work)
{
  CsrCpuReferenceStageStatus status = seedEndpointRequired(payloads, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  status = seedCsrCpuSequentialRequired(payloads, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  status = reverseRequired(edges, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  status = finalizeSlacks(work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  status = flattenPaths(work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  return validateFinalShape(edges, work) ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kInvariantViolation;
}

}  // namespace ista::csr
