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
#include <atomic>
#include <exception>
#include <limits>
#include <optional>
#include <omp.h>
#include <set>
#include <stdexcept>
#include <utility>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "Database.hpp"
#include "TimingResultScatterInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::detail {
namespace {

[[nodiscard]] constexpr std::size_t nodeLane(std::size_t node, std::size_t analysis, std::size_t transition) noexcept
{
  return (node * kAnalysisCount + analysis) * kTransitionCount + transition;
}

[[nodiscard]] constexpr std::size_t pathRow(std::size_t node, std::size_t analysis, std::size_t source, std::size_t transition) noexcept
{
  return ((node * kAnalysisCount + analysis) * kPathSourceCount + source) * kTransitionCount + transition;
}

[[nodiscard]] constexpr std::size_t arcTransitionLane(std::size_t arc, std::size_t analysis, std::size_t input_transition) noexcept
{
  return (arc * kAnalysisCount + analysis) * kTransitionCount + input_transition;
}

[[nodiscard]] constexpr std::size_t arcIoLane(std::size_t arc, std::size_t analysis, std::size_t input_transition, std::size_t output_transition) noexcept
{
  return arcTransitionLane(arc, analysis, input_transition) * kTransitionCount + output_transition;
}

[[nodiscard]] constexpr std::size_t arcTypeLane(std::size_t arc, std::size_t input_transition) noexcept
{
  return arc * kTransitionCount + input_transition;
}

[[nodiscard]] constexpr std::size_t timingArcLane(std::size_t child, std::size_t analysis, std::size_t input_transition, std::size_t output_transition) noexcept
{
  return ((child * kAnalysisCount + analysis) * kTransitionCount + input_transition) * kTransitionCount + output_transition;
}

[[nodiscard]] std::optional<ista::AnalysisType> nativeAnalysis(AnalysisMode analysis) noexcept
{
  switch (analysis) {
    case AnalysisMode::kMax:
      return ista::AnalysisType::kMax;
    case AnalysisMode::kMin:
      return ista::AnalysisType::kMin;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ista::TransType> nativeTransition(Transition transition) noexcept
{
  switch (transition) {
    case Transition::kRise:
      return ista::TransType::kRise;
    case Transition::kFall:
      return ista::TransType::kFall;
    case Transition::kNone:
      return ista::TransType::kNone;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ista::PathSourceType> nativePathSource(PathSource source) noexcept
{
  switch (source) {
    case PathSource::kInput:
      return ista::PathSourceType::kInput;
    case PathSource::kRegister:
      return ista::PathSourceType::kRegister;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> nativeSize(std::uint64_t value) noexcept
{
  if constexpr (std::numeric_limits<std::size_t>::max() < std::numeric_limits<std::uint64_t>::max())
    if (value > std::numeric_limits<std::size_t>::max())
      return std::nullopt;
  return static_cast<std::size_t>(value);
}

[[nodiscard]] std::optional<StableScatterTargets> resolveStableTargets(ista::Database& database, const CsrReferenceInput& committed_input,
                                                                       const CsrReferenceInput& current_input)
{
  auto& native_points = database.get_timing_point_map();
  auto& native_arcs = database.get_arc_list();
  if (current_input.nodes.size() != committed_input.nodes.size() || current_input.node_ids_by_key.size() != committed_input.node_ids_by_key.size()
      || current_input.arcs.size() != committed_input.arcs.size() || current_input.arc_ids_by_key.size() != committed_input.arc_ids_by_key.size()
      || native_points.size() != current_input.nodes.size() || native_arcs.size() != current_input.arcs.size())
    return std::nullopt;

  StableScatterTargets targets;
  targets.nodes.resize(committed_input.nodes.size());
  targets.arcs.resize(committed_input.arcs.size());
  targets.timing_order = &database.get_timing_order_list();

  std::set<ista::TimingPoint*> unique_points;
  std::set<ista::Arc*> unique_arcs;

  for (std::size_t index = 0; index < committed_input.nodes.size(); ++index) {
    const NodeKey& key = committed_input.nodes[index].key;
    const auto found = std::lower_bound(current_input.node_ids_by_key.begin(), current_input.node_ids_by_key.end(), key,
                                        [](const NodeIdLookupEntry& entry, const NodeKey& candidate) { return nodeKeyLess(entry.key, candidate); });
    if (found == current_input.node_ids_by_key.end() || !(found->key == key) || found->id.value() >= current_input.nodes.size())
      return std::nullopt;
    const NodeRecord& current_node = current_input.nodes[found->id.value()];
    if (current_node.id != found->id || !(current_node.key == found->key))
      return std::nullopt;
    const std::string& full_name = current_node.key.full_name;
    auto native = native_points.find(full_name);
    if (native == native_points.end() || !unique_points.insert(&native->second).second)
      return std::nullopt;
    targets.nodes[index] = ResolvedNodeTarget{&native->second, &full_name};
  }

  for (std::size_t index = 0; index < committed_input.arcs.size(); ++index) {
    const ArcKey& key = committed_input.arcs[index].key;
    const auto found = std::lower_bound(current_input.arc_ids_by_key.begin(), current_input.arc_ids_by_key.end(), key,
                                        [](const ArcIdLookupEntry& entry, const ArcKey& candidate) { return arcKeyLess(entry.key, candidate); });
    if (found == current_input.arc_ids_by_key.end() || !(found->key == key) || found->id.value() >= current_input.arcs.size())
      return std::nullopt;
    const ArcRecord& current_arc = current_input.arcs[found->id.value()];
    if (current_arc.id != found->id || !(current_arc.key == found->key) || current_arc.source.value() >= current_input.nodes.size()
        || current_arc.sink.value() >= current_input.nodes.size())
      return std::nullopt;
    const NodeRecord& source = current_input.nodes[current_arc.source.value()];
    const NodeRecord& sink = current_input.nodes[current_arc.sink.value()];
    if (source.id != current_arc.source || sink.id != current_arc.sink)
      return std::nullopt;
    const auto rank = nativeSize(current_arc.native_traversal_rank);
    if (!rank || *rank >= native_arcs.size() || !unique_arcs.insert(&native_arcs[*rank]).second
        || !nativeArcMatchesReferenceRecord(native_arcs[*rank], current_arc, source, sink))
      return std::nullopt;
    targets.arcs[index] = ResolvedArcTarget{&native_arcs[*rank], *rank};
  }
  return targets;
}

[[nodiscard]] const ResolvedNodeTarget* resolvedNode(const StableScatterTargets& targets, NodeId node) noexcept
{
  return node.value() < targets.nodes.size() && targets.nodes[node.value()].point != nullptr && targets.nodes[node.value()].full_name != nullptr
             ? &targets.nodes[node.value()]
             : nullptr;
}

[[nodiscard]] const ResolvedArcTarget* resolvedArc(const StableScatterTargets& targets, ArcId arc) noexcept
{
  return arc.value() < targets.arcs.size() && targets.arcs[arc.value()].arc != nullptr ? &targets.arcs[arc.value()] : nullptr;
}

// Only dimensions here: row bounds and paired presence are checked while
// preparing their actual values, without another full result traversal.
template <class... Soas>
bool matchingSoaSizes(std::size_t count, const Soas&... values)
{
  return ((values.presence.size() == count && values.values.size() == count) && ...);
}

template <class... Soas>
bool lanePresence(std::size_t index, std::uint8_t expected, const Soas&... values)
{
  return ((values.presence[index] == expected) && ...);
}

bool matchingClockSizes(const ClockResultSoa& values, std::size_t count)
{
  return matchingSoaSizes(count, values.arrivals, values.slews, values.predecessor_nodes, values.predecessor_arc_delays, values.predecessor_transitions);
}

bool validResolvedTimingShapes(const CsrTimingValues& result, std::size_t node_count, std::size_t arc_count)
{
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  if (node_count > (maximum - 1) / 8 || arc_count > (maximum - 1) / 8)
    return false;
  const auto& nodes = result.nodes;
  const auto& paths = result.paths;
  const auto& arcs = result.arcs;
  const auto child_count = arcs.timing_arc_indices.size();
  if (child_count > maximum / 8)
    return false;
  return result.node_to_level.size() == node_count && result.level_nodes.size() == node_count
         && matchingSoaSizes(node_count, nodes.is_clock_point, nodes.arrivals, nodes.requireds, nodes.slacks, nodes.launches, nodes.clock_names,
                             nodes.predecessor_nodes, nodes.predecessor_arcs)
         && matchingClockSizes(result.effective_clock, node_count * 4) && matchingClockSizes(result.physical_clock, node_count * 4)
         && matchingSoaSizes(node_count * 4, result.data_slews) && paths.row_offsets.size() == node_count * 8 + 1 && paths.row_offsets.front() == 0
         && paths.row_offsets.back() == paths.clock_tags.values.size()
         && matchingSoaSizes(paths.clock_tags.values.size(), paths.clock_tags, paths.arrivals, paths.slews, paths.launches, paths.start_nodes,
                             paths.predecessor_nodes, paths.predecessor_arcs, paths.predecessor_arc_delays, paths.predecessor_transitions, paths.crpr_nodes,
                             paths.crpr_transitions)
         && matchingSoaSizes(arc_count, arcs.delays, arcs.min_delays, arcs.max_delays) && matchingSoaSizes(arc_count * 4, arcs.transition_delays)
         && matchingSoaSizes(arc_count * 8, arcs.input_output_delays, arcs.graph_delays) && matchingSoaSizes(arc_count * 2, arcs.transition_types)
         && arcs.timing_arc_row_offsets.size() == arc_count + 1 && arcs.timing_arc_row_offsets.front() == 0 && arcs.timing_arc_row_offsets.back() == child_count
         && matchingSoaSizes(child_count * 8, arcs.timing_arc_delays, arcs.timing_arc_fallbacks);
}

[[nodiscard]] bool prepareClockState(const ClockResultSoa& source, const TimingScatterLayout& layout, const StableScatterTargets& targets, std::size_t node,
                                     PreparedClockState& prepared)
{
  for (std::size_t analysis_index = 0; analysis_index < kAnalysisCount; ++analysis_index) {
    const auto analysis = nativeAnalysis(layout.analysis_order[analysis_index]);
    if (!analysis)
      return false;
    for (std::size_t transition_index = 0; transition_index < kTransitionCount; ++transition_index) {
      const auto transition = nativeTransition(layout.transition_order[transition_index]);
      if (!transition)
        return false;
      const std::size_t lane = nodeLane(node, analysis_index, transition_index);
      const auto arrival_present = source.arrivals.presence[lane];
      const auto predecessor_present = source.predecessor_nodes.presence[lane];
      if (arrival_present > 1 || predecessor_present > 1 || !lanePresence(lane, arrival_present, source.slews)
          || !lanePresence(lane, predecessor_present, source.predecessor_arc_delays, source.predecessor_transitions))
        return false;
      if (source.arrivals.presence[lane] != 0)
        prepared.arrivals[*analysis][*transition] = source.arrivals.values[lane];
      if (source.slews.presence[lane] != 0)
        prepared.slews[*analysis][*transition] = source.slews.values[lane];
      if (source.predecessor_nodes.presence[lane] != 0) {
        const auto* predecessor = resolvedNode(targets, source.predecessor_nodes.values[lane]);
        if (predecessor == nullptr)
          return false;
        prepared.predecessors[*analysis][*transition] = *predecessor->full_name;
      }
      if (source.predecessor_arc_delays.presence[lane] != 0)
        prepared.predecessor_arc_delays[*analysis][*transition] = source.predecessor_arc_delays.values[lane];
      if (source.predecessor_transitions.presence[lane] != 0) {
        const auto predecessor_transition = nativeTransition(source.predecessor_transitions.values[lane]);
        if (!predecessor_transition)
          return false;
        prepared.predecessor_transitions[*analysis][*transition] = *predecessor_transition;
      }
    }
  }
  return true;
}

[[nodiscard]] std::optional<CsrScatterCode> preparePathStates(const CsrTimingValues& result, const TimingScatterLayout& layout,
                                                              const StableScatterTargets& targets, std::size_t node, NativePathStateMap& prepared)
{
  const PathResultSoa& paths = result.paths;
  for (std::size_t analysis_index = 0; analysis_index < kAnalysisCount; ++analysis_index) {
    const auto analysis = nativeAnalysis(layout.analysis_order[analysis_index]);
    if (!analysis)
      return CsrScatterCode::kIdMappingMismatch;
    for (std::size_t source_index = 0; source_index < kPathSourceCount; ++source_index) {
      const auto source = nativePathSource(layout.path_source_order[source_index]);
      if (!source)
        return CsrScatterCode::kIdMappingMismatch;
      for (std::size_t transition_index = 0; transition_index < kTransitionCount; ++transition_index) {
        const auto transition = nativeTransition(layout.transition_order[transition_index]);
        if (!transition)
          return CsrScatterCode::kIdMappingMismatch;
        const std::size_t row = pathRow(node, analysis_index, source_index, transition_index);
        const auto begin = nativeSize(paths.row_offsets[row]);
        const auto end = nativeSize(paths.row_offsets[row + 1]);
        if (!begin || !end || *begin > *end || *end > paths.clock_tags.values.size())
          return CsrScatterCode::kShapeMismatch;
        if (*begin == *end)
          continue;
        auto& native_row = prepared[*analysis][*source][*transition];
        for (std::size_t entry = *begin; entry < *end; ++entry) {
          if (!lanePresence(entry, 1, paths.clock_tags, paths.arrivals, paths.slews, paths.launches, paths.start_nodes)
              || paths.predecessor_nodes.presence[entry] > 1 || paths.predecessor_arcs.presence[entry] > 1 || paths.predecessor_arc_delays.presence[entry] > 1
              || paths.predecessor_transitions.presence[entry] > 1 || paths.crpr_nodes.presence[entry] > 1 || paths.crpr_transitions.presence[entry] > 1)
            return CsrScatterCode::kShapeMismatch;
          const std::uint64_t tag_index = paths.clock_tags.values[entry].value();
          if (tag_index >= layout.clock_tags.size())
            return CsrScatterCode::kIdMappingMismatch;
          const std::string& clock_tag = layout.clock_tags[tag_index];
          const auto* start = resolvedNode(targets, paths.start_nodes.values[entry]);
          if (start == nullptr)
            return CsrScatterCode::kIdMappingMismatch;

          ista::TimingPathState state;
          state.set_arrival(paths.arrivals.values[entry]);
          state.set_slew(paths.slews.values[entry]);
          state.set_launch_time(paths.launches.values[entry]);
          state.set_start_point(*start->full_name);
          state.set_clock_name(clock_tag);
          state.set_trans_type(*transition);

          const bool has_predecessor = paths.predecessor_nodes.presence[entry] != 0;
          if (has_predecessor != (paths.predecessor_arcs.presence[entry] != 0) || has_predecessor != (paths.predecessor_arc_delays.presence[entry] != 0)
              || has_predecessor != (paths.predecessor_transitions.presence[entry] != 0))
            return CsrScatterCode::kPredecessorMismatch;
          if (has_predecessor) {
            const auto* predecessor = resolvedNode(targets, paths.predecessor_nodes.values[entry]);
            const auto* predecessor_arc = resolvedArc(targets, paths.predecessor_arcs.values[entry]);
            const auto predecessor_transition = nativeTransition(paths.predecessor_transitions.values[entry]);
            if (predecessor == nullptr || predecessor_arc == nullptr || !predecessor_transition)
              return CsrScatterCode::kIdMappingMismatch;
            state.set_predecessor(*predecessor->full_name);
            state.set_predecessor_arc_idx(predecessor_arc->native_rank);
            state.set_predecessor_arc_delay(paths.predecessor_arc_delays.values[entry]);
            state.set_predecessor_trans_type(*predecessor_transition);
          }

          const bool has_crpr = paths.crpr_nodes.presence[entry] != 0;
          if (has_crpr != (paths.crpr_transitions.presence[entry] != 0))
            return CsrScatterCode::kPredecessorMismatch;
          if (has_crpr) {
            const auto* crpr = resolvedNode(targets, paths.crpr_nodes.values[entry]);
            const auto crpr_transition = nativeTransition(paths.crpr_transitions.values[entry]);
            if (crpr == nullptr || !crpr_transition)
              return CsrScatterCode::kIdMappingMismatch;
            state.set_crpr_clock_pin(*crpr->full_name);
            state.set_crpr_clock_trans_type(*crpr_transition);
          }

          if (!native_row.emplace(clock_tag, std::move(state)).second)
            return CsrScatterCode::kShapeMismatch;
        }
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CsrScatterCode> prepareNodeState(const CsrTimingValues& result, const TimingScatterLayout& layout,
                                                             const StableScatterTargets& targets, std::size_t node, PreparedTimingPointState& prepared)
{
  const auto& nodes = result.nodes;
  if (!lanePresence(node, 1, nodes.is_clock_point, nodes.arrivals, nodes.requireds, nodes.slacks, nodes.launches) || nodes.clock_names.presence[node] > 1
      || nodes.predecessor_nodes.presence[node] > 1 || nodes.predecessor_arcs.presence[node] > 1)
    return CsrScatterCode::kShapeMismatch;
  const auto level = nativeSize(result.node_to_level[node]);
  if (!level || *level > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    return CsrScatterCode::kResultNumericInvalid;

  prepared.target = targets.nodes[node].point;
  prepared.arrival = result.nodes.arrivals.values[node];
  prepared.required = result.nodes.requireds.values[node];
  prepared.slack = result.nodes.slacks.values[node];
  prepared.launch = result.nodes.launches.values[node];
  prepared.level = static_cast<std::int32_t>(*level);
  prepared.is_clock_point = result.nodes.is_clock_point.values[node] != 0;
  prepared.predecessor_arc_index = std::numeric_limits<std::size_t>::max();
  if (result.nodes.clock_names.presence[node] != 0)
    prepared.clock_name = result.nodes.clock_names.values[node];

  const bool has_predecessor = result.nodes.predecessor_nodes.presence[node] != 0;
  if (has_predecessor != (result.nodes.predecessor_arcs.presence[node] != 0))
    return CsrScatterCode::kPredecessorMismatch;
  if (has_predecessor) {
    const auto* predecessor = resolvedNode(targets, result.nodes.predecessor_nodes.values[node]);
    const auto* predecessor_arc = resolvedArc(targets, result.nodes.predecessor_arcs.values[node]);
    if (predecessor == nullptr || predecessor_arc == nullptr)
      return CsrScatterCode::kIdMappingMismatch;
    prepared.predecessor = *predecessor->full_name;
    prepared.predecessor_arc_index = predecessor_arc->native_rank;
  }

  if (!prepareClockState(result.effective_clock, layout, targets, node, prepared.effective_clock)
      || !prepareClockState(result.physical_clock, layout, targets, node, prepared.physical_clock))
    return CsrScatterCode::kIdMappingMismatch;

  for (std::size_t analysis_index = 0; analysis_index < kAnalysisCount; ++analysis_index) {
    const auto analysis = nativeAnalysis(layout.analysis_order[analysis_index]);
    if (!analysis)
      return CsrScatterCode::kIdMappingMismatch;
    for (std::size_t transition_index = 0; transition_index < kTransitionCount; ++transition_index) {
      const auto transition = nativeTransition(layout.transition_order[transition_index]);
      if (!transition)
        return CsrScatterCode::kIdMappingMismatch;
      const std::size_t lane = nodeLane(node, analysis_index, transition_index);
      if (result.data_slews.presence[lane] > 1)
        return CsrScatterCode::kShapeMismatch;
      if (result.data_slews.presence[lane] != 0)
        prepared.data_slews[*analysis][*transition] = result.data_slews.values[lane];
    }
  }
  return preparePathStates(result, layout, targets, node, prepared.path_states);
}

[[nodiscard]] std::optional<CsrScatterCode> prepareArcState(const CsrTimingValues& result, const TimingScatterLayout& layout,
                                                            const StableScatterTargets& targets, std::size_t arc, PreparedArcState& prepared)
{
  const ArcResultSoa& arcs = result.arcs;
  if (!lanePresence(arc, 1, arcs.delays, arcs.min_delays, arcs.max_delays))
    return CsrScatterCode::kShapeMismatch;
  prepared.target = targets.arcs[arc].arc;
  prepared.delay = arcs.delays.values[arc];
  prepared.minimum_delay = arcs.min_delays.values[arc];
  prepared.maximum_delay = arcs.max_delays.values[arc];

  for (std::size_t analysis_index = 0; analysis_index < kAnalysisCount; ++analysis_index) {
    const auto analysis = nativeAnalysis(layout.analysis_order[analysis_index]);
    if (!analysis)
      return CsrScatterCode::kIdMappingMismatch;
    for (std::size_t input_index = 0; input_index < kTransitionCount; ++input_index) {
      const auto input_transition = nativeTransition(layout.transition_order[input_index]);
      if (!input_transition)
        return CsrScatterCode::kIdMappingMismatch;
      const std::size_t transition_lane = arcTransitionLane(arc, analysis_index, input_index);
      if (arcs.transition_delays.presence[transition_lane] != 1)
        return CsrScatterCode::kShapeMismatch;
      prepared.transition_delays[*analysis][*input_transition] = arcs.transition_delays.values[transition_lane];
      for (std::size_t output_index = 0; output_index < kTransitionCount; ++output_index) {
        const auto output_transition = nativeTransition(layout.transition_order[output_index]);
        if (!output_transition)
          return CsrScatterCode::kIdMappingMismatch;
        const std::size_t lane = arcIoLane(arc, analysis_index, input_index, output_index);
        if (arcs.input_output_delays.presence[lane] > 1 || arcs.graph_delays.presence[lane] > 1)
          return CsrScatterCode::kShapeMismatch;
        if (arcs.input_output_delays.presence[lane] != 0)
          prepared.input_output_delays[*analysis][*input_transition][*output_transition] = arcs.input_output_delays.values[lane];
        if (arcs.graph_delays.presence[lane] != 0)
          prepared.graph_delays[*analysis][*input_transition][*output_transition] = arcs.graph_delays.values[lane];
      }
    }
  }

  for (std::size_t input_index = 0; input_index < kTransitionCount; ++input_index) {
    const auto input_transition = nativeTransition(layout.transition_order[input_index]);
    if (!input_transition)
      return CsrScatterCode::kIdMappingMismatch;
    const std::size_t lane = arcTypeLane(arc, input_index);
    if (arcs.transition_types.presence[lane] > 1)
      return CsrScatterCode::kShapeMismatch;
    if (arcs.transition_types.presence[lane] != 0) {
      const auto output_transition = nativeTransition(arcs.transition_types.values[lane]);
      if (!output_transition)
        return CsrScatterCode::kIdMappingMismatch;
      prepared.transition_types[*input_transition] = *output_transition;
    }
  }

  const auto begin = nativeSize(arcs.timing_arc_row_offsets[arc]);
  const auto end = nativeSize(arcs.timing_arc_row_offsets[arc + 1]);
  if (!begin || !end || *begin > *end || *end > arcs.timing_arc_indices.size())
    return CsrScatterCode::kShapeMismatch;
  for (std::size_t child = *begin; child < *end; ++child) {
    const std::int32_t timing_arc_index = arcs.timing_arc_indices[child];
    for (std::size_t analysis_index = 0; analysis_index < kAnalysisCount; ++analysis_index) {
      const auto analysis = nativeAnalysis(layout.analysis_order[analysis_index]);
      if (!analysis)
        return CsrScatterCode::kIdMappingMismatch;
      for (std::size_t input_index = 0; input_index < kTransitionCount; ++input_index) {
        const auto input_transition = nativeTransition(layout.transition_order[input_index]);
        if (!input_transition)
          return CsrScatterCode::kIdMappingMismatch;
        for (std::size_t output_index = 0; output_index < kTransitionCount; ++output_index) {
          const auto output_transition = nativeTransition(layout.transition_order[output_index]);
          if (!output_transition)
            return CsrScatterCode::kIdMappingMismatch;
          const std::size_t lane = timingArcLane(child, analysis_index, input_index, output_index);
          if (arcs.timing_arc_delays.presence[lane] > 1 || arcs.timing_arc_fallbacks.presence[lane] > 1)
            return CsrScatterCode::kShapeMismatch;
          const bool has_delay = arcs.timing_arc_delays.presence[lane] != 0;
          if (has_delay != (arcs.timing_arc_fallbacks.presence[lane] != 0))
            return CsrScatterCode::kShapeMismatch;
          if (!has_delay)
            continue;
          prepared.timing_arc_delays.delay_map[timing_arc_index][*analysis][*input_transition][*output_transition] = arcs.timing_arc_delays.values[lane];
          if (arcs.timing_arc_fallbacks.values[lane] != 0)
            prepared.timing_arc_delays.fallback_key_set.emplace(timing_arc_index, *analysis, *input_transition, *output_transition);
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace

CsrReportNodeReader::CsrReportNodeReader(const StableScatterTargets& targets, const TimingScatterLayout& layout,
                                       const CsrTimingValues& values)
    : _targets(targets), _layout(layout), _values(values)
{
  if (!validResolvedTimingShapes(values, targets.nodes.size(), targets.arcs.size()))
    throw std::invalid_argument("CSR report timing has inconsistent dimensions");
  _ids.reserve(targets.nodes.size());
  for (std::size_t id = 0; id < targets.nodes.size(); ++id)
    _ids.emplace(*targets.nodes[id].full_name, id);
}

ista::TimingPoint& CsrReportNodeReader::point(std::string_view name)
{
  const auto id = _ids.at(name);
  if (const auto found = _points.find(id); found != _points.end()) return found->second;
  PreparedTimingPointState prepared;
  if (const auto failure = prepareNodeState(_values, _layout, _targets, id, prepared))
    throw std::runtime_error("CSR report node cannot be decoded: " + std::string(token(*failure)));
  auto& point = _points.try_emplace(id).first->second;
  prepared.target = &point;
  publishPreparedTimingPoint(prepared);
  return point;
}

TimingResultPreparationResult prepareTimingResultScatter(ista::Database& database, const CsrReferenceInput& committed_input,
                                                         const CsrReferenceInput& current_input, const CsrTimingResult& result)
{
  const auto targets = resolveStableTargets(database, committed_input, current_input);
  if (!targets)
    return TimingResultPreparationFailure{CsrScatterCode::kIdMappingMismatch};

  return prepareResolvedTimingResult(
      *targets, {committed_input.analysis_order, committed_input.path_source_order, committed_input.transition_order, committed_input.clock_tags}, result);
}

TimingResultPreparationResult prepareStableTimingResult(
    Database& database, const std::vector<std::string>& node_names,
    const std::vector<std::optional<std::size_t>>& arc_native_ranks,
    const TimingScatterLayout& layout, const CsrTimingValues& values)
{
  StableScatterTargets targets;
  targets.nodes.reserve(node_names.size());
  targets.arcs.resize(arc_native_ranks.size());
  for (const auto& name : node_names)
    targets.nodes.push_back({&database.get_timing_point_map().at(name), &name});
  for (std::size_t id = 0; id < arc_native_ranks.size(); ++id)
    if (arc_native_ranks[id])
      targets.arcs[id] = {&database.get_arc_list().at(*arc_native_ranks[id]), *arc_native_ranks[id]};
  targets.timing_order = &database.get_timing_order_list();
  return prepareResolvedTimingResult(targets, layout, values);
}

TimingResultPreparationResult prepareResolvedTimingResult(const StableScatterTargets& targets, const TimingScatterLayout& layout, const CsrTimingValues& result)
{
  if (targets.timing_order == nullptr || !validResolvedTimingShapes(result, targets.nodes.size(), targets.arcs.size()))
    return TimingResultPreparationFailure{CsrScatterCode::kShapeMismatch};
  PreparedTimingResult prepared;
  prepared.nodes.resize(targets.nodes.size());
  prepared.arcs.reserve(targets.arcs.size());
  prepared.timing_order_target = targets.timing_order;
  prepared.timing_order.reserve(result.level_nodes.size());

  std::atomic<std::size_t> first_failure{prepared.nodes.size()};
  std::optional<CsrScatterCode> failure_code;
  std::exception_ptr failure_exception;
  const int threads = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(prepared.nodes.size(), omp_get_max_threads())));
  // Each node owns its temporary maps; publication remains serial and no-throw.
#pragma omp parallel for schedule(static) num_threads(threads)
  for (std::size_t node = 0; node < prepared.nodes.size(); ++node) {
    if (node > first_failure.load(std::memory_order_relaxed))
      continue;
    std::optional<CsrScatterCode> failure;
    std::exception_ptr exception;
    try {
      failure = resolvedNode(targets, NodeId{node}) == nullptr
                    ? std::optional{CsrScatterCode::kIdMappingMismatch}
                    : prepareNodeState(result, layout, targets, node, prepared.nodes[node]);
    } catch (...) {
      exception = std::current_exception();
    }
    if (failure || exception) {
#pragma omp critical(ista_scatter_preparation_failure)
      {
        if (node < first_failure.load(std::memory_order_relaxed)) {
          failure_code = failure;
          failure_exception = std::move(exception);
          first_failure.store(node, std::memory_order_relaxed);
        }
      }
    }
  }
  if (failure_exception)
    std::rethrow_exception(failure_exception);
  if (failure_code)
    return TimingResultPreparationFailure{*failure_code};
  for (std::size_t arc = 0; arc < targets.arcs.size(); ++arc) {
    if (targets.arcs[arc].arc == nullptr) {
      if (!lanePresence(arc, 0, result.arcs.delays, result.arcs.min_delays, result.arcs.max_delays))
        return TimingResultPreparationFailure{CsrScatterCode::kIdMappingMismatch};
      continue;
    }
    prepared.arcs.emplace_back();
    if (const auto failure = prepareArcState(result, layout, targets, arc, prepared.arcs.back()))
      return TimingResultPreparationFailure{*failure};
  }
  for (NodeId node : result.level_nodes) {
    const auto* target = resolvedNode(targets, node);
    if (target == nullptr)
      return TimingResultPreparationFailure{CsrScatterCode::kIdMappingMismatch};
    prepared.timing_order.push_back(*target->full_name);
  }
  return prepared;
}

}  // namespace ista::csr::detail
