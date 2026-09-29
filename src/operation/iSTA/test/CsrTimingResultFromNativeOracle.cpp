// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrTimingScatterTestFixture.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::test {
template <typename T>
[[nodiscard]] T canonicalAbsent()
{
  return T{};
}

template <>
[[nodiscard]] Transition canonicalAbsent<Transition>()
{
  return Transition::kNone;
}

namespace {

template <typename T>
void initialize(OptionalSoa<T>& values, std::size_t size)
{
  values.presence.assign(size, 0);
  values.values.assign(size, canonicalAbsent<T>());
}

template <typename T>
void store(OptionalSoa<T>& values, std::size_t index, const std::optional<T>& value)
{
  if (value) {
    values.presence[index] = 1;
    values.values[index] = *value;
  }
}

template <typename T>
void appendRequired(OptionalSoa<T>& values, T value)
{
  values.presence.push_back(1);
  values.values.push_back(std::move(value));
}

template <typename T>
void appendOptional(OptionalSoa<T>& values, const std::optional<T>& value)
{
  values.presence.push_back(value ? 1 : 0);
  values.values.push_back(value.value_or(canonicalAbsent<T>()));
}

[[nodiscard]] NodeId requireNodeId(const CsrReferenceInput& input, const NodeKey& key)
{
  const auto found = std::ranges::lower_bound(input.node_ids_by_key, key, nodeKeyLess, &NodeIdLookupEntry::key);
  if (found == input.node_ids_by_key.end() || found->key != key || found->id.value() >= input.nodes.size()) {
    throw std::runtime_error("timing scatter materializer: NodeKey does not resolve");
  }
  return found->id;
}

[[nodiscard]] ArcId requireArcId(const CsrReferenceInput& input, const ArcKey& key)
{
  const auto found = std::ranges::lower_bound(input.arc_ids_by_key, key, arcKeyLess, &ArcIdLookupEntry::key);
  if (found == input.arc_ids_by_key.end() || found->key != key || found->id.value() >= input.arcs.size()) {
    throw std::runtime_error("timing scatter materializer: ArcKey does not resolve");
  }
  return found->id;
}

[[nodiscard]] ClockTagId requireClockTagId(const CsrReferenceInput& input, const std::string& tag)
{
  const auto found = std::ranges::lower_bound(input.clock_tags, tag);
  if (found == input.clock_tags.end() || *found != tag) {
    throw std::runtime_error("timing scatter materializer: clock tag does not resolve");
  }
  return ClockTagId{static_cast<std::uint64_t>(std::distance(input.clock_tags.begin(), found))};
}

template <typename T, std::size_t Size>
[[nodiscard]] std::size_t requireIndex(const std::array<T, Size>& order, T value, const char* field)
{
  const auto found = std::ranges::find(order, value);
  if (found == order.end()) {
    throw std::runtime_error(std::string("timing scatter materializer: unknown ") + field);
  }
  return static_cast<std::size_t>(std::distance(order.begin(), found));
}

[[nodiscard]] const CsrNativeTimingNode& requireNativeNode(const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle, std::size_t node)
{
  const NodeKey& key = input.nodes.at(node).key;
  const auto found = std::ranges::lower_bound(oracle.nodes, key, nodeKeyLess, &CsrNativeTimingNode::key);
  if (found == oracle.nodes.end() || found->key != key) {
    throw std::runtime_error("timing scatter materializer: native node is missing");
  }
  return *found;
}

[[nodiscard]] const CsrNativeTimingArc& requireNativeArc(const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle, std::size_t arc)
{
  const ArcKey& key = input.arcs.at(arc).key;
  const auto found = std::ranges::lower_bound(oracle.arcs, key, arcKeyLess, &CsrNativeTimingArc::key);
  if (found == oracle.arcs.end() || found->key != key) {
    throw std::runtime_error("timing scatter materializer: native Arc is missing");
  }
  return *found;
}

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

void copyClockState(const CsrNativeClockTimingState& source, ClockResultSoa& destination, std::size_t node)
{
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      const std::size_t lane = nodeLane(node, analysis, transition);
      store(destination.arrivals, lane, source.arrivals[analysis][transition]);
      store(destination.slews, lane, source.slews[analysis][transition]);
      store(destination.predecessor_arc_delays, lane, source.predecessor_arc_delays[analysis][transition]);
      store(destination.predecessor_transitions, lane, source.predecessor_transitions[analysis][transition]);
    }
  }
}

void copyClockPredecessors(const CsrReferenceInput& input, const CsrNativeClockTimingState& source, ClockResultSoa& destination, std::size_t node)
{
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      const auto& predecessor = source.predecessor_nodes[analysis][transition];
      if (predecessor) {
        const std::size_t lane = nodeLane(node, analysis, transition);
        destination.predecessor_nodes.presence[lane] = 1;
        destination.predecessor_nodes.values[lane] = requireNodeId(input, *predecessor);
      }
    }
  }
}

void initializeNodeStorage(CsrTimingResult& result, std::size_t node_count)
{
  initialize(result.nodes.is_clock_point, node_count);
  initialize(result.nodes.arrivals, node_count);
  initialize(result.nodes.requireds, node_count);
  initialize(result.nodes.slacks, node_count);
  initialize(result.nodes.launches, node_count);
  initialize(result.nodes.clock_names, node_count);
  initialize(result.nodes.predecessor_nodes, node_count);
  initialize(result.nodes.predecessor_arcs, node_count);

  const std::size_t lanes = node_count * kAnalysisCount * kTransitionCount;
  initialize(result.effective_clock.arrivals, lanes);
  initialize(result.effective_clock.slews, lanes);
  initialize(result.effective_clock.predecessor_nodes, lanes);
  initialize(result.effective_clock.predecessor_arc_delays, lanes);
  initialize(result.effective_clock.predecessor_transitions, lanes);
  initialize(result.physical_clock.arrivals, lanes);
  initialize(result.physical_clock.slews, lanes);
  initialize(result.physical_clock.predecessor_nodes, lanes);
  initialize(result.physical_clock.predecessor_arc_delays, lanes);
  initialize(result.physical_clock.predecessor_transitions, lanes);
  initialize(result.data_slews, lanes);
}

void materializeNodes(const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle, CsrTimingResult& result)
{
  const std::size_t node_count = input.nodes.size();
  initializeNodeStorage(result, node_count);
  result.node_to_level.resize(node_count);

  std::size_t maximum_level = 0;
  for (std::size_t node = 0; node < node_count; ++node) {
    const CsrNativeTimingNode& native = requireNativeNode(input, oracle, node);
    if (native.level <= 0) {
      throw std::runtime_error("timing scatter materializer: negative native node level");
    }
    const std::size_t level = static_cast<std::size_t>(native.level);
    maximum_level = std::max(maximum_level, level);
    result.node_to_level[node] = level;

    result.nodes.is_clock_point.presence[node] = 1;
    result.nodes.is_clock_point.values[node] = native.is_clock_point ? 1 : 0;
    result.nodes.arrivals.presence[node] = 1;
    result.nodes.arrivals.values[node] = native.arrival;
    result.nodes.requireds.presence[node] = 1;
    result.nodes.requireds.values[node] = native.required;
    result.nodes.slacks.presence[node] = 1;
    result.nodes.slacks.values[node] = native.slack;
    result.nodes.launches.presence[node] = 1;
    result.nodes.launches.values[node] = native.launch;
    store(result.nodes.clock_names, node, native.clock_name);

    if (native.predecessor_node.has_value() != native.predecessor_arc.has_value()) {
      throw std::runtime_error("timing scatter materializer: scalar predecessor presence mismatch");
    }
    if (native.predecessor_node) {
      result.nodes.predecessor_nodes.presence[node] = 1;
      result.nodes.predecessor_nodes.values[node] = requireNodeId(input, *native.predecessor_node);
      result.nodes.predecessor_arcs.presence[node] = 1;
      result.nodes.predecessor_arcs.values[node] = requireArcId(input, *native.predecessor_arc);
    }

    copyClockState(native.effective_clock, result.effective_clock, node);
    copyClockPredecessors(input, native.effective_clock, result.effective_clock, node);
    copyClockState(native.physical_clock, result.physical_clock, node);
    copyClockPredecessors(input, native.physical_clock, result.physical_clock, node);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        store(result.data_slews, nodeLane(node, analysis, transition), native.data_slews[analysis][transition]);
      }
    }
  }

  if (oracle.native_timing_order.size() != node_count) {
    throw std::runtime_error("timing scatter materializer: native timing order cardinality mismatch");
  }
  result.level_nodes.clear();
  result.level_offsets = {0};
  std::vector<std::uint8_t> seen(node_count, 0);
  std::size_t previous_level = 0;
  for (const NodeKey& key : oracle.native_timing_order) {
    const NodeId node = requireNodeId(input, key);
    if (seen[node.value()]++ != 0) {
      throw std::runtime_error("timing scatter materializer: duplicate native timing-order node");
    }
    const std::size_t level = result.node_to_level[node.value()];
    if (result.level_nodes.empty() && level != 1) {
      throw std::runtime_error("timing scatter materializer: native timing order must start at level one");
    }
    if (!result.level_nodes.empty() && level != previous_level) {
      if (level != previous_level + 1) {
        throw std::runtime_error("timing scatter materializer: native timing order has a noncontiguous level");
      }
      result.level_offsets.push_back(result.level_nodes.size());
    }
    result.level_nodes.push_back(node);
    previous_level = level;
  }
  if (std::ranges::find(seen, std::uint8_t{0}) != seen.end()) {
    throw std::runtime_error("timing scatter materializer: native timing order omits a node");
  }
  if (!result.level_nodes.empty()) {
    result.level_offsets.push_back(result.level_nodes.size());
    if (previous_level != maximum_level) {
      throw std::runtime_error("timing scatter materializer: native timing order omits the maximum level");
    }
  }
}

void appendPath(const CsrReferenceInput& input, const CsrNativePathTimingState& path, PathResultSoa& result)
{
  if (path.clock_name != path.clock_tag || path.state_transition != path.lane_transition) {
    throw std::runtime_error("timing scatter materializer: native path has non-canonical implicit fields");
  }
  appendRequired(result.clock_tags, requireClockTagId(input, path.clock_tag));
  appendRequired(result.arrivals, path.arrival);
  appendRequired(result.slews, path.slew);
  appendRequired(result.launches, path.launch);
  appendRequired(result.start_nodes, requireNodeId(input, path.start_node));

  if (path.predecessor_node.has_value() != path.predecessor_arc.has_value()) {
    throw std::runtime_error("timing scatter materializer: path predecessor presence mismatch");
  }
  if (path.predecessor_node) {
    appendOptional(result.predecessor_nodes, std::optional<NodeId>{requireNodeId(input, *path.predecessor_node)});
    appendOptional(result.predecessor_arcs, std::optional<ArcId>{requireArcId(input, *path.predecessor_arc)});
    appendOptional(result.predecessor_arc_delays, std::optional<double>{path.predecessor_arc_delay});
    appendOptional(result.predecessor_transitions, std::optional<Transition>{path.predecessor_transition});
  } else {
    if (path.predecessor_arc_delay != 0.0 || path.predecessor_transition != Transition::kNone) {
      throw std::runtime_error("timing scatter materializer: absent path predecessor has payload");
    }
    appendOptional(result.predecessor_nodes, std::optional<NodeId>{});
    appendOptional(result.predecessor_arcs, std::optional<ArcId>{});
    appendOptional(result.predecessor_arc_delays, std::optional<double>{});
    appendOptional(result.predecessor_transitions, std::optional<Transition>{});
  }

  if (path.crpr_node) {
    appendOptional(result.crpr_nodes, std::optional<NodeId>{requireNodeId(input, *path.crpr_node)});
    appendOptional(result.crpr_transitions, std::optional<Transition>{path.crpr_transition});
  } else {
    if (path.crpr_transition != Transition::kNone) {
      throw std::runtime_error("timing scatter materializer: absent CRPR node has transition payload");
    }
    appendOptional(result.crpr_nodes, std::optional<NodeId>{});
    appendOptional(result.crpr_transitions, std::optional<Transition>{});
  }
}

void materializePaths(const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle, CsrTimingResult& result)
{
  const std::size_t row_count = input.nodes.size() * kAnalysisCount * kPathSourceCount * kTransitionCount;
  std::vector<std::vector<const CsrNativePathTimingState*>> rows(row_count);
  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    const CsrNativeTimingNode& native = requireNativeNode(input, oracle, node);
    for (const CsrNativePathTimingState& path : native.paths) {
      const std::size_t analysis = requireIndex(input.analysis_order, path.analysis, "path analysis");
      const std::size_t source = requireIndex(input.path_source_order, path.source, "path source");
      const std::size_t transition = requireIndex(input.transition_order, path.lane_transition, "path transition");
      rows[pathRow(node, analysis, source, transition)].push_back(&path);
    }
  }

  result.paths.row_offsets.assign(row_count + 1, 0);
  for (std::size_t row = 0; row < row_count; ++row) {
    auto& paths = rows[row];
    std::sort(paths.begin(), paths.end(), [&](const auto* lhs, const auto* rhs) {
      return requireClockTagId(input, lhs->clock_tag).value() < requireClockTagId(input, rhs->clock_tag).value();
    });
    for (std::size_t index = 1; index < paths.size(); ++index) {
      if (paths[index - 1]->clock_tag == paths[index]->clock_tag) {
        throw std::runtime_error("timing scatter materializer: duplicate path clock tag");
      }
    }
    for (const CsrNativePathTimingState* path : paths) {
      appendPath(input, *path, result.paths);
    }
    result.paths.row_offsets[row + 1] = result.paths.clock_tags.values.size();
  }
}

void initializeArcStorage(CsrTimingResult& result, std::size_t arc_count)
{
  initialize(result.arcs.delays, arc_count);
  initialize(result.arcs.min_delays, arc_count);
  initialize(result.arcs.max_delays, arc_count);
  initialize(result.arcs.transition_delays, arc_count * kAnalysisCount * kTransitionCount);
  initialize(result.arcs.input_output_delays, arc_count * kAnalysisCount * kTransitionCount * kTransitionCount);
  initialize(result.arcs.graph_delays, arc_count * kAnalysisCount * kTransitionCount * kTransitionCount);
  initialize(result.arcs.transition_types, arc_count * kTransitionCount);
  result.arcs.timing_arc_row_offsets.assign(arc_count + 1, 0);
}

void materializeArcs(const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle, CsrTimingResult& result)
{
  initializeArcStorage(result, input.arcs.size());
  std::size_t child_count = 0;
  for (std::size_t arc = 0; arc < input.arcs.size(); ++arc) {
    const CsrNativeTimingArc& native = requireNativeArc(input, oracle, arc);
    result.arcs.delays.presence[arc] = 1;
    result.arcs.delays.values[arc] = native.delay;
    result.arcs.min_delays.presence[arc] = 1;
    result.arcs.min_delays.values[arc] = native.min_delay;
    result.arcs.max_delays.presence[arc] = 1;
    result.arcs.max_delays.values[arc] = native.max_delay;

    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
        store(result.arcs.transition_delays, arcTransitionLane(arc, analysis, input_transition), native.transition_delays[analysis][input_transition]);
        for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
          const std::size_t lane = arcIoLane(arc, analysis, input_transition, output_transition);
          store(result.arcs.input_output_delays, lane, native.input_output_delays[analysis][input_transition][output_transition]);
          store(result.arcs.graph_delays, lane, native.graph_delays[analysis][input_transition][output_transition]);
        }
      }
    }
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      store(result.arcs.transition_types, arc * kTransitionCount + transition, native.transition_types[transition]);
    }

    result.arcs.timing_arc_row_offsets[arc] = child_count;
    for (const ChildTimingArcModel& child : input.arcs[arc].transfer.children()) {
      const auto entry = std::ranges::find(native.timing_arcs, child.signature.arc_index, &CsrNativeTimingArcEntry::timing_arc_index);
      if (entry == native.timing_arcs.end()) {
        throw std::runtime_error("timing scatter materializer: timing Arc child is missing");
      }
      result.arcs.timing_arc_indices.push_back(child.signature.arc_index);
      for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
        for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
          for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
            const auto& lane = entry->lanes[analysis][input_transition][output_transition];
            if (lane) {
              appendOptional(result.arcs.timing_arc_delays, std::optional<double>{lane->delay});
              appendOptional(result.arcs.timing_arc_fallbacks, std::optional<std::uint8_t>{lane->is_fallback ? 1U : 0U});
            } else {
              appendOptional(result.arcs.timing_arc_delays, std::optional<double>{});
              appendOptional(result.arcs.timing_arc_fallbacks, std::optional<std::uint8_t>{});
            }
          }
        }
      }
      ++child_count;
    }
    result.arcs.timing_arc_row_offsets[arc + 1] = child_count;
  }
}

}  // namespace

CsrTimingResult materializeTimingResultFromNativeOracle(const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle)
{
  if (oracle.schema != kNativeTimingOracleSchema || oracle.nodes.size() != input.nodes.size() || oracle.arcs.size() != input.arcs.size()) {
    throw std::runtime_error("timing scatter materializer: oracle identity or cardinality mismatch");
  }

  CsrTimingResult result;
  result.schema_version = input.schema_version;
  result.source_identity = input.source_identity;
  materializeNodes(input, oracle, result);
  materializePaths(input, oracle, result);
  materializeArcs(input, oracle, result);
  return result;
}

}  // namespace ista::csr::test
