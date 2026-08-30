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
#include <cstddef>
#include <optional>
#include <utility>

#include "CsrReferenceInputAdapterInternal.hpp"

namespace ista::csr::detail {
namespace {

std::optional<double> presentValue(bool present, double value, bool nonnegative)
{
  if (!present)
    return std::nullopt;
  requireFiniteReference(value);
  if (nonnegative)
    requireNonnegativeReference(value);
  return value;
}

}  // namespace

void exportReferenceConstraints(ReferenceBuildState& state)
{
  const auto& constraints = state.database.get_timing_constraint();

  state.input.clocks.reserve(constraints.get_clock_map().size());
  for (const auto& [map_key, native_clock] : constraints.get_clock_map()) {
    ClockConstraintModel clock;
    clock.map_key = map_key;
    clock.embedded_clock_name = native_clock.get_clock_name();
    clock.period = native_clock.get_period();
    clock.edges = {native_clock.get_rise_edge(), native_clock.get_fall_edge()};
    clock.propagated = native_clock.get_is_propagated();
    requireFiniteReference(clock.period);
    requireFiniteReference(clock.edges[0]);
    requireFiniteReference(clock.edges[1]);

    clock.ordered_sources.reserve(native_clock.get_source_list().size());
    for (const auto& source_name : native_clock.get_source_list()) {
      RawNodeReference source{source_name, resolveNode(state, source_name)};
      if (source.resolved_node)
        state.clock_source_nodes[source.resolved_node->value()] = 1;
      clock.ordered_sources.push_back(std::move(source));
    }
    state.input.clocks.push_back(std::move(clock));
  }

  state.input.port_constraints.reserve(constraints.get_port_constraint_map().size());
  for (const auto& [map_key, native_constraint] : constraints.get_port_constraint_map()) {
    PortConstraintModel constraint;
    constraint.map_key = map_key;
    constraint.embedded_port_name = native_constraint.get_port_name();
    constraint.resolved_node = resolveNode(state, map_key);
    constraint.input_delay[0] = presentValue(native_constraint.get_has_input_delay_max(), native_constraint.get_input_delay_max(), false);
    constraint.input_delay[1] = presentValue(native_constraint.get_has_input_delay_min(), native_constraint.get_input_delay_min(), false);
    constraint.input_transition = presentValue(native_constraint.get_has_input_transition(), native_constraint.get_input_transition(), true);
    constraint.output_delay[0] = presentValue(native_constraint.get_has_output_delay_max(), native_constraint.get_output_delay_max(), false);
    constraint.output_delay[1] = presentValue(native_constraint.get_has_output_delay_min(), native_constraint.get_output_delay_min(), false);
    constraint.output_load = presentValue(native_constraint.get_has_load(), native_constraint.get_load(), true);
    constraint.clock_name = native_constraint.get_clock_name();

    const std::size_t index = state.input.port_constraints.size();
    if (constraint.resolved_node) {
      const NodeRecord& node = requireNode(state, *constraint.resolved_node);
      if (!node.is_port || state.port_constraint_indices_by_node[node.id.value()])
        malformedReferenceInput();
      state.port_constraint_indices_by_node[node.id.value()] = index;
    }
    state.input.port_constraints.push_back(std::move(constraint));
  }

  state.input.case_analysis_constraints.reserve(constraints.get_case_analysis_map().size());
  for (const auto& [raw_name, value] : constraints.get_case_analysis_map())
    state.input.case_analysis_constraints.push_back({raw_name, resolveNode(state, raw_name), value});
}

}  // namespace ista::csr::detail
