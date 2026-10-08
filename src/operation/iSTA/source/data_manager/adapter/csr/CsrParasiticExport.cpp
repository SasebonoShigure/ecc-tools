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
#include <utility>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"

namespace ista::csr::detail {

ParasiticNetModel projectCanonicalParasiticNet(
    const std::string& map_key, const ista::ParasiticNet& native_net)
{
  ParasiticNetModel net;
  net.map_key = map_key;
  net.embedded_name = native_net.get_net_name();
  net.lumped_capacitance = native_net.get_lumped_capacitance();
  requireNonnegativeReference(net.lumped_capacitance);
  net.nodes.reserve(native_net.get_node_map().size());
  net.resistors.reserve(native_net.get_resistor_list().size());

  for (const auto& [node_key, native_node] : native_net.get_node_map()) {
    ParasiticNodeModel node;
    node.map_key = node_key;
    node.embedded_name = native_node.get_node_name();
    node.capacitance = native_node.get_capacitance();
    node.x = native_node.get_x();
    node.y = native_node.get_y();
    requireNonnegativeReference(node.capacitance);
    requireFiniteReference(node.x);
    requireFiniteReference(node.y);
    net.nodes.push_back(std::move(node));
  }

  for (const auto& native_resistor : native_net.get_resistor_list()) {
    ParasiticResistorModel resistor;
    resistor.source = native_resistor.get_source_node();
    resistor.sink = native_resistor.get_sink_node();
    resistor.resistance = native_resistor.get_resistance();
    requireNonnegativeReference(resistor.resistance);
    net.resistors.push_back(std::move(resistor));
  }
  return net;
}

void exportReferenceParasitics(ReferenceBuildState& state)
{
  const auto& native_library = state.database.get_parasitic_library();
  const auto& native_nets = native_library.get_net_map();
  if (native_nets.empty()) {
    state.input.parasitics = {};
    return;
  }

  if (native_library.get_capacitive_unit().empty() || native_library.get_resistance_unit().empty())
    malformedReferenceInput();

  ParasiticModel model;
  model.capacitance_unit = native_library.get_capacitive_unit();
  model.resistance_unit = native_library.get_resistance_unit();
  const auto& timing = state.database.get_timing_library();
  model.input_port_thresholds = {timing.get_slew_derate_from_library(),
      {timing.get_slew_lower_threshold_pct_rise(), timing.get_slew_lower_threshold_pct_fall()},
      {timing.get_slew_upper_threshold_pct_rise(), timing.get_slew_upper_threshold_pct_fall()}};
  requireFiniteReference(model.input_port_thresholds.slew_derate);
  for (double value : model.input_port_thresholds.lower) requireFiniteReference(value);
  for (double value : model.input_port_thresholds.upper) requireFiniteReference(value);
  model.nets.reserve(native_nets.size());

  for (const auto& [map_key, native_net] : native_nets)
    model.nets.push_back(projectCanonicalParasiticNet(map_key, native_net));

  state.input.parasitics = std::move(model);
}

}  // namespace ista::csr::detail
