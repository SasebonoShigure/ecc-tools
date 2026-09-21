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
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include <cstddef>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::detail {
namespace {

struct ByteStringComparator
{
  bool operator()(const std::string& lhs, const std::string& rhs) const { return referenceByteStringLess(lhs, rhs); }
};

struct NativeRoleIndex
{
  std::vector<std::optional<std::size_t>> instance_by_node;
  std::vector<std::optional<NodeId>> clock_node_by_instance;
  std::vector<std::optional<NodeId>> output_node_by_instance;
};

bool isInputLike(PinDirection direction)
{
  return direction == PinDirection::kInput || direction == PinDirection::kInout;
}

bool isOutputLike(PinDirection direction)
{
  return direction == PinDirection::kOutput || direction == PinDirection::kInout;
}

const ReferencedLibraryCellModel& requireReferencedCell(const ReferenceBuildState& state, const std::string& cell_name)
{
  const auto found = state.referenced_cell_indices_by_name.find(cell_name);
  if (found == state.referenced_cell_indices_by_name.end() || found->second >= state.input.referenced_library_cells.size())
    malformedReferenceInput();
  return state.input.referenced_library_cells[found->second];
}

bool hasOutputRole(const ReferencedLibraryCellModel& cell)
{
  if (!cell.present)
    return false;
  if (std::ranges::any_of(cell.cell_arcs, [](const LibraryCellArcModel& arc) { return arc.transfer.is_clock_arc; }))
    return true;
  return std::ranges::any_of(cell.ports, [](const LibraryPortModel& port) { return port.is_output && !port.is_clock; });
}

std::string instanceRoleFullName(const InstanceRecord& instance, bool role_present, const std::string& local_name)
{
  if (!role_present)
    return {};
  return instance.instance_name + ":" + local_name;
}

std::optional<NodeId> resolveInstanceRole(const ReferenceBuildState& state, const InstanceRecord& instance, bool role_present, const std::string& local_name)
{
  if (!role_present || local_name.empty())
    return std::nullopt;
  const std::string full_name = instanceRoleFullName(instance, role_present, local_name);
  const auto resolved = resolveNode(state, full_name);
  if (!resolved)
    return std::nullopt;
  const NodeRecord& node = requireNode(state, *resolved);
  if (node.is_port || node.key.instance_name != instance.instance_name || node.key.pin_name != local_name)
    malformedReferenceInput();
  return resolved;
}

NativeRoleIndex buildNativeRoleIndex(const ReferenceBuildState& state)
{
  NativeRoleIndex roles{std::vector<std::optional<std::size_t>>(state.input.nodes.size()), std::vector<std::optional<NodeId>>(state.input.instances.size()),
                        std::vector<std::optional<NodeId>>(state.input.instances.size())};

  for (std::size_t instance_index = 0; instance_index < state.input.instances.size(); ++instance_index) {
    const InstanceRecord& instance = state.input.instances[instance_index];
    const ista::Instance& native_instance = requireNativeInstance(state, instance.instance_name);
    const ReferencedLibraryCellModel& cell = requireReferencedCell(state, instance.cell_name);
    if (native_instance.get_is_sequential() != instance.is_sequential || native_instance.get_is_clock_gating() != instance.is_clock_gating)
      malformedReferenceInput();

    const bool timing_check_role_present = cell.present && !cell.timing_checks.empty();
    const bool output_role_present = hasOutputRole(cell);
    const auto clock_node = resolveInstanceRole(state, instance, timing_check_role_present, cell.clock_pin_name);
    const auto data_node = resolveInstanceRole(state, instance, timing_check_role_present, cell.data_pin_name);
    const auto output_node = resolveInstanceRole(state, instance, output_role_present, cell.output_pin_name);
    const std::string expected_clock = instanceRoleFullName(instance, timing_check_role_present, cell.clock_pin_name);
    const std::string expected_data = instanceRoleFullName(instance, timing_check_role_present, cell.data_pin_name);
    const std::string expected_output = instanceRoleFullName(instance, output_role_present, cell.output_pin_name);
    if (native_instance.get_clock_pin_name() != expected_clock || native_instance.get_data_pin_name() != expected_data
        || native_instance.get_output_pin_name() != expected_output)
      malformedReferenceInput();

    roles.clock_node_by_instance[instance_index] = clock_node;
    roles.output_node_by_instance[instance_index] = output_node;
    for (NodeId pin : instance.ordered_pins) {
      if (pin.value() >= roles.instance_by_node.size() || roles.instance_by_node[pin.value()])
        malformedReferenceInput();
      const NodeRecord& node = requireNode(state, pin);
      if (node.is_port || node.key.instance_name != instance.instance_name)
        malformedReferenceInput();
      const auto library_port = std::ranges::find(cell.ports, node.key.pin_name, &LibraryPortModel::name);
      if (library_port != cell.ports.end() && library_port->direction != node.direction)
        malformedReferenceInput();
      roles.instance_by_node[pin.value()] = instance_index;
    }
    for (const auto& role : {clock_node, data_node, output_node})
      if (role && roles.instance_by_node[role->value()] != instance_index)
        malformedReferenceInput();
  }
  return roles;
}

const PortConstraintModel* findPortConstraint(const ReferenceBuildState& state, NodeId node)
{
  if (node.value() >= state.port_constraint_indices_by_node.size())
    malformedReferenceInput();
  const auto index = state.port_constraint_indices_by_node[node.value()];
  if (!index)
    return nullptr;
  if (*index >= state.input.port_constraints.size())
    malformedReferenceInput();
  return &state.input.port_constraints[*index];
}

std::optional<std::size_t> findDirectClock(const ReferenceBuildState& state, NodeId node)
{
  for (std::size_t clock_index = 0; clock_index < state.input.clocks.size(); ++clock_index)
    for (const auto& source : state.input.clocks[clock_index].ordered_sources)
      if (source.resolved_node && *source.resolved_node == node)
        return clock_index;
  return std::nullopt;
}

bool isClockStopNode(const ReferenceBuildState& state, const NativeRoleIndex& roles, NodeId node)
{
  if (node.value() >= roles.instance_by_node.size() || !roles.instance_by_node[node.value()])
    return false;
  const InstanceRecord& instance = state.input.instances[*roles.instance_by_node[node.value()]];
  return instance.is_sequential && !instance.is_clock_gating && requireNode(state, node).is_clock_pin;
}

void replayClockReachability(ReferenceBuildState& state, const NativeRoleIndex& roles)
{
  const std::size_t node_count = state.input.nodes.size();
  const std::size_t arc_count = state.input.arcs.size();
  if (state.input.out_row_offsets.size() != node_count + 1)
    malformedReferenceInput();

  state.clock_owner_by_node.assign(node_count, std::nullopt);
  state.input.clock_owner_by_node.assign(node_count, std::nullopt);
  state.clock_reachable_nodes.assign(node_count, 0);
  state.replayed_clock_arcs.assign(arc_count, 0);

  for (std::size_t clock_index = 0; clock_index < state.input.clocks.size(); ++clock_index) {
    std::vector<std::uint8_t> visited(node_count, 0);
    std::queue<NodeId> ready;
    const auto visit = [&](NodeId node) {
      if (node.value() >= node_count)
        malformedReferenceInput();
      state.clock_reachable_nodes[node.value()] = 1;
      if (!state.clock_owner_by_node[node.value()]) {
        state.clock_owner_by_node[node.value()] = clock_index;
        state.input.clock_owner_by_node[node.value()] = state.input.clocks[clock_index].map_key;
      }
      if (visited[node.value()] == 0) {
        visited[node.value()] = 1;
        ready.push(node);
      }
    };

    for (const auto& source : state.input.clocks[clock_index].ordered_sources)
      if (source.resolved_node)
        visit(*source.resolved_node);

    while (!ready.empty()) {
      const NodeId node = ready.front();
      ready.pop();
      if (isClockStopNode(state, roles, node))
        continue;
      const std::uint64_t begin = state.input.out_row_offsets[node.value()];
      const std::uint64_t end = state.input.out_row_offsets[node.value() + 1];
      if (begin > end || end > state.input.out_arc_ids.size())
        malformedReferenceInput();
      for (std::uint64_t offset = begin; offset < end; ++offset) {
        const ArcId arc_id = state.input.out_arc_ids[offset];
        if (arc_id.value() >= arc_count)
          malformedReferenceInput();
        const ArcRecord& arc = state.input.arcs[arc_id.value()];
        if (arc.source != node || arc.sink.value() >= node_count)
          malformedReferenceInput();
        if (arc.is_disabled || arc.is_loop_disabled)
          continue;
        state.replayed_clock_arcs[arc_id.value()] = 1;
        visit(arc.sink);
      }
    }
  }
}

std::vector<std::size_t> fullNameOrder(const ReferenceBuildState& state)
{
  std::vector<std::size_t> order(state.input.nodes.size());
  std::iota(order.begin(), order.end(), 0);
  std::ranges::sort(order, [&](std::size_t lhs, std::size_t rhs) {
    return referenceByteStringLess(state.input.nodes[lhs].key.full_name, state.input.nodes[rhs].key.full_name);
  });
  return order;
}

void rebuildStartEndRoles(ReferenceBuildState& state, const NativeRoleIndex& roles)
{
  std::vector<std::uint8_t> has_incoming(state.input.nodes.size(), 0);
  std::vector<std::uint8_t> has_outgoing(state.input.nodes.size(), 0);
  for (const auto& arc : state.input.arcs) {
    if (arc.source.value() >= state.input.nodes.size() || arc.sink.value() >= state.input.nodes.size())
      malformedReferenceInput();
    has_outgoing[arc.source.value()] = 1;
    has_incoming[arc.sink.value()] = 1;
  }

  state.input.ordered_start_ids.clear();
  state.input.ordered_end_ids.clear();
  for (auto& node : state.input.nodes) {
    const bool input_port = node.is_port && isInputLike(node.direction);
    const bool output_port = node.is_port && isOutputLike(node.direction);
    bool non_gating_sequential_clock = false;
    if (!node.is_port && roles.instance_by_node[node.id.value()]) {
      const auto& instance = state.input.instances[*roles.instance_by_node[node.id.value()]];
      non_gating_sequential_clock = instance.is_sequential && !instance.is_clock_gating && node.is_clock_pin;
    }
    if (non_gating_sequential_clock) {
      node.is_startpoint = true;
    } else if (node.is_clock_pin) {
      node.is_startpoint = false;
    } else if (state.clock_source_nodes[node.id.value()] != 0) {
      node.is_startpoint = input_port;
    } else {
      node.is_startpoint = has_incoming[node.id.value()] == 0 || input_port;
    }
    node.is_endpoint = !node.is_clock_pin && state.clock_source_nodes[node.id.value()] == 0
                       && (has_outgoing[node.id.value()] == 0 || output_port || state.timing_check_data_nodes[node.id.value()] != 0);
  }

  for (std::size_t index : fullNameOrder(state)) {
    if (state.input.nodes[index].is_startpoint)
      state.input.ordered_start_ids.emplace_back(index);
    if (state.input.nodes[index].is_endpoint)
      state.input.ordered_end_ids.emplace_back(index);
  }
}

std::string resolveClockTag(const ReferenceBuildState& state, const NativeRoleIndex& roles, NodeId node)
{
  if (const auto direct = findDirectClock(state, node))
    return state.input.clocks[*direct].embedded_clock_name;
  if (node.value() < roles.instance_by_node.size() && roles.instance_by_node[node.value()]) {
    const std::size_t instance_index = *roles.instance_by_node[node.value()];
    if (state.input.instances[instance_index].is_sequential) {
      const auto clock_node = roles.clock_node_by_instance[instance_index];
      if (clock_node && state.clock_owner_by_node[clock_node->value()])
        return state.input.clocks[*state.clock_owner_by_node[clock_node->value()]].map_key;
    }
  }
  if (node.value() < state.clock_owner_by_node.size() && state.clock_owner_by_node[node.value()])
    return state.input.clocks[*state.clock_owner_by_node[node.value()]].map_key;
  const PortConstraintModel* constraint = findPortConstraint(state, node);
  if (constraint != nullptr && !constraint->clock_name.empty())
    return constraint->clock_name;
  if (!state.input.clocks.empty())
    return state.input.clocks.front().map_key;
  return "clk";
}

Transition selectedCrprTransition(const ReferencedLibraryCellModel& cell)
{
  const auto selected = std::ranges::find_if(cell.cell_arcs, [](const LibraryCellArcModel& arc) { return arc.transfer.is_clock_arc; });
  if (selected == cell.cell_arcs.end() || selected->transfer.children().empty())
    return Transition::kRise;
  const Transition trigger = selected->transfer.children().front().signature.trigger_transition;
  return trigger == Transition::kNone ? Transition::kRise : trigger;
}

void fillInputSeedValues(const ReferenceBuildState& state, NodeId node, const std::optional<std::size_t>& direct_clock, StartSeedModel& seed)
{
  const PortConstraintModel* constraint = findPortConstraint(state, node);
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      if (direct_clock) {
        const auto& clock = state.input.clocks[*direct_clock];
        seed.arrival[analysis][transition] = analysis == 0 && transition == 1 ? clock.edges[1] : clock.edges[0];
      } else if (analysis == 0) {
        seed.arrival[analysis][transition] = constraint && constraint->input_delay[0] ? *constraint->input_delay[0] : 0.0;
      } else {
        seed.arrival[analysis][transition] = constraint && constraint->input_delay[1]
                                                 ? *constraint->input_delay[1]
                                                 : (constraint && constraint->input_delay[0] ? *constraint->input_delay[0] : 0.0);
      }
      seed.slew[analysis][transition] = constraint && constraint->input_transition ? *constraint->input_transition : 0.0;
    }
  }
}

void fillNativeNoneSlew(const ReferenceBuildState& state, NodeId node, StartSeedModel& seed)
{
  const PortConstraintModel* constraint = findPortConstraint(state, node);
  const double slew = constraint && constraint->input_transition ? *constraint->input_transition : 0.0;
  for (auto& analysis : seed.slew)
    for (auto& transition : analysis)
      transition = slew;
}

void exportStartSeeds(ReferenceBuildState& state, const NativeRoleIndex& roles)
{
  state.input.start_seeds.clear();
  state.input.start_seeds.reserve(state.input.ordered_start_ids.size());
  for (NodeId node_id : state.input.ordered_start_ids) {
    const NodeRecord& node = requireNode(state, node_id);
    StartSeedModel seed;
    seed.seed_node = node_id;
    seed.path_start_node = node_id;
    seed.clock_tag = resolveClockTag(state, roles, node_id);

    const auto owner = roles.instance_by_node[node_id.value()];
    if (owner && state.input.instances[*owner].is_sequential) {
      seed.availability = StartSeedAvailability::kDeferredSequential;
      const auto clock_node = roles.clock_node_by_instance[*owner];
      const auto output_node = roles.output_node_by_instance[*owner];
      const bool reachable_clock = clock_node && state.clock_reachable_nodes[clock_node->value()] != 0;
      const bool selected_register_node = (clock_node && node_id == *clock_node) || (output_node && node_id == *output_node);
      if (reachable_clock && selected_register_node) {
        if (!output_node)
          malformedReferenceInput();
        seed.path_source = PathSource::kRegister;
        seed.path_start_node = node_id == *clock_node ? *output_node : node_id;
        seed.crpr_node = *clock_node;
        seed.crpr_transition = selectedCrprTransition(requireReferencedCell(state, state.input.instances[*owner].cell_name));
      }
    } else {
      const auto direct_clock = findDirectClock(state, node_id);
      const PortConstraintModel* constraint = findPortConstraint(state, node_id);
      const bool has_input_delay = constraint != nullptr && (constraint->input_delay[0].has_value() || constraint->input_delay[1].has_value());
      const bool input_seed = node.is_port && isInputLike(node.direction) && (direct_clock.has_value() || has_input_delay);
      if (input_seed) {
        seed.path_source = PathSource::kInput;
        fillInputSeedValues(state, node_id, direct_clock, seed);
      } else {
        fillNativeNoneSlew(state, node_id, seed);
      }
    }
    state.input.start_seeds.push_back(std::move(seed));
  }
}

void exportEndpointRequired(ReferenceBuildState& state, const NativeRoleIndex& roles)
{
  state.input.endpoint_required.clear();
  for (NodeId node_id : state.input.ordered_end_ids) {
    const NodeRecord& node = requireNode(state, node_id);
    if (!node.is_port || !isOutputLike(node.direction))
      continue;
    EndpointRequiredModel endpoint;
    endpoint.node = node_id;
    endpoint.clock_name = resolveClockTag(state, roles, node_id);
    const PortConstraintModel* constraint = findPortConstraint(state, node_id);
    if (constraint != nullptr)
      endpoint.output_delay = constraint->output_delay;
    const auto named = std::ranges::find(state.input.clocks, endpoint.clock_name, &ClockConstraintModel::map_key);
    const ClockConstraintModel* resolved = named != state.input.clocks.end() ? &*named : (state.input.clocks.empty() ? nullptr : &state.input.clocks.front());
    endpoint.clock_period = resolved == nullptr ? 0.0 : resolved->period;
    requireFiniteReference(endpoint.clock_period);
    state.input.endpoint_required.push_back(std::move(endpoint));
  }
}

void exportClockTags(ReferenceBuildState& state)
{
  std::set<std::string, ByteStringComparator> tags;
  const auto add = [&](const std::string& tag) {
    if (!tag.empty())
      tags.insert(tag);
  };
  for (const auto& clock : state.input.clocks) {
    add(clock.map_key);
    add(clock.embedded_clock_name);
  }
  for (const auto& constraint : state.input.port_constraints)
    add(constraint.clock_name);
  for (const auto& seed : state.input.start_seeds)
    add(seed.clock_tag);
  for (const auto& endpoint : state.input.endpoint_required)
    add(endpoint.clock_name);
  state.input.clock_tags.assign(tags.begin(), tags.end());
}

void exportEligibility(ReferenceBuildState& state)
{
  for (std::size_t index = 0; index < state.input.arcs.size(); ++index) {
    ArcRecord& arc = state.input.arcs[index];
    const NodeRecord& sink = requireNode(state, arc.sink);
    arc.eligibility
        = projectCanonicalArcEligibility(arc, sink, state.clock_reachable_nodes[arc.sink.value()] != 0, state.replayed_clock_arcs[index] != 0);
  }
}

}  // namespace

void exportReferenceClockSeeds(ReferenceBuildState& state)
{
  if (state.clock_source_nodes.size() != state.input.nodes.size() || state.timing_check_data_nodes.size() != state.input.nodes.size())
    malformedReferenceInput();
  const NativeRoleIndex roles = buildNativeRoleIndex(state);
  replayClockReachability(state, roles);
  rebuildStartEndRoles(state, roles);
  exportStartSeeds(state, roles);
  exportEndpointRequired(state, roles);
  exportClockTags(state);
  exportEligibility(state);
}

}  // namespace ista::csr::detail
