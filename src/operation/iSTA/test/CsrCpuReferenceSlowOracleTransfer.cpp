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
#include <vector>

#include "CsrCpuReferenceSlowOracleInternal.hpp"

namespace ista::csr::test::detail {
namespace {

template <typename T>
OptionalSoa<T> resetSoa(std::size_t size, const T& value = T{})
{
  return {std::vector<std::uint8_t>(size, 0), std::vector<T>(size, value)};
}

template <typename T>
OptionalSoa<T> requiredSoa(std::size_t size, const T& value)
{
  return {std::vector<std::uint8_t>(size, 1), std::vector<T>(size, value)};
}

bool childHasRelation(const ChildTimingArcModel& child, std::size_t output)
{
  return child.delay_tables[output].present || child.slew_tables[output].present || child.check_tables[output].present;
}

bool inputTriggered(const ArcRecord& arc, std::size_t input)
{
  if (arc.transfer.children().empty() || !arc.transfer.is_clock_arc)
    return true;
  const Transition trigger = arc.transfer.children().front().signature.trigger_transition;
  if (trigger == Transition::kFall)
    return input == 1;
  return input == 0;
}

enum class RelationMode : std::uint8_t
{
  kIdentity,
  kFlip,
  kBoth
};

RelationMode relationMode(const ArcRecord& arc)
{
  bool negative = false;
  bool nonnegative = false;
  bool non_unate = false;
  for (const auto& child : arc.transfer.children()) {
    negative = negative || child.signature.sense == TimingSense::kNegativeUnate;
    non_unate = non_unate || child.signature.sense == TimingSense::kNonUnate;
    nonnegative = nonnegative || child.signature.sense == TimingSense::kPositiveUnate || child.signature.sense == TimingSense::kNoneUsesPositiveFallback;
  }
  if (arc.transfer.is_clock_arc || non_unate || (negative && nonnegative))
    return RelationMode::kBoth;
  return negative ? RelationMode::kFlip : RelationMode::kIdentity;
}

bool relationAllowed(RelationMode mode, std::size_t input, std::size_t output)
{
  if (mode == RelationMode::kBoth)
    return true;
  if (mode == RelationMode::kFlip)
    return input != output;
  return input == output;
}

bool childAllowed(const ChildTimingArcModel& child, std::size_t input, std::size_t output)
{
  const bool flip = input != output;
  if (child.signature.sense == TimingSense::kNonUnate)
    return true;
  if (child.signature.sense == TimingSense::kNegativeUnate)
    return flip;
  return !flip;
}

SlowOracleStatus initializeSimpleArc(const ArcRecord& arc, std::size_t arc_index, SlowOracleState& state)
{
  auto& result = state.result.arcs;
  const bool net = arc.kind == ArcKind::kNet;
  const bool empty_cell = arc.kind == ArcKind::kCell && arc.transfer.children().empty();
  const double maximum = net ? 0.0 : arc.transfer.delay_max;
  const double minimum = net ? 0.0 : arc.transfer.delay_min;
  result.delays.values[arc_index] = maximum;
  result.min_delays.values[arc_index] = minimum;
  result.max_delays.values[arc_index] = maximum;
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    const double delay = analysis == 0 ? maximum : minimum;
    for (std::size_t input = 0; input < kTransitionCount; ++input) {
      const std::size_t transition_lane = slowArcTransitionLane(arc_index, analysis, input);
      result.transition_delays.values[transition_lane] = delay;
      const std::size_t io_lane = slowArcIoLane(arc_index, analysis, input, input);
      result.input_output_delays.presence[io_lane] = 1;
      result.input_output_delays.values[io_lane] = delay;
      result.transition_types.presence[arc_index * kTransitionCount + input] = 1;
      result.transition_types.values[arc_index * kTransitionCount + input] = input == 0 ? Transition::kRise : Transition::kFall;
    }
  }
  return net || empty_cell ? SlowOracleStatus::kSuccess : SlowOracleStatus::kInvariant;
}

SlowOracleStatus initializeChildArc(const ArcRecord& arc, std::size_t arc_index, SlowOracleState& state)
{
  auto& result = state.result.arcs;
  const RelationMode mode = relationMode(arc);
  std::array<std::array<bool, kTransitionCount>, kAnalysisCount> transition_written{};
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
      if (!inputTriggered(arc, input_transition))
        continue;
      for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
        if (!relationAllowed(mode, input_transition, output_transition))
          continue;
        bool relation = false;
        for (const auto& child : arc.transfer.children())
          relation = relation || childHasRelation(child, output_transition);
        if (!relation)
          continue;

        bool delay_written = false;
        double selected_delay = arc.transfer.delay;
        for (std::size_t child_index = 0; child_index < arc.transfer.children().size(); ++child_index) {
          const auto& child = arc.transfer.children()[child_index];
          if (!childAllowed(child, input_transition, output_transition) || !child.delay_tables[output_transition].present)
            continue;
          if (!arc.output_load)
            return SlowOracleStatus::kInvariant;
          const auto lookup = slowOracleLookupTable(child.delay_tables[output_transition], 0.0, (*arc.output_load)[analysis][output_transition],
                                                    child.time_unit_scale, child.capacitance_unit_scale);
          if (lookup.status != SlowOracleStatus::kSuccess)
            return lookup.status;
          const std::size_t global_child = result.timing_arc_row_offsets[arc_index] + child_index;
          const std::size_t cache_lane = slowChildLane(global_child, analysis, input_transition, output_transition);
          result.timing_arc_delays.presence[cache_lane] = 1;
          result.timing_arc_delays.values[cache_lane] = lookup.value;
          result.timing_arc_fallbacks.presence[cache_lane] = 1;
          result.timing_arc_fallbacks.values[cache_lane] = 1;
          if (!delay_written || (analysis == 0 ? lookup.value > selected_delay : lookup.value < selected_delay)) {
            selected_delay = lookup.value;
            delay_written = true;
          }
        }

        const std::size_t io_lane = slowArcIoLane(arc_index, analysis, input_transition, output_transition);
        result.input_output_delays.presence[io_lane] = 1;
        result.input_output_delays.values[io_lane] = selected_delay;
        const std::size_t transition_lane = slowArcTransitionLane(arc_index, analysis, input_transition);
        if (!transition_written[analysis][input_transition]
            || (analysis == 0 ? selected_delay > result.transition_delays.values[transition_lane]
                              : selected_delay < result.transition_delays.values[transition_lane])) {
          result.transition_delays.values[transition_lane] = selected_delay;
          result.transition_types.presence[arc_index * kTransitionCount + input_transition] = 1;
          result.transition_types.values[arc_index * kTransitionCount + input_transition] = output_transition == 0 ? Transition::kRise : Transition::kFall;
          transition_written[analysis][input_transition] = true;
        }
      }
    }
  }
  const double maximum = std::max(result.transition_delays.values[slowArcTransitionLane(arc_index, 0, 0)],
                                  result.transition_delays.values[slowArcTransitionLane(arc_index, 0, 1)]);
  const double minimum = std::min(result.transition_delays.values[slowArcTransitionLane(arc_index, 1, 0)],
                                  result.transition_delays.values[slowArcTransitionLane(arc_index, 1, 1)]);
  if (!std::isfinite(maximum) || !std::isfinite(minimum))
    return SlowOracleStatus::kNumeric;
  result.delays.values[arc_index] = maximum;
  result.min_delays.values[arc_index] = minimum;
  result.max_delays.values[arc_index] = maximum;
  return SlowOracleStatus::kSuccess;
}

}  // namespace

std::size_t slowNodeLane(std::size_t node, std::size_t analysis, std::size_t transition)
{
  return (node * kAnalysisCount + analysis) * kTransitionCount + transition;
}

std::size_t slowPathRow(std::size_t node, std::size_t analysis, std::size_t source, std::size_t transition)
{
  return ((node * kAnalysisCount + analysis) * kPathSourceCount + source) * kTransitionCount + transition;
}

std::size_t slowArcTransitionLane(std::size_t arc, std::size_t analysis, std::size_t input_transition)
{
  return (arc * kAnalysisCount + analysis) * kTransitionCount + input_transition;
}

std::size_t slowArcIoLane(std::size_t arc, std::size_t analysis, std::size_t input_transition, std::size_t output_transition)
{
  return slowArcTransitionLane(arc, analysis, input_transition) * kTransitionCount + output_transition;
}

std::size_t slowChildLane(std::size_t child, std::size_t analysis, std::size_t input_transition, std::size_t output_transition)
{
  return ((child * kAnalysisCount + analysis) * kTransitionCount + input_transition) * kTransitionCount + output_transition;
}

SlowOracleStatus slowOracleInitialize(const CsrReferenceInput& input, const SlowOracleLevelization& levelization, SlowOracleState& state)
{
  const std::size_t nodes = input.nodes.size();
  const std::size_t arcs = input.arcs.size();
  const std::size_t node_lanes = nodes * kAnalysisCount * kTransitionCount;
  const std::size_t path_rows = nodes * kAnalysisCount * kPathSourceCount * kTransitionCount;
  const std::size_t transition_lanes = arcs * kAnalysisCount * kTransitionCount;
  const std::size_t io_lanes = transition_lanes * kTransitionCount;

  state.result = {};
  state.result.source_identity = input.source_identity;
  state.result.level_offsets = levelization.level_offsets;
  state.result.level_nodes = levelization.level_nodes;
  state.result.node_to_level = levelization.node_to_level;
  state.result.nodes.is_clock_point = requiredSoa<std::uint8_t>(nodes, 0);
  state.result.nodes.arrivals = requiredSoa(nodes, -std::numeric_limits<double>::infinity());
  state.result.nodes.requireds = requiredSoa(nodes, std::numeric_limits<double>::infinity());
  state.result.nodes.slacks = requiredSoa(nodes, 0.0);
  state.result.nodes.launches = requiredSoa(nodes, 0.0);
  state.result.nodes.clock_names = resetSoa<std::string>(nodes);
  state.result.nodes.predecessor_nodes = resetSoa<NodeId>(nodes);
  state.result.nodes.predecessor_arcs = resetSoa<ArcId>(nodes);

  state.result.effective_clock.arrivals = resetSoa<double>(node_lanes);
  state.result.effective_clock.slews = resetSoa<double>(node_lanes);
  state.result.effective_clock.predecessor_nodes = resetSoa<NodeId>(node_lanes);
  state.result.effective_clock.predecessor_arc_delays = resetSoa<double>(node_lanes);
  state.result.effective_clock.predecessor_transitions = resetSoa(node_lanes, Transition::kNone);
  state.result.physical_clock = state.result.effective_clock;
  state.result.data_slews = resetSoa<double>(node_lanes);
  state.result.paths.row_offsets.assign(path_rows + 1, 0);
  state.path_rows.assign(path_rows, {});

  state.result.arcs.delays = requiredSoa(arcs, 0.0);
  state.result.arcs.min_delays = requiredSoa(arcs, 0.0);
  state.result.arcs.max_delays = requiredSoa(arcs, 0.0);
  state.result.arcs.transition_delays = requiredSoa(transition_lanes, 0.0);
  state.result.arcs.input_output_delays = resetSoa<double>(io_lanes);
  state.result.arcs.graph_delays = resetSoa<double>(io_lanes);
  state.result.arcs.transition_types = resetSoa(arcs * kTransitionCount, Transition::kNone);
  state.result.arcs.timing_arc_row_offsets = {0};
  for (const auto& arc : input.arcs) {
    for (const auto& child : arc.transfer.children())
      state.result.arcs.timing_arc_indices.push_back(child.signature.arc_index);
    state.result.arcs.timing_arc_row_offsets.push_back(state.result.arcs.timing_arc_indices.size());
  }
  const std::size_t child_lanes = state.result.arcs.timing_arc_indices.size() * kAnalysisCount * kTransitionCount * kTransitionCount;
  state.result.arcs.timing_arc_delays = resetSoa<double>(child_lanes);
  state.result.arcs.timing_arc_fallbacks = resetSoa<std::uint8_t>(child_lanes);
  state.dynamic_cache_seen.assign(child_lanes, 0);
  return slowOracleInitializeArcs(input, state);
}

SlowOracleStatus slowOracleInitializeArcs(const CsrReferenceInput& input, SlowOracleState& state)
{
  for (std::size_t arc_index = 0; arc_index < input.arcs.size(); ++arc_index) {
    const ArcRecord& arc = input.arcs[arc_index];
    const SlowOracleStatus status
        = arc.kind == ArcKind::kCell && !arc.transfer.children().empty() ? initializeChildArc(arc, arc_index, state) : initializeSimpleArc(arc, arc_index, state);
    if (status != SlowOracleStatus::kSuccess)
      return status;
  }
  return SlowOracleStatus::kSuccess;
}

}  // namespace ista::csr::test::detail
