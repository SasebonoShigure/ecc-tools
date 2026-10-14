#include "CudaTargetedTimingProjection.hpp"

#include <map>
#include <stdexcept>

namespace ista::csr::test {
namespace {
void require(bool value, const char* message)
{
  if (!value)
    throw std::runtime_error(message);
}

template <class T>
void appendLane(OptionalSoa<T>& target, const OptionalSoa<T>& source, std::size_t row, std::size_t lanes = 1)
{
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    target.presence.push_back(source.presence.at(row * lanes + lane));
    target.values.push_back(source.values.at(row * lanes + lane));
  }
}

void appendClock(ClockResultSoa& target, const ClockResultSoa& source, std::size_t node)
{
  appendLane(target.arrivals, source.arrivals, node, 4);
  appendLane(target.slews, source.slews, node, 4);
  appendLane(target.predecessor_nodes, source.predecessor_nodes, node, 4);
  appendLane(target.predecessor_arc_delays, source.predecessor_arc_delays, node, 4);
  appendLane(target.predecessor_transitions, source.predecessor_transitions, node, 4);
}

void appendPath(PathResultSoa& target, const PathResultSoa& source, std::size_t entry)
{
  appendLane(target.clock_tags, source.clock_tags, entry);
  appendLane(target.arrivals, source.arrivals, entry);
  appendLane(target.slews, source.slews, entry);
  appendLane(target.launches, source.launches, entry);
  appendLane(target.start_nodes, source.start_nodes, entry);
  appendLane(target.predecessor_nodes, source.predecessor_nodes, entry);
  appendLane(target.predecessor_arcs, source.predecessor_arcs, entry);
  appendLane(target.predecessor_arc_delays, source.predecessor_arc_delays, entry);
  appendLane(target.predecessor_transitions, source.predecessor_transitions, entry);
  appendLane(target.crpr_nodes, source.crpr_nodes, entry);
  appendLane(target.crpr_transitions, source.crpr_transitions, entry);
}

template <class Id>
void remap(OptionalSoa<Id>& values, const std::vector<std::optional<Id>>& mapping)
{
  for (std::size_t index = 0; index < values.values.size(); ++index) {
    if (values.presence.at(index) == 0)
      continue;
    const auto& target = mapping.at(values.values[index].value());
    require(target.has_value(), "CUDA diagnostic predecessor references inactive ID");
    values.values[index] = *target;
  }
}
}  // namespace

CsrTimingResult projectCudaTargetedTiming(const CsrReferenceInput& input, const CsrTimingValues& values, const std::vector<std::string>& stable_names,
                                          const std::vector<std::optional<std::size_t>>& stable_ranks)
{
  require(input.nodes.size() == stable_names.size(), "CUDA diagnostic node mapping has wrong size");
  std::map<std::string, std::size_t> nodes_by_name;
  for (std::size_t node = 0; node < stable_names.size(); ++node)
    require(nodes_by_name.emplace(stable_names[node], node).second, "CUDA diagnostic node mapping repeats a name");
  std::map<std::size_t, std::size_t> arcs_by_rank;
  for (std::size_t arc = 0; arc < stable_ranks.size(); ++arc)
    if (stable_ranks[arc])
      require(arcs_by_rank.emplace(*stable_ranks[arc], arc).second, "CUDA diagnostic arc mapping repeats a native rank");
  require(arcs_by_rank.size() == input.arcs.size(), "CUDA diagnostic active arc mapping has wrong size");
  std::vector<std::optional<NodeId>> node_map(stable_names.size());
  std::vector<std::optional<ArcId>> arc_map(stable_ranks.size());
  CsrTimingResult result;
  result.source_identity = input.source_identity;
  for (const auto& node : input.nodes) {
    const auto old = nodes_by_name.at(node.key.full_name);
    node_map[old] = node.id;
    result.node_to_level.push_back(values.node_to_level.at(old));
    appendLane(result.nodes.is_clock_point, values.nodes.is_clock_point, old);
    appendLane(result.nodes.arrivals, values.nodes.arrivals, old);
    appendLane(result.nodes.requireds, values.nodes.requireds, old);
    appendLane(result.nodes.slacks, values.nodes.slacks, old);
    appendLane(result.nodes.launches, values.nodes.launches, old);
    appendLane(result.nodes.clock_names, values.nodes.clock_names, old);
    appendLane(result.nodes.predecessor_nodes, values.nodes.predecessor_nodes, old);
    appendLane(result.nodes.predecessor_arcs, values.nodes.predecessor_arcs, old);
    appendClock(result.effective_clock, values.effective_clock, old);
    appendClock(result.physical_clock, values.physical_clock, old);
    appendLane(result.data_slews, values.data_slews, old, 4);
    for (std::size_t lane = 0; lane < 8; ++lane) {
      result.paths.row_offsets.push_back(result.paths.arrivals.values.size());
      const auto begin = values.paths.row_offsets.at(old * 8 + lane);
      const auto end = values.paths.row_offsets.at(old * 8 + lane + 1);
      require(begin <= end && end <= values.paths.arrivals.values.size(), "CUDA diagnostic path row is invalid");
      for (auto entry = begin; entry < end; ++entry)
        appendPath(result.paths, values.paths, entry);
    }
  }
  result.paths.row_offsets.push_back(result.paths.arrivals.values.size());
  result.arcs.timing_arc_row_offsets.push_back(0);
  for (const auto& arc : input.arcs) {
    const auto old = arcs_by_rank.at(arc.native_traversal_rank);
    arc_map[old] = arc.id;
    appendLane(result.arcs.delays, values.arcs.delays, old);
    appendLane(result.arcs.min_delays, values.arcs.min_delays, old);
    appendLane(result.arcs.max_delays, values.arcs.max_delays, old);
    appendLane(result.arcs.transition_delays, values.arcs.transition_delays, old, 4);
    appendLane(result.arcs.input_output_delays, values.arcs.input_output_delays, old, 8);
    appendLane(result.arcs.graph_delays, values.arcs.graph_delays, old, 8);
    appendLane(result.arcs.transition_types, values.arcs.transition_types, old, 2);
    const auto begin = values.arcs.timing_arc_row_offsets.at(old);
    const auto end = values.arcs.timing_arc_row_offsets.at(old + 1);
    require(begin <= end && end <= values.arcs.timing_arc_indices.size(), "CUDA diagnostic child row is invalid");
    for (auto child = begin; child < end; ++child) {
      result.arcs.timing_arc_indices.push_back(values.arcs.timing_arc_indices[child]);
      appendLane(result.arcs.timing_arc_delays, values.arcs.timing_arc_delays, child, 8);
      appendLane(result.arcs.timing_arc_fallbacks, values.arcs.timing_arc_fallbacks, child, 8);
    }
    result.arcs.timing_arc_row_offsets.push_back(result.arcs.timing_arc_indices.size());
  }
  result.level_offsets = values.level_offsets;
  for (const auto node : values.level_nodes)
    result.level_nodes.push_back(node_map.at(node.value()).value());
  remap(result.nodes.predecessor_nodes, node_map);
  remap(result.nodes.predecessor_arcs, arc_map);
  remap(result.effective_clock.predecessor_nodes, node_map);
  remap(result.physical_clock.predecessor_nodes, node_map);
  remap(result.paths.start_nodes, node_map);
  remap(result.paths.predecessor_nodes, node_map);
  remap(result.paths.predecessor_arcs, arc_map);
  remap(result.paths.crpr_nodes, node_map);
  return result;
}
}  // namespace ista::csr::test
