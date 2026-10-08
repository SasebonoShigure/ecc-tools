// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrParasiticTree.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ista::csr {
namespace {
constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();
struct Branch { std::size_t node, edge; double resistance; };
struct Node
{
  std::string_view name;
  double capacitance{0.0};
  std::vector<Branch> edges;
};

void nonnegative(double value)
{
  if (!std::isfinite(value) || value < 0.0)
    throw std::invalid_argument("invalid RC capacitance or resistance");
}
}  // namespace

CsrParasiticTree prepareCsrParasiticTree(const ParasiticNetModel& net, NodeId driver, std::span<const CsrRcPin> pins)
{
  if (pins.empty() || net.map_key.empty() || net.map_key != net.embedded_name)
    throw std::invalid_argument("invalid RC net identity");
  std::vector<Node> nodes;
  std::unordered_map<std::string_view, std::size_t> by_name;
  auto add_node = [&](std::string_view name) {
    if (name.empty()) throw std::invalid_argument("empty RC node");
    auto [entry, inserted] = by_name.emplace(name, nodes.size());
    if (inserted) nodes.push_back({name, 0.0, {}});
    return entry->second;
  };
  for (const auto& node : net.nodes) {
    if (node.map_key != node.embedded_name || by_name.contains(node.map_key))
      throw std::invalid_argument("invalid RC node identity");
    nonnegative(node.capacitance);
    nodes[add_node(node.map_key)].capacitance = node.capacitance;
  }
  const auto explicit_node_count = nodes.size();
  for (std::size_t edge = 0; edge < net.resistors.size(); ++edge) {
    const auto& resistor = net.resistors[edge];
    nonnegative(resistor.resistance);
    const auto source = add_node(resistor.source), sink = add_node(resistor.sink);
    const double resistance = resistor.resistance * 1E-3;
    nodes[source].edges.push_back({sink, edge, resistance});
    nodes[sink].edges.push_back({source, edge, resistance});
  }
  if (nodes.empty() || net.resistors.size() != nodes.size() - 1)
    throw std::invalid_argument("RC support requires one connected tree");

  std::unordered_map<std::string_view, std::size_t> pin_by_name;
  std::unordered_set<std::uint64_t> pin_ids;
  std::size_t driver_pin = absent;
  for (std::size_t i = 0; i < pins.size(); ++i) {
    const auto& pin = pins[i];
    if (pin.name.empty() || !pin_by_name.emplace(pin.name, i).second || !pin_ids.emplace(pin.node.value()).second)
      throw std::invalid_argument("duplicate or empty RC pin");
    for (const auto& analysis : pin.capacitance)
      for (double capacitance : analysis) nonnegative(capacitance);
    if (pin.node == driver) driver_pin = i;
  }
  if (driver_pin == absent) throw std::invalid_argument("missing RC driver pin");
  auto source = by_name.find(pins[driver_pin].name);
  if (source == by_name.end() || source->second >= explicit_node_count) {
    std::string spef_name(pins[driver_pin].name);
    std::replace(spef_name.begin(), spef_name.end(), ':', '/');
    source = by_name.find(spef_name);
  }
  if (source == by_name.end() || source->second >= explicit_node_count) throw std::invalid_argument("missing RC driver node");
  const std::size_t root = source->second;

  struct Frame { std::size_t node, parent_edge, next, tree_index; };
  std::vector<Frame> stack{{root, absent, 0, 0}};
  std::vector<bool> visited(nodes.size(), false), pin_seen(pins.size(), false);
  std::vector<std::size_t> ordered_nodes{root}, terminals, terminal_pin;
  std::vector<CsrRcTreeNode> tree{{0, 0.0, 0.0}};
  visited[root] = true;
  while (!stack.empty()) {
    auto& frame = stack.back();
    if (frame.next == nodes[frame.node].edges.size()) {
      stack.pop_back();
      continue;
    }
    const auto branch = nodes[frame.node].edges[frame.next++];
    if (branch.edge == frame.parent_edge) continue;
    if (visited[branch.node]) throw std::invalid_argument("cyclic RC network");
    const auto parent = frame.tree_index;
    visited[branch.node] = true;
    ordered_nodes.push_back(branch.node);
    tree.push_back({parent, branch.resistance, 0.0});
    stack.push_back({branch.node, branch.edge, 0, tree.size() - 1});
  }
  if (ordered_nodes.size() != nodes.size()) throw std::invalid_argument("disconnected RC network");

  CsrParasiticTree result;
  result.net_name = net.map_key;
  for (std::size_t i = 0; i < ordered_nodes.size(); ++i) {
    const auto name = nodes[ordered_nodes[i]].name;
    auto pin = pin_by_name.find(name);
    if (pin == pin_by_name.end()) {
      std::string native_name(name);
      std::replace(native_name.begin(), native_name.end(), '/', ':');
      pin = pin_by_name.find(native_name);
    }
    if (pin == pin_by_name.end()) continue;
    if (ordered_nodes[i] >= explicit_node_count)
      throw std::invalid_argument("RC pin requires an explicit parasitic node");
    if (pin_seen[pin->second]) throw std::invalid_argument("ambiguous RC pin alias");
    pin_seen[pin->second] = true;
    terminals.push_back(i);
    terminal_pin.push_back(pin->second);
    result.terminals.push_back(pins[pin->second].node);
  }
  if (result.terminals.size() != pins.size() || result.terminals.front() != driver)
    throw std::invalid_argument("RC tree does not cover its pins");

  for (std::size_t lane = 0; lane < result.lanes.size(); ++lane) {
    const auto analysis = lane / kTransitionCount, transition = lane % kTransitionCount;
    for (std::size_t previous = 0; previous < lane; ++previous) {
      if (std::ranges::all_of(pins, [&](const CsrRcPin& pin) {
            return pin.capacitance[analysis][transition]
                   == pin.capacitance[previous / kTransitionCount][previous % kTransitionCount];
          })) {
        result.lanes[lane] = result.lanes[previous];
        break;
      }
    }
    if (result.lanes[lane]) continue;
    for (std::size_t i = 0; i < tree.size(); ++i)
      tree[i].capacitance_pf = nodes[ordered_nodes[i]].capacitance;
    for (std::size_t i = 0; i < terminals.size(); ++i) {
      auto& capacitance = tree[terminals[i]].capacitance_pf;
      capacitance += pins[terminal_pin[i]].capacitance[analysis][transition];
      if (!std::isfinite(capacitance)) throw std::overflow_error("RC node load overflow");
    }
    result.lanes[lane] = std::make_shared<const CsrParasiticModel>(buildCsrParasiticModel(tree, terminals));
  }
  return result;
}
}  // namespace ista::csr
