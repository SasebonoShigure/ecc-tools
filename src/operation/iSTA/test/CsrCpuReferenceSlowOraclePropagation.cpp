// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>
#include <vector>

#include "CsrCpuReferenceSlowOracleInternal.hpp"

namespace ista::csr::test::detail {
namespace {

constexpr double kSlewEpsilon = 1e-6;
constexpr double kScalarEpsilon = 1e-9;

bool finite(double value)
{
  return std::isfinite(value);
}

std::vector<std::size_t> nativeArcOrder(const CsrReferenceInput& input)
{
  std::vector<std::size_t> order;
  order.reserve(input.arcs.size());
  for (std::size_t index = 0; index < input.arcs.size(); ++index)
    order.push_back(index);
  std::ranges::sort(order, [&](std::size_t lhs, std::size_t rhs) { return input.arcs[lhs].native_traversal_rank < input.arcs[rhs].native_traversal_rank; });
  return order;
}

std::optional<std::size_t> tagIndex(const CsrReferenceInput& input, const std::string& tag)
{
  for (std::size_t index = 0; index < input.clock_tags.size(); ++index)
    if (input.clock_tags[index] == tag)
      return index;
  return std::nullopt;
}

const ClockConstraintModel* directClock(const CsrReferenceInput& input, NodeId node)
{
  for (const auto& clock : input.clocks)
    for (const auto& source : clock.ordered_sources)
      if (source.resolved_node == node)
        return &clock;
  return nullptr;
}

bool better(double candidate, double current, std::size_t analysis, double epsilon)
{
  return analysis == 0 ? candidate > current + epsilon : candidate < current - epsilon;
}

bool childMatches(const ChildTimingArcModel& child, std::size_t input, std::size_t output)
{
  if (child.signature.sense == TimingSense::kNonUnate)
    return true;
  if (child.signature.sense == TimingSense::kNegativeUnate)
    return input != output;
  return input == output;
}

SlowOracleStatus seedClocksAndStarts(const CsrReferenceInput& input, SlowOracleState& state)
{
  auto& result = state.result;
  for (const auto& clock : input.clocks) {
    for (const auto& source : clock.ordered_sources) {
      if (!source.resolved_node)
        continue;
      const std::size_t node = source.resolved_node->value();
      if (node >= input.nodes.size())
        return SlowOracleStatus::kInvariant;
      result.nodes.is_clock_point.values[node] = 1;
      if (result.nodes.clock_names.presence[node] == 0) {
        result.nodes.clock_names.presence[node] = 1;
        result.nodes.clock_names.values[node] = clock.map_key;
      }
      for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
        for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
          const std::size_t lane = slowNodeLane(node, analysis, transition);
          result.effective_clock.arrivals.presence[lane] = 1;
          result.effective_clock.slews.presence[lane] = 1;
          result.physical_clock.arrivals.presence[lane] = 1;
          result.physical_clock.slews.presence[lane] = 1;
        }
    }
  }

  for (NodeId start : input.ordered_start_ids) {
    const auto seed = std::find_if(input.start_seeds.begin(), input.start_seeds.end(), [&](const StartSeedModel& value) { return value.seed_node == start; });
    if (seed == input.start_seeds.end())
      return SlowOracleStatus::kInvariant;
    const std::size_t node = start.value();
    const ClockConstraintModel* clock = directClock(input, start);
    result.nodes.arrivals.values[node] = seed->path_source ? seed->arrival[0][0].value_or(0.0) : 0.0;
    result.nodes.launches.values[node] = clock == nullptr ? 0.0 : clock->edges[0];
    result.nodes.clock_names.presence[node] = 1;
    result.nodes.clock_names.values[node] = seed->clock_tag;
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        const std::size_t lane = slowNodeLane(node, analysis, transition);
        if (seed->slew[analysis][transition]) {
          result.data_slews.presence[lane] = 1;
          result.data_slews.values[lane] = *seed->slew[analysis][transition];
        }
        if (!seed->path_source || !seed->arrival[analysis][transition] || !seed->slew[analysis][transition])
          continue;
        const auto tag = tagIndex(input, seed->clock_tag);
        if (!tag)
          return SlowOracleStatus::kInvariant;
        const std::size_t source = *seed->path_source == PathSource::kInput ? 0 : 1;
        const std::size_t row = slowPathRow(node, analysis, source, transition);
        double launch = 0.0;
        if (clock != nullptr)
          launch = analysis == 0 && transition == 1 ? clock->edges[1] : clock->edges[0];
        state.path_rows[row].push_back({ClockTagId{*tag}, *seed->arrival[analysis][transition], *seed->slew[analysis][transition], launch,
                                        seed->path_start_node, std::nullopt, std::nullopt, std::nullopt, std::nullopt, seed->crpr_node, seed->crpr_transition});
      }
  }
  return SlowOracleStatus::kSuccess;
}

SlowOracleStatus updateChildCache(const ArcRecord& arc, std::size_t arc_index, std::size_t child_index, std::size_t analysis, std::size_t input_transition,
                                  std::size_t output_transition, double delay, SlowOracleState& state)
{
  const std::size_t child = state.result.arcs.timing_arc_row_offsets[arc_index] + child_index;
  if (child >= state.result.arcs.timing_arc_indices.size())
    return SlowOracleStatus::kInvariant;
  const std::size_t lane = slowChildLane(child, analysis, input_transition, output_transition);
  auto& delays = state.result.arcs.timing_arc_delays;
  auto& fallbacks = state.result.arcs.timing_arc_fallbacks;
  if (state.dynamic_cache_seen[lane] == 0 || better(delay, delays.values[lane], analysis, 0.0))
    delays.values[lane] = delay;
  delays.presence[lane] = 1;
  fallbacks.presence[lane] = 1;
  fallbacks.values[lane] = 0;
  state.dynamic_cache_seen[lane] = 1;
  (void) arc;
  return SlowOracleStatus::kSuccess;
}

SlowOracleStatus dynamicArc(const CsrReferenceInput& input, std::size_t arc_index, std::size_t analysis, std::size_t input_transition,
                            std::size_t output_transition, double source_slew, double& delay, double& slew, SlowOracleState& state)
{
  const ArcRecord& arc = input.arcs[arc_index];
  const std::size_t io_lane = slowArcIoLane(arc_index, analysis, input_transition, output_transition);
  if (state.result.arcs.input_output_delays.presence[io_lane] == 0)
    return SlowOracleStatus::kInvariant;
  if (arc.kind == ArcKind::kNet || arc.transfer.children.empty()) {
    delay = state.result.arcs.input_output_delays.values[io_lane];
    slew = source_slew;
    return SlowOracleStatus::kSuccess;
  }
  if (!arc.output_load)
    return SlowOracleStatus::kInvariant;

  bool delay_written = false;
  bool slew_written = false;
  delay = arc.transfer.delay;
  slew = source_slew;
  for (std::size_t child_index = 0; child_index < arc.transfer.children.size(); ++child_index) {
    const auto& child = arc.transfer.children[child_index];
    if (!childMatches(child, input_transition, output_transition))
      continue;
    if (child.delay_tables[output_transition].present) {
      const auto candidate = slowOracleLookupTable(child.delay_tables[output_transition], source_slew, (*arc.output_load)[analysis][output_transition],
                                                   child.time_unit_scale, child.capacitance_unit_scale);
      if (candidate.status != SlowOracleStatus::kSuccess)
        return candidate.status;
      const auto cache_status = updateChildCache(arc, arc_index, child_index, analysis, input_transition, output_transition, candidate.value, state);
      if (cache_status != SlowOracleStatus::kSuccess)
        return cache_status;
      if (!delay_written || better(candidate.value, delay, analysis, 0.0)) {
        delay = candidate.value;
        delay_written = true;
      }
    }
    if (child.slew_tables[output_transition].present) {
      const auto candidate = slowOracleLookupTable(child.slew_tables[output_transition], source_slew, (*arc.output_load)[analysis][output_transition],
                                                   child.time_unit_scale, child.capacitance_unit_scale);
      if (candidate.status != SlowOracleStatus::kSuccess)
        return candidate.status;
      if (!finite(candidate.value) || candidate.value < 0.0)
        return SlowOracleStatus::kNumeric;
      if (!slew_written || better(candidate.value, slew, analysis, 0.0)) {
        slew = candidate.value;
        slew_written = true;
      }
    }
  }
  return SlowOracleStatus::kSuccess;
}

SlowOracleStatus propagateSlewAndDelay(const CsrReferenceInput& input, SlowOracleState& state)
{
  auto& result = state.result;
  const std::vector<std::size_t> arc_order = nativeArcOrder(input);
  for (NodeId node : result.timing_order) {
    for (std::size_t arc_index : arc_order) {
      const ArcRecord& arc = input.arcs[arc_index];
      if (arc.source != node || !arc.eligibility.data_slew)
        continue;
      for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
        for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
          const std::size_t source_lane = slowNodeLane(arc.source.value(), analysis, input_transition);
          if (result.data_slews.presence[source_lane] == 0)
            continue;
          for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
            const std::size_t io_lane = slowArcIoLane(arc_index, analysis, input_transition, output_transition);
            if (result.arcs.input_output_delays.presence[io_lane] == 0)
              continue;
            double delay = 0.0;
            double slew = 0.0;
            const SlowOracleStatus status
                = dynamicArc(input, arc_index, analysis, input_transition, output_transition, result.data_slews.values[source_lane], delay, slew, state);
            if (status != SlowOracleStatus::kSuccess)
              return status;
            if (!finite(delay) || !finite(slew) || slew < 0.0)
              return SlowOracleStatus::kNumeric;
            if (result.arcs.graph_delays.presence[io_lane] == 0 || better(delay, result.arcs.graph_delays.values[io_lane], analysis, kSlewEpsilon)) {
              result.arcs.graph_delays.presence[io_lane] = 1;
              result.arcs.graph_delays.values[io_lane] = delay;
            }
            const std::size_t sink_lane = slowNodeLane(arc.sink.value(), analysis, output_transition);
            if (result.data_slews.presence[sink_lane] == 0 || better(slew, result.data_slews.values[sink_lane], analysis, kSlewEpsilon)) {
              result.data_slews.presence[sink_lane] = 1;
              result.data_slews.values[sink_lane] = slew;
            }
          }
        }
    }
  }
  return SlowOracleStatus::kSuccess;
}

SlowOracleStatus propagateScalarArc(const CsrReferenceInput& input, std::size_t arc_index, SlowOracleState& state)
{
  const ArcRecord& arc = input.arcs[arc_index];
  auto& nodes = state.result.nodes;
  const double source = nodes.arrivals.values[arc.source.value()];
  if (!finite(source))
    return SlowOracleStatus::kSuccess;
  const double candidate = source + state.result.arcs.delays.values[arc_index];
  if (!finite(candidate))
    return SlowOracleStatus::kNumeric;
  const std::size_t sink = arc.sink.value();
  if (!finite(nodes.arrivals.values[sink]) || candidate > nodes.arrivals.values[sink] + kScalarEpsilon) {
    nodes.arrivals.values[sink] = candidate;
    nodes.launches.values[sink] = nodes.launches.values[arc.source.value()];
    nodes.clock_names.presence[sink] = nodes.clock_names.presence[arc.source.value()];
    nodes.clock_names.values[sink] = nodes.clock_names.values[arc.source.value()];
    nodes.predecessor_nodes.presence[sink] = 1;
    nodes.predecessor_nodes.values[sink] = arc.source;
    nodes.predecessor_arcs.presence[sink] = 1;
    nodes.predecessor_arcs.values[sink] = arc.id;
  }
  return SlowOracleStatus::kSuccess;
}

SlowOracleStatus propagatePathArc(const CsrReferenceInput& input, std::size_t arc_index, SlowOracleState& state)
{
  const ArcRecord& arc = input.arcs[arc_index];
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition)
      for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
        const std::size_t io_lane = slowArcIoLane(arc_index, analysis, input_transition, output_transition);
        if (state.result.arcs.input_output_delays.presence[io_lane] == 0)
          continue;
        double delay = state.result.arcs.input_output_delays.values[io_lane];
        if (state.result.arcs.graph_delays.presence[io_lane] != 0)
          delay = state.result.arcs.graph_delays.values[io_lane];
        const std::size_t source_row = slowPathRow(arc.source.value(), analysis, 0, input_transition);
        const std::size_t sink_row = slowPathRow(arc.sink.value(), analysis, 0, output_transition);
        for (const SlowOraclePathEntry& source : state.path_rows[source_row]) {
          const double sum = source.arrival + delay;
          if (!finite(sum))
            return SlowOracleStatus::kNumeric;
          const double scaled = sum * 1e15;
          if (!finite(scaled))
            return SlowOracleStatus::kNumeric;
          const double arrival = std::round(scaled) / 1e15;
          if (!finite(arrival))
            return SlowOracleStatus::kNumeric;
          const std::size_t slew_lane = slowNodeLane(arc.sink.value(), analysis, output_transition);
          if (state.result.data_slews.presence[slew_lane] == 0)
            return SlowOracleStatus::kInvariant;
          SlowOraclePathEntry candidate{source.clock_tag,
                                        arrival,
                                        state.result.data_slews.values[slew_lane],
                                        source.launch,
                                        source.start_node,
                                        arc.source,
                                        arc.id,
                                        delay,
                                        input_transition == 0 ? Transition::kRise : Transition::kFall,
                                        source.crpr_node,
                                        source.crpr_transition};
          auto& row = state.path_rows[sink_row];
          auto found = std::lower_bound(row.begin(), row.end(), candidate.clock_tag,
                                        [](const SlowOraclePathEntry& entry, ClockTagId tag) { return entry.clock_tag.value() < tag.value(); });
          if (found == row.end() || found->clock_tag != candidate.clock_tag) {
            row.insert(found, candidate);
          } else if (better(candidate.arrival, found->arrival, analysis, kSlewEpsilon)) {
            *found = candidate;
          }
        }
      }
  return SlowOracleStatus::kSuccess;
}

SlowOracleStatus propagateArrivalAndPath(const CsrReferenceInput& input, SlowOracleState& state)
{
  const std::vector<std::size_t> arc_order = nativeArcOrder(input);
  for (NodeId node : state.result.timing_order)
    for (std::size_t arc : arc_order) {
      if (input.arcs[arc].source != node || !input.arcs[arc].eligibility.data_arrival)
        continue;
      const SlowOracleStatus scalar = propagateScalarArc(input, arc, state);
      if (scalar != SlowOracleStatus::kSuccess)
        return scalar;
      const SlowOracleStatus path = propagatePathArc(input, arc, state);
      if (path != SlowOracleStatus::kSuccess)
        return path;
    }
  return SlowOracleStatus::kSuccess;
}

SlowOracleStatus propagateRequired(const CsrReferenceInput& input, SlowOracleState& state)
{
  auto& result = state.result;
  double default_required = 0.0;
  for (NodeId endpoint : input.ordered_end_ids)
    if (finite(result.nodes.arrivals.values[endpoint.value()]))
      default_required = std::max(default_required, result.nodes.arrivals.values[endpoint.value()]);
  for (NodeId endpoint : input.ordered_end_ids)
    result.nodes.requireds.values[endpoint.value()] = default_required;
  for (const auto& endpoint : input.endpoint_required) {
    double required = default_required;
    if (endpoint.output_delay[0]) {
      required = endpoint.clock_period - *endpoint.output_delay[0];
      if (!finite(required))
        return SlowOracleStatus::kNumeric;
    }
    result.nodes.requireds.values[endpoint.node.value()] = required;
  }

  const std::vector<std::size_t> arc_order = nativeArcOrder(input);
  for (auto node = result.timing_order.rbegin(); node != result.timing_order.rend(); ++node)
    for (std::size_t arc_index : arc_order) {
      const ArcRecord& arc = input.arcs[arc_index];
      if (arc.source != *node || !arc.eligibility.data_arrival)
        continue;
      const double sink = result.nodes.requireds.values[arc.sink.value()];
      if (!finite(sink))
        continue;
      const double candidate = sink - result.arcs.delays.values[arc_index];
      if (!finite(candidate))
        return SlowOracleStatus::kNumeric;
      result.nodes.requireds.values[arc.source.value()] = std::min(result.nodes.requireds.values[arc.source.value()], candidate);
    }

  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    const double arrival = result.nodes.arrivals.values[node];
    const double required = result.nodes.requireds.values[node];
    if (finite(arrival) && finite(required)) {
      const double slack = required - arrival;
      if (!finite(slack))
        return SlowOracleStatus::kNumeric;
      result.nodes.slacks.values[node] = slack;
    }
  }
  return SlowOracleStatus::kSuccess;
}

void flattenPaths(SlowOracleState& state)
{
  auto& paths = state.result.paths;
  std::size_t entries = 0;
  for (std::size_t row = 0; row < state.path_rows.size(); ++row) {
    entries += state.path_rows[row].size();
    paths.row_offsets[row + 1] = entries;
  }
  paths.clock_tags = {std::vector<std::uint8_t>(entries, 1), std::vector<ClockTagId>(entries)};
  paths.arrivals = {std::vector<std::uint8_t>(entries, 1), std::vector<double>(entries)};
  paths.slews = {std::vector<std::uint8_t>(entries, 1), std::vector<double>(entries)};
  paths.launches = {std::vector<std::uint8_t>(entries, 1), std::vector<double>(entries)};
  paths.start_nodes = {std::vector<std::uint8_t>(entries, 1), std::vector<NodeId>(entries)};
  paths.predecessor_nodes = {std::vector<std::uint8_t>(entries), std::vector<NodeId>(entries)};
  paths.predecessor_arcs = {std::vector<std::uint8_t>(entries), std::vector<ArcId>(entries)};
  paths.predecessor_arc_delays = {std::vector<std::uint8_t>(entries), std::vector<double>(entries)};
  paths.predecessor_transitions = {std::vector<std::uint8_t>(entries), std::vector<Transition>(entries, Transition::kNone)};
  paths.crpr_nodes = {std::vector<std::uint8_t>(entries), std::vector<NodeId>(entries)};
  paths.crpr_transitions = {std::vector<std::uint8_t>(entries), std::vector<Transition>(entries, Transition::kNone)};
  std::size_t flat = 0;
  for (const auto& row : state.path_rows)
    for (const auto& entry : row) {
      paths.clock_tags.values[flat] = entry.clock_tag;
      paths.arrivals.values[flat] = entry.arrival;
      paths.slews.values[flat] = entry.slew;
      paths.launches.values[flat] = entry.launch;
      paths.start_nodes.values[flat] = entry.start_node;
      if (entry.predecessor_node) {
        paths.predecessor_nodes.presence[flat] = 1;
        paths.predecessor_arcs.presence[flat] = 1;
        paths.predecessor_arc_delays.presence[flat] = 1;
        paths.predecessor_transitions.presence[flat] = 1;
        paths.predecessor_nodes.values[flat] = *entry.predecessor_node;
        paths.predecessor_arcs.values[flat] = *entry.predecessor_arc;
        paths.predecessor_arc_delays.values[flat] = *entry.predecessor_arc_delay;
        paths.predecessor_transitions.values[flat] = *entry.predecessor_transition;
      }
      if (entry.crpr_node) {
        paths.crpr_nodes.presence[flat] = 1;
        paths.crpr_transitions.presence[flat] = 1;
        paths.crpr_nodes.values[flat] = *entry.crpr_node;
        paths.crpr_transitions.values[flat] = *entry.crpr_transition;
      }
      ++flat;
    }
}

}  // namespace

SlowOracleLevelization slowOracleLevelize(const CsrReferenceInput& input)
{
  SlowOracleLevelization result;
  result.node_to_level.assign(input.nodes.size(), 0);
  std::vector<std::uint64_t> indegree(input.nodes.size(), 0);
  for (const auto& arc : input.arcs)
    if (arc.eligibility.levelization)
      ++indegree[arc.sink.value()];
  const std::vector<std::size_t> arc_order = nativeArcOrder(input);

  std::vector<NodeId> ready;
  for (std::size_t node = 0; node < input.nodes.size(); ++node)
    if (indegree[node] == 0)
      ready.emplace_back(node);
  const auto byte_less = [&](NodeId lhs, NodeId rhs) {
    const auto& left = input.nodes[lhs.value()].key.full_name;
    const auto& right = input.nodes[rhs.value()].key.full_name;
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                        [](char a, char b) { return static_cast<unsigned char>(a) < static_cast<unsigned char>(b); });
  };
  std::ranges::sort(ready, byte_less);
  for (NodeId root : ready)
    result.node_to_level[root.value()] = 1;

  for (std::size_t head = 0; head < ready.size(); ++head) {
    const NodeId node = ready[head];
    result.level_nodes.push_back(node);
    for (std::size_t arc_index : arc_order) {
      const ArcRecord& arc = input.arcs[arc_index];
      if (!arc.eligibility.levelization || arc.source != node)
        continue;
      result.node_to_level[arc.sink.value()] = std::max(result.node_to_level[arc.sink.value()], result.node_to_level[node.value()] + 1);
      if (--indegree[arc.sink.value()] == 0)
        ready.push_back(arc.sink);
    }
  }
  if (!result.level_nodes.empty()) {
    std::uint64_t level = result.node_to_level[result.level_nodes.front().value()];
    for (std::size_t index = 1; index < result.level_nodes.size(); ++index) {
      const std::uint64_t next = result.node_to_level[result.level_nodes[index].value()];
      if (next != level) {
        result.level_offsets.push_back(index);
        level = next;
      }
    }
    result.level_offsets.push_back(result.level_nodes.size());
  }
  result.complete = result.level_nodes.size() == input.nodes.size();
  return result;
}

SlowOracleStatus slowOraclePropagate(const CsrReferenceInput& input, SlowOracleState& state)
{
  SlowOracleStatus status = seedClocksAndStarts(input, state);
  if (status == SlowOracleStatus::kSuccess)
    status = propagateSlewAndDelay(input, state);
  if (status == SlowOracleStatus::kSuccess)
    status = propagateArrivalAndPath(input, state);
  if (status == SlowOracleStatus::kSuccess)
    status = propagateRequired(input, state);
  if (status == SlowOracleStatus::kSuccess)
    flattenPaths(state);
  return status;
}

}  // namespace ista::csr::test::detail

namespace ista::csr::test {

CsrComputationResult computeCsrCpuReferenceSlowOracle(const CsrReferenceInput& input)
{
  if (!input.unsupported_reasons.empty())
    return {CsrReferenceSupportResult{input.unsupported_reasons}};
  const detail::SlowOracleLevelization levelization = detail::slowOracleLevelize(input);
  if (!levelization.complete)
    return {CsrComputationCode::kCycle};
  detail::SlowOracleState state;
  detail::SlowOracleStatus status = detail::slowOracleInitialize(input, levelization, state);
  if (status == detail::SlowOracleStatus::kSuccess)
    status = detail::slowOraclePropagate(input, state);
  if (status == detail::SlowOracleStatus::kSuccess)
    return {std::move(state.result)};
  if (status == detail::SlowOracleStatus::kNumeric)
    return {CsrComputationCode::kNumericFailure};
  if (status == detail::SlowOracleStatus::kCycle)
    return {CsrComputationCode::kCycle};
  return {CsrComputationCode::kInvariantViolation};
}

}  // namespace ista::csr::test
