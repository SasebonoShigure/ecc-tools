#include "CsrCudaPacking.hpp"

#include <stdexcept>
#include <utility>

namespace ista::csr::cuda_detail {
namespace {

template <class T>
void append(OptionalSoa<T>& result, const T& value, bool present = true)
{
  result.presence.push_back(present ? 1 : 0);
  result.values.push_back(present ? value : T{});
}

void appendTransition(OptionalSoa<Transition>& result, std::uint8_t value, bool present)
{
  result.presence.push_back(present ? 1 : 0);
  result.values.push_back(present ? static_cast<Transition>(value) : Transition::kNone);
}

void unpackNodes(DownloadData& data, const ResultLayout& layout, CsrTimingValues& result)
{

  for (std::size_t i = 0; i < data.nodes.size(); ++i) {
    const value_detail::NodeValue& node = data.nodes[i];
    result.nodes.is_clock_point.values[i] = node.clock_point;
    result.nodes.arrivals.values[i] = node.arrival;
    result.nodes.requireds.values[i] = node.required;
    result.nodes.slacks.values[i] = node.slack;
    result.nodes.launches.values[i] = node.launch;
    if (node.clock_name != absent && node.clock_name >= layout.clock_names.size())
      throw std::runtime_error("CUDA returned invalid clock-name index");
    result.nodes.clock_names.presence[i] = node.clock_name == absent ? 0 : 1;
    result.nodes.clock_names.values[i] = node.clock_name == absent ? std::string{} : layout.clock_names[node.clock_name];
    result.nodes.predecessor_nodes.presence[i] = node.predecessor_node == absent ? 0 : 1;
    result.nodes.predecessor_nodes.values[i] = NodeId{node.predecessor_node == absent ? 0 : node.predecessor_node};
    result.nodes.predecessor_arcs.presence[i] = node.predecessor_arc == absent ? 0 : 1;
    result.nodes.predecessor_arcs.values[i] = ArcId{node.predecessor_arc == absent ? 0 : node.predecessor_arc};
    for (std::size_t lane = 0; lane < 4; ++lane) {
      result.data_slews.presence[i * 4 + lane] = node.slew[lane].present ? 1 : 0;
      result.data_slews.values[i * 4 + lane] = node.slew[lane].present ? node.slew[lane].value : 0.0;
      const auto& clock = node.physical_clock[lane];
      append(result.effective_clock.arrivals, node.propagated_clock ? clock.arrival.value : 0.0,
             node.propagated_clock ? clock.arrival.present : node.clock_point);
      append(result.effective_clock.slews, node.propagated_clock ? clock.slew.value : 0.0,
             node.propagated_clock ? clock.slew.present : node.clock_point);
      const bool effective_predecessor = node.propagated_clock && clock.predecessor != absent;
      append(result.effective_clock.predecessor_nodes, NodeId{effective_predecessor ? clock.predecessor : 0}, effective_predecessor);
      append(result.effective_clock.predecessor_arc_delays, clock.predecessor_delay, effective_predecessor);
      appendTransition(result.effective_clock.predecessor_transitions, clock.predecessor_transition, effective_predecessor);
      append(result.physical_clock.arrivals, clock.arrival.value, clock.arrival.present);
      append(result.physical_clock.slews, clock.slew.value, clock.slew.present);
      const bool predecessor = clock.predecessor != absent;
      append(result.physical_clock.predecessor_nodes, NodeId{predecessor ? clock.predecessor : 0}, predecessor);
      append(result.physical_clock.predecessor_arc_delays, clock.predecessor_delay, predecessor);
      appendTransition(result.physical_clock.predecessor_transitions, clock.predecessor_transition, predecessor);
    }
  }
}

void unpackPaths(DownloadData& data, const PathDomain& domain, CsrTimingValues& result)
{
  auto& paths = result.paths;
  paths.row_offsets.clear();
  const auto row_count = data.nodes.size() * 8;
  paths.row_offsets.reserve(row_count + 1);
  for (std::size_t row = 0; row < row_count; ++row) {
    paths.row_offsets.push_back(paths.arrivals.values.size());
    for (Index slot = domain.rows[row].begin; slot < domain.rows[row].end; ++slot) {
      const Index tag = domain.tags[slot];
      const auto& path = data.paths[slot];
      if (!path.present)
        continue;
      append(paths.clock_tags, ClockTagId{tag});
      append(paths.arrivals, path.arrival);
      append(paths.slews, path.slew);
      append(paths.launches, path.launch);
      append(paths.start_nodes, NodeId{path.start});
      const bool predecessor = path.predecessor_node != absent;
      append(paths.predecessor_nodes, NodeId{predecessor ? path.predecessor_node : 0}, predecessor);
      append(paths.predecessor_arcs, ArcId{predecessor ? path.predecessor_arc : 0}, predecessor);
      append(paths.predecessor_arc_delays, path.predecessor_delay, predecessor);
      appendTransition(paths.predecessor_transitions, path.predecessor_transition, predecessor);
      append(paths.crpr_nodes, NodeId{path.crpr_node == absent ? 0 : path.crpr_node}, path.crpr_node != absent);
      appendTransition(paths.crpr_transitions, path.crpr_transition, path.crpr_node != absent);
    }
  }
  paths.row_offsets.push_back(paths.arrivals.values.size());
}

void unpackArcs(DownloadData& data, const ResultLayout& layout, CsrTimingValues& timing)
{
  auto& result = timing.arcs;
  result.timing_arc_row_offsets.push_back(0);
  for (std::size_t id = 0; id < layout.arcs.size(); ++id) {
    const auto& arc = layout.arcs[id];
    const auto& state = data.arc_state[id];
    append(result.delays, state.delay, arc.active);
    append(result.min_delays, state.delay_min, arc.active);
    append(result.max_delays, state.delay_max, arc.active);
    for (std::size_t lane = 0; lane < 4; ++lane)
      append(result.transition_delays, state.transition[lane], arc.active);
    for (std::size_t lane = 0; lane < 8; ++lane) {
      append(result.input_output_delays, state.io[lane].value, state.io[lane].present);
      append(result.graph_delays, state.graph[lane].value, state.graph[lane].present);
    }
    for (std::size_t lane = 0; lane < 2; ++lane)
      appendTransition(result.transition_types, state.type[lane].value, state.type[lane].present);
    for (Index child = arc.child_begin; child < arc.child_begin + arc.child_count; ++child) {
      result.timing_arc_indices.push_back(layout.child_indices[child]);
      const auto& cache = data.child_state[child];
      for (std::size_t lane = 0; lane < 8; ++lane) {
        append(result.timing_arc_delays, cache.delay[lane].value, cache.delay[lane].present);
        append(result.timing_arc_fallbacks, cache.fallback[lane], cache.delay[lane].present);
      }
    }
    result.timing_arc_row_offsets.push_back(result.timing_arc_indices.size());
  }
}

}  // namespace

CsrTimingValues unpackValues(DownloadData&& data, const ResultLayout& layout, const PathDomain& domain)
{
  CsrTimingValues result;
  const auto count = data.nodes.size();
  auto initialize = [count]<class T>(OptionalSoa<T>& soa, bool present) {
    soa.values.resize(count);
    soa.presence.assign(count, present ? 1 : 0);
  };
  initialize(result.nodes.is_clock_point, true);
  initialize(result.nodes.arrivals, true);
  initialize(result.nodes.requireds, true);
  initialize(result.nodes.slacks, true);
  initialize(result.nodes.launches, true);
  initialize(result.nodes.clock_names, false);
  initialize(result.nodes.predecessor_nodes, false);
  initialize(result.nodes.predecessor_arcs, false);
  result.data_slews.values.resize(count * 4);
  result.data_slews.presence.resize(count * 4);
  unpackNodes(data, layout, result);
  unpackPaths(data, domain, result);
  unpackArcs(data, layout, result);
  return result;
}

}  // namespace ista::csr::cuda_detail
