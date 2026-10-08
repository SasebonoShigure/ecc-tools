// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PreRouteRc.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include "FLUTERouter.hh"
#include "IdbLayout.h"

namespace izh {
namespace {
void demand(bool value, const char* message)
{
  if (!value) throw std::invalid_argument(message);
}
bool nonnegative(double value) { return std::isfinite(value) && value >= 0.0; }
void requireLayer(const PreRouteLayerRc& layer)
{
  demand(!layer.name.empty() && std::isfinite(layer.width_um) && layer.width_um > 0.0
      && nonnegative(layer.resistance_per_um) && nonnegative(layer.area_capacitance_per_um)
      && nonnegative(layer.edge_capacitance_per_um), "pre-route RC layer parameters are missing or invalid");
}
}  // namespace

PreRouteRcTechnology preparePreRouteRcTechnology(idb::IdbLayout& layout, std::int32_t design_dbu,
    const std::string& bottom, const std::string& top, const std::string& horizontal, const std::string& vertical)
{
  demand(layout.get_units() && layout.get_layers(), "pre-route RC requires LEF units and layers");
  const auto dbu = layout.get_units()->get_micron_dbu();
  demand(dbu > 0 && design_dbu == dbu, "pre-route RC requires matching positive DEF/LEF DBU");
  const auto& layers = layout.get_layers()->get_routing_layers();
  const auto find = [&](const std::string& name) {
    const auto found = std::ranges::find_if(layers, [&](auto* layer) { return layer && layer->get_name() == name; });
    demand(found != layers.end(), "pre-route RC layer is outside the routing stack");
    return static_cast<std::size_t>(found - layers.begin());
  };
  const auto first = find(bottom), last = find(top);
  demand(first <= last, "pre-route RC routing interval is reversed");
  const auto select = [&](const std::string& requested, idb::IdbLayerDirection direction) {
    idb::IdbLayerRouting* selected = nullptr;
    if (!requested.empty()) {
      const auto index = find(requested);
      demand(index >= first && index <= last, "pre-route RC override is outside routing interval");
      selected = dynamic_cast<idb::IdbLayerRouting*>(layers[index]);
    } else {
      for (auto index = first; index <= last; ++index) {
        auto* candidate = dynamic_cast<idb::IdbLayerRouting*>(layers[index]);
        if (candidate && candidate->get_direction() == direction) { selected = candidate; break; }
      }
    }
    demand(selected && selected->get_direction() == direction, "pre-route RC layer direction is unavailable");
    const double width = static_cast<double>(selected->get_width()) / dbu;
    demand(width > 0.0 && nonnegative(selected->get_resistance()) && nonnegative(selected->get_capacitance())
        && nonnegative(selected->get_edge_capacitance()), "pre-route RC technology has missing width/R/C");
    // LEF RPERSQ is ohms/square, CPERSQDIST pF/um^2 and EDGECAPACITANCE
    // pF/um. UNITS precision is not an additional physical scale factor.
    PreRouteLayerRc result{selected->get_name(), width, selected->get_resistance() / width,
                          selected->get_capacitance() * width, selected->get_edge_capacitance()};
    requireLayer(result);
    return result;
  };
  return {dbu, select(horizontal, idb::IdbLayerDirection::kHorizontal), select(vertical, idb::IdbLayerDirection::kVertical)};
}

ista::ParasiticNet buildPreRouteRcNet(const std::string& name, std::span<const PreRouteTerminal> terminals,
                                     const PreRouteRcTechnology& technology)
{
  demand(!name.empty() && !terminals.empty() && technology.dbu_per_um > 0, "pre-route RC has no net/driver or valid DBU");
  requireLayer(technology.horizontal);
  requireLayer(technology.vertical);
  using Position = std::pair<std::int32_t, std::int32_t>;
  std::map<std::string, const PreRouteTerminal*> by_name;
  std::map<Position, std::vector<const PreRouteTerminal*>> positions;
  auto xmin = terminals.front().x_dbu, xmax = xmin, ymin = terminals.front().y_dbu, ymax = ymin;
  for (const auto& terminal : terminals) {
    demand(!terminal.name.empty() && by_name.emplace(terminal.name, &terminal).second, "pre-route RC terminal names must be unique");
    xmin = std::min(xmin, terminal.x_dbu); xmax = std::max(xmax, terminal.x_dbu);
    ymin = std::min(ymin, terminal.y_dbu); ymax = std::max(ymax, terminal.y_dbu);
  }
  for (const auto& [key, terminal] : by_name) {
    static_cast<void>(key);
    positions[{terminal->x_dbu, terminal->y_dbu}].push_back(terminal);
  }
  const auto span = std::int64_t{xmax} - xmin + std::int64_t{ymax} - ymin;
  demand(positions.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max() / 2)
      && (positions.size() <= 1 || span <= std::numeric_limits<int>::max() / static_cast<std::int64_t>(positions.size() - 1)),
      "pre-route RC exceeds FLUTE integer geometry capacity");
  const Position driver_position{terminals.front().x_dbu, terminals.front().y_dbu};
  const auto representative = [&](const Position& position) -> const PreRouteTerminal& {
    return position == driver_position ? terminals.front() : *positions.at(position).front();
  };
  // FLUTERouter reserves "steiner_<id>" names internally. Pass only local
  // terminal names, then restore physical pin identities on the returned tree.
  std::map<std::string, const PreRouteTerminal*> flute_names;
  const auto fluteTerminal = [&](const PreRouteTerminal& terminal) {
    icts::ClockRoutingTerminal result;
    result.name = "terminal_" + std::to_string(flute_names.size());
    flute_names.emplace(result.name, &terminal);
    result.location = icts::Point<int>(terminal.x_dbu, terminal.y_dbu);
    return result;
  };
  std::vector<icts::ClockRoutingTerminal> loads;
  for (const auto& [position, pins] : positions) {
    static_cast<void>(pins);
    if (position != driver_position) loads.push_back(fluteTerminal(representative(position)));
  }
  // FLUTE's coordinate map keeps one pin per position. Deduplicate coordinates,
  // never pin identities or real pin locations; zero-R edges restore co-located pins.
  const auto tree = icts::FLUTERouter::buildTree(fluteTerminal(terminals.front()), loads);
  ista::ParasiticNet result;
  result.set_net_name(name);
  const double dbu = technology.dbu_per_um;
  const auto addNode = [&](const std::string& node_name, int x, int y) {
    auto& node = result.get_node_map()[node_name];
    node.set_node_name(node_name);
    node.set_x(x / dbu);
    node.set_y(y / dbu);
  };
  for (const auto& terminal : terminals) addNode(terminal.name, terminal.x_dbu, terminal.y_dbu);
  std::size_t next_id = 0;
  const auto internalName = [&]() {
    std::string value;
    do { value = name + ":__incregpusta_rc_" + std::to_string(next_id++); }
    while (result.get_node_map().contains(value));
    return value;
  };
  std::vector<std::string> node_names;
  for (const auto& node : tree.get_nodes()) {
    std::string node_name;
    if (node.is_terminal) {
      const auto found = flute_names.find(node.name);
      demand(found != flute_names.end() && found->second->x_dbu == node.location.get_x()
          && found->second->y_dbu == node.location.get_y(), "FLUTE changed a terminal identity or coordinate");
      node_name = found->second->name;
    } else node_name = internalName();
    addNode(node_name, node.location.get_x(), node.location.get_y());
    node_names.push_back(std::move(node_name));
  }
  double total_capacitance = 0.0;
  const auto edge = [&](const std::string& from, const std::string& to, double length, const PreRouteLayerRc& layer) {
    const double resistance = length * layer.resistance_per_um;
    const double capacitance = length == 0.0 ? 0.0 :
        length * layer.area_capacitance_per_um + 2.0 * (length + layer.width_um) * layer.edge_capacitance_per_um;
    demand(nonnegative(resistance) && nonnegative(capacitance), "pre-route RC segment overflow");
    auto& a = result.get_node_map().at(from);
    auto& b = result.get_node_map().at(to);
    const double ca = a.get_capacitance() + capacitance / 2.0, cb = b.get_capacitance() + capacitance / 2.0;
    demand(std::isfinite(ca) && std::isfinite(cb) && std::isfinite(total_capacitance + capacitance), "pre-route RC capacitance overflow");
    a.set_capacitance(ca); b.set_capacitance(cb);
    total_capacitance += capacitance;
    auto& resistor = result.get_resistor_list().emplace_back();
    resistor.set_source_node(from); resistor.set_sink_node(to); resistor.set_resistance(resistance);
  };
  for (const auto& connection : tree.get_edges()) {
    const auto& a = tree.get_nodes().at(connection.source_node_id).location;
    const auto& b = tree.get_nodes().at(connection.target_node_id).location;
    const auto& from = node_names.at(connection.source_node_id);
    const auto& to = node_names.at(connection.target_node_id);
    const double horizontal = std::abs(std::int64_t{a.get_x()} - b.get_x()) / dbu;
    const double vertical = std::abs(std::int64_t{a.get_y()} - b.get_y()) / dbu;
    if (horizontal > 0.0 && vertical > 0.0) {
      const auto corner = internalName();
      addNode(corner, b.get_x(), a.get_y());
      edge(from, corner, horizontal, technology.horizontal);
      edge(corner, to, vertical, technology.vertical);
    } else edge(from, to, horizontal + vertical, horizontal > 0.0 ? technology.horizontal : technology.vertical);
  }
  for (const auto& [position, pins] : positions)
    for (const auto* pin : pins)
      if (pin->name != representative(position).name)
        edge(representative(position).name, pin->name, 0.0, technology.horizontal);
  demand(result.get_resistor_list().size() + 1 == result.get_node_map().size(), "pre-route RC did not produce a tree");
  result.set_lumped_capacitance(total_capacitance);
  return result;
}
}  // namespace izh
