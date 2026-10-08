#pragma once
#include "ParasiticNet.hpp"
#include <map>
#include <string>
#include <tuple>
#include <vector>
namespace izh::test {
using NodeValue = std::tuple<double, double, double>;
using EdgeValue = std::tuple<std::string, std::string, double>;
inline auto rcValues(const ista::ParasiticNet& net)
{
  std::map<std::string, NodeValue> nodes;
  std::vector<EdgeValue> edges;
  for (const auto& [name, node] : net.get_node_map())
    nodes.emplace(name, NodeValue{node.get_x(), node.get_y(), node.get_capacitance()});
  for (const auto& edge : net.get_resistor_list())
    edges.emplace_back(edge.get_source_node(), edge.get_sink_node(), edge.get_resistance());
  return std::tuple{net.get_net_name(), net.get_lumped_capacitance(), nodes, edges};
}
}  // namespace izh::test
