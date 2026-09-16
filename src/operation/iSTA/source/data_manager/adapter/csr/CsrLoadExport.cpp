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
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include <vector>

#include "CsrReferenceInputAdapterInternal.hpp"

namespace ista::csr::detail {
namespace {

using LoadMatrix = ArcOutputLoadMatrix;

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

const CellPinCapacitanceModel* findPinCapacitance(const ReferenceBuildState& state, NodeId node)
{
  if (node.value() >= state.capacitance_indices_by_node.size())
    malformedReferenceInput();
  const auto index = state.capacitance_indices_by_node[node.value()];
  if (!index)
    return nullptr;
  if (*index >= state.input.cell_pin_capacitances.size())
    malformedReferenceInput();
  return &state.input.cell_pin_capacitances[*index];
}

const ReferencedLibraryCellModel& requireReferencedCell(const ReferenceBuildState& state, const std::string& cell_name)
{
  const auto found = state.referenced_cell_indices_by_name.find(cell_name);
  if (found == state.referenced_cell_indices_by_name.end() || found->second >= state.input.referenced_library_cells.size())
    malformedReferenceInput();
  return state.input.referenced_library_cells[found->second];
}

std::map<std::string, LoadMatrix> buildNetLoads(const ReferenceBuildState& state)
{
  std::map<std::string, LoadMatrix> result;
  for (const auto& [map_key, native_net] : state.database.get_net_map()) {
    if (map_key != native_net.get_net_name())
      malformedReferenceInput();
    std::map<std::string, CanonicalLoadPinFacts> facts;
    for (const auto& load_pin_name : native_net.get_load_pin_list()) {
      const auto node = resolveNode(state, load_pin_name);
      if (!node)
        malformedReferenceInput();
      const NodeRecord& load_node = requireNode(state, *node);
      if (load_node.net_name != map_key)
        malformedReferenceInput();
      CanonicalLoadPinFacts pin_facts;
      pin_facts.key = load_node.key;
      pin_facts.direction = load_node.direction;
      pin_facts.is_port = load_node.is_port;
      if (load_node.is_port) {
        const PortConstraintModel* constraint
            = findPortConstraint(state, load_node.id);
        if (constraint != nullptr
            && (load_node.direction == PinDirection::kOutput
                || load_node.direction == PinDirection::kInout))
          pin_facts.port_output_load = constraint->output_load;
      } else {
        const CellPinCapacitanceModel* capacitance
            = findPinCapacitance(state, load_node.id);
        if (capacitance == nullptr)
          malformedReferenceInput();
        pin_facts.cell_pin_capacitance = *capacitance;
      }
      if (!facts.emplace(load_pin_name, std::move(pin_facts)).second)
        malformedReferenceInput();
    }
    if (!result.emplace(map_key, projectCanonicalNoParasiticNetLoad(
                                     state.database, map_key, facts)).second)
      malformedReferenceInput();
  }
  return result;
}

void exportLoads(ReferenceBuildState& state)
{
  if (!state.input.parasitics.nets.empty()) {
    for (auto& arc : state.input.arcs)
      arc.output_load.reset();
    return;
  }

  const auto net_loads = buildNetLoads(state);
  for (auto& arc : state.input.arcs) {
    const NodeRecord& sink = requireNode(state, arc.sink);
    arc.output_load = projectCanonicalNoParasiticOutputLoad(sink.net_name, net_loads);
  }
}

void preflightConvertedLoads(const ReferenceBuildState& state)
{
  for (const auto& arc : state.input.arcs) {
    if (!arc.output_load)
      continue;
    for (const auto& child : arc.transfer.children) {
      if (std::abs(child.capacitance_unit_scale - 1.0) < STA_ERROR)
        continue;
      for (const auto& analysis : *arc.output_load) {
        for (double load : analysis) {
          const double converted = std::ceil(load * child.capacitance_unit_scale);
          if (!std::isfinite(converted) || converted < 0.0 || converted > static_cast<double>(std::numeric_limits<int>::max()))
            illegalReferenceNumeric();
        }
      }
    }
  }
}

std::vector<std::optional<std::size_t>> indexInstanceOwners(const ReferenceBuildState& state)
{
  std::vector<std::optional<std::size_t>> result(state.input.nodes.size());
  for (std::size_t instance_index = 0; instance_index < state.input.instances.size(); ++instance_index) {
    const auto& instance = state.input.instances[instance_index];
    for (NodeId node : instance.ordered_pins) {
      if (node.value() >= result.size() || result[node.value()])
        malformedReferenceInput();
      result[node.value()] = instance_index;
    }
  }
  return result;
}

bool validateDividerFlags(const ReferenceBuildState& state)
{
  const auto instance_by_node = indexInstanceOwners(state);
  bool any_divider = false;
  for (const auto& arc : state.input.arcs) {
    if (arc.kind != ArcKind::kNet)
      continue;
    const NodeRecord& source = requireNode(state, arc.source);
    const NodeRecord& sink = requireNode(state, arc.sink);
    bool divider = false;
    if (!source.is_port && !sink.is_port && instance_by_node[source.id.value()] && instance_by_node[sink.id.value()]) {
      const InstanceRecord& source_instance = state.input.instances[*instance_by_node[source.id.value()]];
      const InstanceRecord& sink_instance = state.input.instances[*instance_by_node[sink.id.value()]];
      const ReferencedLibraryCellModel& source_cell = requireReferencedCell(state, source_instance.cell_name);
      const ista::Instance& native_sink = requireNativeInstance(state, sink_instance.instance_name);
      divider = source_cell.present && source_cell.is_sequential && !source_cell.is_clock_gating && !source_cell.is_macro
                && sink.key.full_name == native_sink.get_clock_pin_name();
    }
    any_divider |= divider;
    if (arc.is_disabled != divider)
      malformedReferenceInput();
  }
  return any_divider;
}

void exportUnsupportedReasons(ReferenceBuildState& state)
{
  const bool has_divider = validateDividerFlags(state);
  for (const auto& seed : state.input.start_seeds)
    if (seed.crpr_node.has_value() != seed.crpr_transition.has_value())
      malformedReferenceInput();
  const bool has_propagated_clock = std::ranges::any_of(state.input.clocks, [](const ClockConstraintModel& clock) { return clock.propagated; });
  const bool has_clock_gating = std::ranges::any_of(state.input.instances, [](const InstanceRecord& instance) { return instance.is_clock_gating; });

  state.input.unsupported_reasons.clear();
  const auto add = [&](bool condition, CsrReferenceUnsupportedReason reason) {
    if (condition)
      state.input.unsupported_reasons.push_back(reason);
  };
  add(!state.input.parasitics.nets.empty(), CsrReferenceUnsupportedReason::kParasiticNet);
  add(has_propagated_clock, CsrReferenceUnsupportedReason::kPropagatedClock);
  add(has_clock_gating || has_divider, CsrReferenceUnsupportedReason::kClockGatingOrDivider);
  add(!state.input.case_analysis_constraints.empty(), CsrReferenceUnsupportedReason::kCaseAnalysis);
}

}  // namespace

void exportReferenceLoadsAndSupport(ReferenceBuildState& state)
{
  exportLoads(state);
  preflightConvertedLoads(state);
  exportUnsupportedReasons(state);
}

}  // namespace ista::csr::detail
