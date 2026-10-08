// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <map>
#include <string>
#include "ParasiticNet.hpp"

namespace ista::timing_edit::test {
inline void wire(std::map<std::string, ParasiticNet>& trees, const std::string& name,
          const std::string& driver, const std::string& sink, double resistance)
{
  auto& net = trees.try_emplace(name).first->second;
  net.set_net_name(name);
  for (const auto* pin : {&driver, &sink}) {
    auto& node = net.get_node_map()[*pin];
    node.set_node_name(*pin);
    node.set_capacitance(0.01);
  }
  auto& edge = net.get_resistor_list().emplace_back();
  edge.set_source_node(driver);
  edge.set_sink_node(sink);
  edge.set_resistance(resistance);
  net.set_lumped_capacitance(0.02);
}
}  // namespace ista::timing_edit::test
