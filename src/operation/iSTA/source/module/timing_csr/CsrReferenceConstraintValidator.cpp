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
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferenceInputValidatorInternal.hpp"
#include "CsrReferenceValidatorDetail.hpp"

namespace ista::csr::detail {
namespace {

using NodeResolutionMap = std::map<std::string, NodeId>;

NodeResolutionMap makeNodeResolutionMap(const CsrReferenceInput& input)
{
  NodeResolutionMap result;
  for (const auto& node : input.nodes)
    result.emplace(node.key.full_name, node.id);
  return result;
}

std::optional<NodeId> resolveNode(const NodeResolutionMap& nodes, const std::string& full_name)
{
  const auto found = nodes.find(full_name);
  return found == nodes.end() ? std::nullopt : std::optional{found->second};
}

const InstanceRecord* findInstance(const CsrReferenceInput& input, const std::string& instance_name)
{
  const auto found = std::lower_bound(input.instances.begin(), input.instances.end(), instance_name,
                                      [](const InstanceRecord& instance, const std::string& name) { return byteStringLess(instance.instance_name, name); });
  return found != input.instances.end() && found->instance_name == instance_name ? &*found : nullptr;
}

const ReferencedLibraryCellModel* findCell(const CsrReferenceInput& input, const std::string& cell_name)
{
  const auto found = std::lower_bound(input.referenced_library_cells.begin(), input.referenced_library_cells.end(), cell_name,
                                      [](const ReferencedLibraryCellModel& cell, const std::string& name) { return byteStringLess(cell.cell_name, name); });
  return found != input.referenced_library_cells.end() && found->cell_name == cell_name ? &*found : nullptr;
}

std::optional<NodeId> findInstancePin(const CsrReferenceInput& input, const InstanceRecord& instance, const std::string& local_name)
{
  if (local_name.empty())
    return std::nullopt;
  for (NodeId pin : instance.ordered_pins)
    if (pin.value() < input.nodes.size() && input.nodes[pin.value()].key.pin_name == local_name)
      return pin;
  return std::nullopt;
}

void validateRawReferences(const CsrReferenceInput& input, const NodeResolutionMap& nodes, Violations& errors)
{
  for (std::size_t clock_index = 0; clock_index < input.clocks.size(); ++clock_index) {
    const auto& clock = input.clocks[clock_index];
    if (clock_index != 0 && !byteStringLess(input.clocks[clock_index - 1].map_key, clock.map_key))
      reject(errors, "clocks.map_key", "clocks must preserve strict native map-key byte order");
    for (const auto& source : clock.ordered_sources)
      if (source.resolved_node != resolveNode(nodes, source.full_name))
        reject(errors, "clocks.ordered_sources.resolved_node", "resolved source must exactly match the raw full name");
  }
  for (std::size_t index = 0; index < input.port_constraints.size(); ++index) {
    const auto& constraint = input.port_constraints[index];
    if (index != 0 && !byteStringLess(input.port_constraints[index - 1].map_key, constraint.map_key))
      reject(errors, "port_constraints.map_key", "port constraints must preserve strict native map-key byte order");
    const auto expected = resolveNode(nodes, constraint.map_key);
    if (constraint.resolved_node != expected)
      reject(errors, "port_constraints.resolved_node", "resolved port must exactly match the raw map key");
    if (constraint.resolved_node && (constraint.resolved_node->value() >= input.nodes.size() || !input.nodes[constraint.resolved_node->value()].is_port))
      reject(errors, "port_constraints.resolved_node", "resolved port constraint must reference a port");
  }
  for (std::size_t index = 0; index < input.case_analysis_constraints.size(); ++index) {
    const auto& constraint = input.case_analysis_constraints[index];
    if (index != 0 && !byteStringLess(input.case_analysis_constraints[index - 1].raw_name, constraint.raw_name))
      reject(errors, "case_analysis_constraints.raw_name", "case constraints must preserve strict native map-key byte order");
    if (constraint.resolved_node != resolveNode(nodes, constraint.raw_name))
      reject(errors, "case_analysis_constraints.resolved_node", "resolved case node must exactly match the raw name");
  }
}

struct ClockReplay
{
  std::vector<std::uint8_t> arcs;
  std::vector<std::optional<std::string>> owners;
  std::vector<std::uint8_t> direct_sources;
};

ClockReplay replayClocks(const CsrReferenceInput& input, Violations& errors)
{
  ClockReplay replay{std::vector<std::uint8_t>(input.arcs.size(), 0), std::vector<std::optional<std::string>>(input.nodes.size()),
                     std::vector<std::uint8_t>(input.nodes.size(), 0)};
  std::vector<std::uint8_t> stop(input.nodes.size(), 0);
  for (const auto& instance : input.instances)
    if (instance.is_sequential && !instance.is_clock_gating)
      for (NodeId pin : instance.ordered_pins)
        if (pin.value() < input.nodes.size() && input.nodes[pin.value()].is_clock_pin)
          stop[pin.value()] = 1;

  bool overlap_reported = false;
  for (const auto& clock : input.clocks) {
    std::vector<std::uint8_t> visited(input.nodes.size(), 0);
    std::queue<NodeId> ready;
    const auto visit = [&](NodeId node) {
      if (node.value() >= input.nodes.size())
        return;
      auto& owner = replay.owners[node.value()];
      if (!owner) {
        owner = clock.map_key;
      } else if (!overlap_reported && *owner != clock.map_key) {
        reject(errors, "clock_owner_by_node", "different clock map keys must not overlap in a materialized candidate");
        overlap_reported = true;
      }
      if (visited[node.value()] == 0) {
        visited[node.value()] = 1;
        ready.push(node);
      }
    };
    for (const auto& source : clock.ordered_sources) {
      if (!source.resolved_node)
        continue;
      if (source.resolved_node->value() < replay.direct_sources.size())
        replay.direct_sources[source.resolved_node->value()] = 1;
      visit(*source.resolved_node);
    }
    while (!ready.empty()) {
      const NodeId node = ready.front();
      ready.pop();
      if (stop[node.value()] != 0 || input.out_row_offsets.size() != input.nodes.size() + 1)
        continue;
      const std::uint64_t begin = input.out_row_offsets[node.value()];
      const std::uint64_t end = input.out_row_offsets[node.value() + 1];
      if (begin > end || end > input.out_arc_ids.size())
        continue;
      for (std::uint64_t offset = begin; offset < end; ++offset) {
        const std::uint64_t arc_id = input.out_arc_ids[offset].value();
        if (arc_id >= input.arcs.size())
          continue;
        const auto& arc = input.arcs[arc_id];
        if (arc.source != node || arc.sink.value() >= input.nodes.size() || arc.is_disabled || arc.is_loop_disabled)
          continue;
        replay.arcs[arc_id] = 1;
        visit(arc.sink);
      }
    }
  }
  if (input.clock_owner_by_node != replay.owners)
    reject(errors, "clock_owner_by_node", "dense clock-owner mirror differs from raw-constraint CSR replay");
  return replay;
}

void validateTimingChecks(const CsrReferenceInput& input, const NodeResolutionMap& nodes, std::vector<std::uint8_t>& check_data_nodes, Violations& errors)
{
  std::size_t record_index = 0;
  for (const auto& instance : input.instances) {
    const auto* cell = findCell(input, instance.cell_name);
    const std::size_t check_count = cell && cell->present ? cell->timing_checks.size() : 0;
    if (instance.has_timing_checks != (check_count != 0))
      reject(errors, "instances.has_timing_checks", "instance mirror must exactly derive from referenced check closure");
    for (std::size_t ordinal = 0; ordinal < check_count; ++ordinal) {
      if (record_index >= input.timing_check_endpoints.size()) {
        reject(errors, "timing_check_endpoints", "endpoint records omit a referenced check ordinal");
        ++record_index;
        continue;
      }
      const auto& record = input.timing_check_endpoints[record_index++];
      const auto& check = cell->timing_checks[ordinal];
      const std::string clock_full = instance.instance_name + ":" + check.clock_port;
      const std::string data_full = instance.instance_name + ":" + check.data_port;
      const auto expected_clock = resolveNode(nodes, clock_full);
      const auto expected_data = resolveNode(nodes, data_full);
      const bool exact = record.instance_name == instance.instance_name && record.referenced_check_ordinal == ordinal && record.check_kind == check.kind
                         && record.clock_pin_raw_name == check.clock_port && record.data_pin_raw_name == check.data_port
                         && record.clock_pin_full_name == clock_full && record.data_pin_full_name == data_full && record.resolved_clock_node == expected_clock
                         && record.resolved_data_node == expected_data;
      if (!exact)
        reject(errors, "timing_check_endpoints", "record must exactly reference its instance closure ordinal and raw/full endpoints");
      for (const auto& resolved : {record.resolved_clock_node, record.resolved_data_node})
        if (resolved
            && (resolved->value() >= input.nodes.size() || input.nodes[resolved->value()].is_port
                || input.nodes[resolved->value()].key.instance_name != instance.instance_name))
          reject(errors, "timing_check_endpoints.resolved_node", "resolved check endpoint must be a non-port pin of the same instance");
      if (record.resolved_data_node && record.resolved_data_node->value() < check_data_nodes.size())
        check_data_nodes[record.resolved_data_node->value()] = 1;
    }
  }
  if (record_index != input.timing_check_endpoints.size())
    reject(errors, "timing_check_endpoints", "endpoint records contain extra closure ordinals");
}

void validateNodeRoles(const CsrReferenceInput& input, const ClockReplay& replay, const std::vector<std::uint8_t>& check_data_nodes, Violations& errors)
{
  std::vector<std::uint8_t> incoming(input.nodes.size(), 0);
  std::vector<std::uint8_t> outgoing(input.nodes.size(), 0);
  for (const auto& arc : input.arcs) {
    if (arc.source.value() < outgoing.size())
      outgoing[arc.source.value()] = 1;
    if (arc.sink.value() < incoming.size())
      incoming[arc.sink.value()] = 1;
  }
  for (std::size_t index = 0; index < input.nodes.size(); ++index) {
    const auto& node = input.nodes[index];
    const bool input_port = node.is_port && (node.direction == PinDirection::kInput || node.direction == PinDirection::kInout);
    const bool output_port = node.is_port && (node.direction == PinDirection::kOutput || node.direction == PinDirection::kInout);
    bool expected_start = false;
    if (node.is_sequential && node.is_clock_pin) {
      const auto* instance = findInstance(input, node.key.instance_name);
      expected_start = instance && !instance->is_clock_gating;
    } else if (node.is_clock_pin) {
      expected_start = false;
    } else if (replay.direct_sources[index] != 0) {
      expected_start = input_port;
    } else {
      expected_start = incoming[index] == 0 || input_port;
    }
    const bool expected_end = !node.is_clock_pin && replay.direct_sources[index] == 0 && (outgoing[index] == 0 || output_port || check_data_nodes[index] != 0);
    if (node.is_startpoint != expected_start || node.is_endpoint != expected_end)
      reject(errors, "nodes.roles", "start/end roles differ from exact GraphBuilder priority and timing-check closure");
  }
}

std::vector<const PortConstraintModel*> portConstraintsByNode(const CsrReferenceInput& input, Violations& errors)
{
  std::vector<const PortConstraintModel*> result(input.nodes.size(), nullptr);
  for (const auto& constraint : input.port_constraints) {
    if (!constraint.resolved_node)
      continue;
    const std::uint64_t node = constraint.resolved_node->value();
    if (node >= result.size() || !input.nodes[node].is_port) {
      reject(errors, "port_constraints.resolved_node", "resolved constraint must reference a port NodeId");
    } else if (result[node]) {
      reject(errors, "port_constraints.resolved_node", "resolved port constraint is duplicated");
    } else {
      result[node] = &constraint;
    }
  }
  return result;
}

std::string resolvedClockTag(const CsrReferenceInput& input, const ClockReplay& replay, const std::vector<const PortConstraintModel*>& ports, NodeId node)
{
  for (const auto& clock : input.clocks)
    for (const auto& source : clock.ordered_sources)
      if (source.resolved_node == node)
        return clock.embedded_clock_name;
  if (node.value() < input.nodes.size() && !input.nodes[node.value()].is_port) {
    const auto* instance = findInstance(input, input.nodes[node.value()].key.instance_name);
    const auto* cell = instance ? findCell(input, instance->cell_name) : nullptr;
    if (instance && cell && cell->present && cell->is_sequential) {
      const auto clock_pin = findInstancePin(input, *instance, cell->clock_pin_name);
      if (clock_pin && clock_pin->value() < replay.owners.size() && replay.owners[clock_pin->value()])
        return *replay.owners[clock_pin->value()];
    }
  }
  if (node.value() < replay.owners.size() && replay.owners[node.value()])
    return *replay.owners[node.value()];
  if (node.value() < ports.size() && ports[node.value()] && !ports[node.value()]->clock_name.empty())
    return ports[node.value()]->clock_name;
  return input.clocks.empty() ? std::string{"clk"} : input.clocks.front().map_key;
}

bool allAbsent(const OptionalLaneMatrix& values)
{
  return std::ranges::all_of(values, [](const auto& row) { return std::ranges::all_of(row, [](const auto& value) { return !value.has_value(); }); });
}

const ClockConstraintModel* findDirectClock(const CsrReferenceInput& input, NodeId node)
{
  for (const auto& clock : input.clocks)
    for (const auto& source : clock.ordered_sources)
      if (source.resolved_node == node)
        return &clock;
  return nullptr;
}

Transition selectedCrprTransition(const ReferencedLibraryCellModel& cell)
{
  const auto selected = std::ranges::find_if(cell.cell_arcs, [](const auto& arc) { return arc.transfer.is_clock_arc; });
  if (selected == cell.cell_arcs.end() || selected->transfer.children().empty())
    return Transition::kRise;
  const Transition trigger = selected->transfer.children().front().signature.trigger_transition;
  return trigger == Transition::kNone ? Transition::kRise : trigger;
}

void validateSeeds(const CsrReferenceInput& input, const ClockReplay& replay, const std::vector<const PortConstraintModel*>& ports, Violations& errors)
{
  if (input.start_seeds.size() != input.ordered_start_ids.size())
    reject(errors, "start_seeds", "seeds must exactly cover ordered startpoints");
  for (std::size_t index = 0; index < input.start_seeds.size(); ++index) {
    const auto& seed = input.start_seeds[index];
    const bool seed_valid = seed.seed_node.value() < input.nodes.size();
    if (!seed_valid || index >= input.ordered_start_ids.size() || seed.seed_node != input.ordered_start_ids[index]) {
      reject(errors, "start_seeds.seed_node", "seed rows must exactly follow ordered startpoint order");
      continue;
    }
    const auto& node = input.nodes[seed.seed_node.value()];
    if (!knownEnum(seed.availability))
      reject(errors, "start_seeds.availability", "availability enum is unknown");
    if (seed.path_source && !knownEnum(*seed.path_source))
      reject(errors, "start_seeds.path_source", "path source enum is unknown");
    if (seed.crpr_transition && (!knownEnum(*seed.crpr_transition) || *seed.crpr_transition == Transition::kNone))
      reject(errors, "start_seeds.crpr_transition", "CRPR transition must be rise or fall");
    if (seed.clock_tag != resolvedClockTag(input, replay, ports, seed.seed_node))
      reject(errors, "start_seeds.clock_tag", "clock tag differs from the frozen resolution priority");

    bool expected_register = false;
    std::optional<NodeId> expected_clock;
    std::optional<NodeId> expected_output;
    const ReferencedLibraryCellModel* cell = nullptr;
    if (!node.is_port) {
      const auto* instance = findInstance(input, node.key.instance_name);
      cell = instance ? findCell(input, instance->cell_name) : nullptr;
      if (instance && cell && cell->present && cell->is_sequential) {
        expected_clock = findInstancePin(input, *instance, cell->clock_pin_name);
        expected_output = findInstancePin(input, *instance, cell->output_pin_name);
        const bool clock_reachable = expected_clock && expected_clock->value() < replay.owners.size() && replay.owners[expected_clock->value()].has_value();
        expected_register = clock_reachable && (seed.seed_node == expected_clock || seed.seed_node == expected_output);
      }
    }

    if (expected_register) {
      if (!expected_clock || !expected_output) {
        reject(errors, "start_seeds", "register seed requires selected clock and output pins");
        continue;
      }
      const NodeId expected_path_start = seed.seed_node == expected_clock ? *expected_output : seed.seed_node;
      const Transition expected_transition = selectedCrprTransition(*cell);
      if (seed.availability != StartSeedAvailability::kDeferredSequential || seed.path_source != PathSource::kRegister
          || seed.path_start_node != expected_path_start || seed.crpr_node != expected_clock || seed.crpr_transition != expected_transition
          || !allAbsent(seed.arrival) || !allAbsent(seed.slew)) {
        reject(errors, "start_seeds", "register seed differs from deferred-sequential structural contract");
      }
      continue;
    }

    if (!node.is_port && node.is_sequential) {
      if (seed.availability != StartSeedAvailability::kDeferredSequential || seed.path_source || seed.path_start_node != seed.seed_node || seed.crpr_node
          || seed.crpr_transition || !allAbsent(seed.arrival) || !allAbsent(seed.slew)) {
        reject(errors, "start_seeds", "non-register sequential start must use deferred-sequential null seed");
      }
      continue;
    }

    const auto* port = ports[seed.seed_node.value()];
    const auto* direct_clock = findDirectClock(input, seed.seed_node);
    const bool has_input_delay = port && std::ranges::any_of(port->input_delay, [](const auto& value) { return value.has_value(); });
    const bool materialized_input
        = node.is_port && (node.direction == PinDirection::kInput || node.direction == PinDirection::kInout) && (has_input_delay || direct_clock != nullptr);
    const std::optional<PathSource> expected_source = materialized_input ? std::optional{PathSource::kInput} : std::nullopt;
    OptionalLaneMatrix expected_arrival;
    OptionalLaneMatrix expected_slew;
    const double slew = port && port->input_transition ? *port->input_transition : 0.0;
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        expected_slew[analysis][transition] = slew;
        if (!materialized_input)
          continue;
        if (direct_clock != nullptr) {
          expected_arrival[analysis][transition] = analysis == 0 && transition == 1 ? direct_clock->edges[1] : direct_clock->edges[0];
        } else if (analysis == 0) {
          expected_arrival[analysis][transition] = port && port->input_delay[0] ? *port->input_delay[0] : 0.0;
        } else {
          expected_arrival[analysis][transition]
              = port && port->input_delay[1] ? *port->input_delay[1] : (port && port->input_delay[0] ? *port->input_delay[0] : 0.0);
        }
      }
    }
    if (seed.availability != StartSeedAvailability::kMaterialized || seed.path_source != expected_source || seed.path_start_node != seed.seed_node
        || seed.crpr_node || seed.crpr_transition || seed.arrival != expected_arrival || seed.slew != expected_slew) {
      reject(errors, "start_seeds", "materialized nonsequential seed differs from exact native constraint values");
    }
  }
}

double endpointPeriod(const CsrReferenceInput& input, const std::string& tag)
{
  const auto named = std::ranges::find(input.clocks, tag, &ClockConstraintModel::map_key);
  if (named != input.clocks.end())
    return named->period;
  return input.clocks.empty() ? 0.0 : input.clocks.front().period;
}

void validateEndpoints(const CsrReferenceInput& input, const ClockReplay& replay, const std::vector<const PortConstraintModel*>& ports, Violations& errors)
{
  std::vector<NodeId> expected;
  for (NodeId node : input.ordered_end_ids)
    if (node.value() < input.nodes.size() && input.nodes[node.value()].is_port
        && (input.nodes[node.value()].direction == PinDirection::kOutput || input.nodes[node.value()].direction == PinDirection::kInout))
      expected.push_back(node);
  if (input.endpoint_required.size() != expected.size())
    reject(errors, "endpoint_required", "numeric required rows must exactly cover output/inout port endpoints");
  for (std::size_t index = 0; index < input.endpoint_required.size(); ++index) {
    const auto& endpoint = input.endpoint_required[index];
    if (index >= expected.size() || endpoint.node != expected[index] || endpoint.node.value() >= input.nodes.size()) {
      reject(errors, "endpoint_required.node", "required rows must follow output endpoint order");
      continue;
    }
    const auto* port = ports[endpoint.node.value()];
    const std::array<std::optional<double>, kAnalysisCount> expected_delay = port ? port->output_delay : decltype(expected_delay){};
    const std::string tag = resolvedClockTag(input, replay, ports, endpoint.node);
    if (endpoint.output_delay != expected_delay || endpoint.clock_name != tag || endpoint.clock_period != endpointPeriod(input, tag))
      reject(errors, "endpoint_required", "required row differs from output-delay and clock fallback semantics");
  }
}

void validateCapacitances(const CsrReferenceInput& input, Violations& errors)
{
  std::vector<NodeId> expected;
  for (const auto& node : input.nodes)
    if (!node.is_port)
      expected.push_back(node.id);
  std::ranges::sort(expected,
                    [&](NodeId lhs, NodeId rhs) { return byteStringLess(input.nodes[lhs.value()].key.full_name, input.nodes[rhs.value()].key.full_name); });
  if (input.cell_pin_capacitances.size() != expected.size())
    reject(errors, "cell_pin_capacitances", "capacitance rows must exactly cover instance pins");
  for (std::size_t index = 0; index < input.cell_pin_capacitances.size(); ++index) {
    const auto& cap = input.cell_pin_capacitances[index];
    if (index >= expected.size() || cap.node != expected[index] || cap.node.value() >= input.nodes.size()) {
      reject(errors, "cell_pin_capacitances.node", "capacitance rows must follow strict pin full-name order");
      continue;
    }
    const auto& node = input.nodes[cap.node.value()];
    const auto* instance = findInstance(input, node.key.instance_name);
    const auto* cell = instance ? findCell(input, instance->cell_name) : nullptr;
    const LibraryPortModel* port = nullptr;
    if (cell && cell->present) {
      const auto found = std::ranges::find(cell->ports, node.key.pin_name, &LibraryPortModel::name);
      if (found != cell->ports.end())
        port = &*found;
    }
    const double expected_default = port ? port->default_capacitance : 0.0;
    const OptionalLaneMatrix expected_transition = port ? port->transition_capacitance : OptionalLaneMatrix{};
    if (cap.default_capacitance != expected_default || cap.transition_capacitance != expected_transition)
      reject(errors, "cell_pin_capacitances", "capacitance row differs from referenced Liberty port closure");
  }
}

void validateLoads(const CsrReferenceInput& input, const std::vector<const PortConstraintModel*>& ports, Violations& errors)
{
  std::set<std::string_view> rc_nets;
  for (const auto& net : input.parasitics.nets)
    rc_nets.insert(net.map_key);
  std::vector<const CellPinCapacitanceModel*> caps(input.nodes.size(), nullptr);
  for (const auto& cap : input.cell_pin_capacitances)
    if (cap.node.value() < caps.size())
      caps[cap.node.value()] = &cap;
  std::map<std::string, ArcOutputLoadMatrix> net_loads;
  std::vector<std::uint8_t> seen(input.nodes.size(), 0);
  std::vector<NodeId> load_nodes;
  for (const auto& arc : input.arcs) {
    if (arc.kind == ArcKind::kNet && arc.sink.value() < input.nodes.size() && !seen[arc.sink.value()]) {
      seen[arc.sink.value()] = 1;
      load_nodes.push_back(arc.sink);
    }
  }
  // Native refreshNetDriverLoad sorts full pin names. Diagnostic Dual-CSR
  // projections have canonical ArcKey ranks, which need not preserve this sum.
  std::ranges::sort(load_nodes, [&](NodeId lhs, NodeId rhs) {
    return byteStringLess(input.nodes[lhs.value()].key.full_name, input.nodes[rhs.value()].key.full_name);
  });
  for (NodeId id : load_nodes) {
    const auto& sink = input.nodes[id.value()];
    if (rc_nets.contains(sink.net_name))
      continue;
    auto& load = net_loads[sink.net_name];
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition)
        if (sink.is_port) {
          if ((sink.direction == PinDirection::kOutput || sink.direction == PinDirection::kInout) && ports[id.value()]
              && ports[id.value()]->output_load)
            load[analysis][transition] += *ports[id.value()]->output_load;
        } else if (caps[id.value()]) {
          const auto& cap = *caps[id.value()];
          load[analysis][transition] += cap.transition_capacitance[analysis][transition].value_or(cap.default_capacitance);
        }
  }
  for (const auto& arc : input.arcs) {
    if (arc.sink.value() >= input.nodes.size() || !arc.output_load)
      continue;
    const auto found = net_loads.find(input.nodes[arc.sink.value()].net_name);
    const ArcOutputLoadMatrix expected = found == net_loads.end() ? ArcOutputLoadMatrix{} : found->second;
    if (*arc.output_load != expected)
      reject(errors, "arcs.output_load", "no-parasitic load differs from exact native-order sink-net sum");
  }
  for (const auto& seed : input.start_seeds) {
    if (seed.seed_node.value() >= input.nodes.size()) continue;
    const auto& node = input.nodes[seed.seed_node.value()];
    const bool needs_load = seed.availability == StartSeedAvailability::kDeferredSequential
        && node.is_output_pin && !rc_nets.contains(node.net_name);
    if (seed.output_load.has_value() != needs_load) {
      reject(errors, "start_seeds.output_load", "load belongs to a non-parasitic deferred Q seed");
      continue;
    }
    if (seed.output_load) {
      const auto found = net_loads.find(node.net_name);
      const ArcOutputLoadMatrix expected = found == net_loads.end() ? ArcOutputLoadMatrix{} : found->second;
      if (*seed.output_load != expected)
        reject(errors, "start_seeds.output_load", "Q load differs from exact native-order net sum");
    }
  }
}

void validateClockTags(const CsrReferenceInput& input, Violations& errors)
{
  std::vector<std::string> expected;
  for (const auto& clock : input.clocks) {
    if (!clock.map_key.empty())
      expected.push_back(clock.map_key);
    if (!clock.embedded_clock_name.empty())
      expected.push_back(clock.embedded_clock_name);
  }
  for (const auto& port : input.port_constraints)
    if (!port.clock_name.empty())
      expected.push_back(port.clock_name);
  for (const auto& seed : input.start_seeds)
    if (!seed.clock_tag.empty())
      expected.push_back(seed.clock_tag);
  for (const auto& endpoint : input.endpoint_required)
    if (!endpoint.clock_name.empty())
      expected.push_back(endpoint.clock_name);
  std::ranges::sort(expected, byteStringLess);
  expected.erase(std::ranges::unique(expected).begin(), expected.end());
  if (input.clock_tags != expected)
    reject(errors, "clock_tags", "tags must be the exact byte-sorted unique nonempty union");
}

}  // namespace

void validateReferenceConstraints(const CsrReferenceInput& input, Violations& errors)
{
  const NodeResolutionMap nodes = makeNodeResolutionMap(input);
  validateRawReferences(input, nodes, errors);
  const ClockReplay replay = replayClocks(input, errors);
  std::vector<std::uint8_t> check_data_nodes(input.nodes.size(), 0);
  validateTimingChecks(input, nodes, check_data_nodes, errors);
  validateNodeRoles(input, replay, check_data_nodes, errors);
  const auto ports = portConstraintsByNode(input, errors);

  bool has_gating = false;
  for (const auto& instance : input.instances) {
    has_gating |= instance.is_clock_gating;
  }
  bool has_divider = false;
  bool net_shape_valid = true;
  std::map<std::string, NodeId> net_drivers;
  std::vector<std::uint8_t> net_load_seen(input.nodes.size(), 0);
  for (std::size_t index = 0; index < input.arcs.size(); ++index) {
    const auto& arc = input.arcs[index];
    const bool endpoints_valid = arc.source.value() < input.nodes.size() && arc.sink.value() < input.nodes.size();
    const bool active = !arc.is_disabled && !arc.is_loop_disabled;
    const bool sequential_clock_sink
        = endpoints_valid && arc.kind == ArcKind::kNet && input.nodes[arc.sink.value()].is_sequential && input.nodes[arc.sink.value()].is_clock_pin;
    const bool sink_reachable = endpoints_valid && arc.sink.value() < replay.owners.size() && replay.owners[arc.sink.value()].has_value();
    const ArcEligibility expected{active, active && !sequential_clock_sink, active && !(sequential_clock_sink && sink_reachable),
                                  index < replay.arcs.size() && replay.arcs[index] != 0};
    if (!(arc.eligibility == expected))
      reject(errors, "arcs.eligibility", "eligibility differs from active graph and replayed clock derivation");
    if (!endpoints_valid || arc.kind != ArcKind::kNet)
      continue;

    const auto& source = input.nodes[arc.source.value()];
    const auto& sink = input.nodes[arc.sink.value()];
    const auto* source_instance = source.is_port ? nullptr : findInstance(input, source.key.instance_name);
    const auto* source_cell = source_instance ? findCell(input, source_instance->cell_name) : nullptr;
    const auto* sink_instance = sink.is_port ? nullptr : findInstance(input, sink.key.instance_name);
    const auto* sink_cell = sink_instance ? findCell(input, sink_instance->cell_name) : nullptr;
    const bool divider = source_instance && source_cell && source_cell->present && source_cell->is_sequential && !source_cell->is_clock_gating
                         && !source_cell->is_macro && sink_instance && sink_cell && sink_cell->present && sink.key.pin_name == sink_cell->clock_pin_name;
    has_divider |= divider;
    if (arc.is_disabled != divider)
      reject(errors, "arcs.is_disabled", "net disabled flag must exactly match the sequential-divider predicate");
    const auto [driver, inserted] = net_drivers.emplace(arc.key.base.owner_name, arc.source);
    if ((!inserted && driver->second != arc.source) || arc.source == arc.sink || net_load_seen[arc.sink.value()]++ != 0)
      net_shape_valid = false;
  }
  if (!net_shape_valid)
    reject(errors, "arcs.net_projection", "net arcs require one driver and unique non-self load sinks");

  validateSeeds(input, replay, ports, errors);
  validateEndpoints(input, replay, ports, errors);
  validateCapacitances(input, errors);
  validateLoads(input, ports, errors);
  validateClockTags(input, errors);
  const bool prepared_parasitics = validateReferenceParasitics(input, errors);

  std::vector<CsrReferenceUnsupportedReason> expected_reasons;
  if (!input.parasitics.nets.empty() && (input.analysis_context.rc_corner != "pre_route" || !prepared_parasitics))
    expected_reasons.push_back(CsrReferenceUnsupportedReason::kParasiticNet);
  if (has_gating || has_divider)
    expected_reasons.push_back(CsrReferenceUnsupportedReason::kClockGatingOrDivider);
  if (!input.case_analysis_constraints.empty())
    expected_reasons.push_back(CsrReferenceUnsupportedReason::kCaseAnalysis);
  if (input.unsupported_reasons != expected_reasons)
    reject(errors, "unsupported_reasons", "reasons must exactly equal the stable enum-order semantic derivation");
}

}  // namespace ista::csr::detail
