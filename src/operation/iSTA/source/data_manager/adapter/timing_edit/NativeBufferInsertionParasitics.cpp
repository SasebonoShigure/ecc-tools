// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "NativeBufferInsertionInternal.hpp"

#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace ista::timing_edit::detail {
void PreparedNativeBufferInsertions::prepareParasitics(std::map<std::string, ParasiticNet> replacements)
{
  // These are CPU-prepared physical trees, not timing answers. Numeric and
  // terminal identity checks happen before any canonical map node is removed.
  std::vector<std::pair<std::string, decltype(parasitics)::node_type>> previous;
  previous.reserve(replacements.size());
  for (const auto& [name, net] : replacements) {
    const auto members = memberships.find(name);
    const auto added = nets.find(name);
    if (members == memberships.end() && added == nets.end())
      throw std::invalid_argument("pre-route RC replacement is outside the edited nets");
    if (name != net.get_net_name())
      throw std::invalid_argument("pre-route RC net identity mismatch");
    const auto nonnegative = [](double value) {
      if (!std::isfinite(value) || value < 0.0)
        throw std::invalid_argument("pre-route RC value is negative or non-finite");
    };
    nonnegative(net.get_lumped_capacitance());
    const auto& terminals = members != memberships.end() ? members->second : added->second.get_pin_name_list();
    const std::set<std::string_view> terminal_names(terminals.begin(), terminals.end());
    for (const auto& pin : terminals)
      if (!net.get_node_map().contains(pin))
        throw std::invalid_argument("pre-route RC tree is missing a post-edit terminal");
    if (net.get_node_map().empty() || net.get_resistor_list().size() != net.get_node_map().size() - 1)
      throw std::invalid_argument("pre-route RC requires a connected tree");
    std::map<std::string_view, std::size_t> node_ids;
    std::vector<std::size_t> parents;
    parents.reserve(net.get_node_map().size());
    for (const auto& [node, value] : net.get_node_map()) {
      node_ids.emplace(node, parents.size());
      parents.push_back(parents.size());
      nonnegative(value.get_capacitance());
      if (!std::isfinite(value.get_x()) || !std::isfinite(value.get_y()))
        throw std::invalid_argument("pre-route RC coordinate is non-finite");
      if (node.empty() || node != value.get_node_name())
        throw std::invalid_argument("pre-route RC node identity mismatch");
      const auto existing = database->get_pin_map().find(node);
      const auto created = pins.find(node);
      if ((existing != database->get_pin_map().end() || created != pins.end())
          && !terminal_names.contains(node))
        throw std::invalid_argument("pre-route RC tree contains a foreign terminal");
    }
    const auto root = [&](std::size_t node) {
      while (parents[node] != node) {
        parents[node] = parents[parents[node]];
        node = parents[node];
      }
      return node;
    };
    for (const auto& resistor : net.get_resistor_list()) {
      nonnegative(resistor.get_resistance());
      if (!net.get_node_map().contains(resistor.get_source_node())
          || !net.get_node_map().contains(resistor.get_sink_node()))
        throw std::invalid_argument("pre-route RC resistor has an unknown endpoint");
      const auto source = root(node_ids.at(resistor.get_source_node()));
      const auto sink = root(node_ids.at(resistor.get_sink_node()));
      if (source == sink)
        throw std::invalid_argument("pre-route RC network contains a cycle");
      parents[source] = sink;
    }
    previous.emplace_back(name, decltype(parasitics)::node_type{});
  }
  parasitics = std::move(replacements);
  previous_parasitics = std::move(previous);
}
}  // namespace ista::timing_edit::detail
