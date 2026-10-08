#include "PreRouteRc.hpp"
#include "PreRouteRcTestSupport.hpp"
#include "IdbLayout.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
using izh::test::rcValues;
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected)
{
  require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-12, "RC numeric mismatch");
}
template<class F> void reject(F&& action)
{
  try { action(); } catch (const std::invalid_argument&) { return; }
  throw std::runtime_error("invalid RC input was accepted");
}
void connected(const ista::ParasiticNet& net, std::span<const izh::PreRouteTerminal> pins)
{
  require(net.get_resistor_list().size() + 1 == net.get_node_map().size(), "RC edge count mismatch");
  std::set<std::string> reached{pins.front().name};
  for (std::size_t pass = 0; pass < net.get_node_map().size(); ++pass)
    for (const auto& edge : net.get_resistor_list()) {
      require(std::isfinite(edge.get_resistance()) && edge.get_resistance() >= 0, "invalid resistance");
      require(net.get_node_map().contains(edge.get_source_node()) && net.get_node_map().contains(edge.get_sink_node()),
              "edge references an absent node");
      if (reached.contains(edge.get_source_node())) reached.insert(edge.get_sink_node());
      if (reached.contains(edge.get_sink_node())) reached.insert(edge.get_source_node());
    }
  require(reached.size() == net.get_node_map().size(), "disconnected RC tree");
  double total = 0;
  for (const auto& [name, node] : net.get_node_map()) {
    require(name == node.get_node_name() && node.get_capacitance() >= 0, "invalid RC node");
    total += node.get_capacitance();
  }
  near(total, net.get_lumped_capacitance());
  for (const auto& pin : pins) {
    near(net.get_node_map().at(pin.name).get_x(), pin.x_dbu / 1000.0);
    near(net.get_node_map().at(pin.name).get_y(), pin.y_dbu / 1000.0);
  }
}
void run()
{
  idb::IdbLayout layout;
  layout.set_units(new idb::IdbUnits());
  layout.get_units()->set_microns_dbu(1000);
  const auto addLayer = [&](const std::string& name, idb::IdbLayerDirection direction, int width) {
    auto* layer = dynamic_cast<idb::IdbLayerRouting*>(layout.get_layers()->set_layer(name, "ROUTING"));
    require(layer != nullptr, "routing layer creation failed");
    layout.get_layers()->add_routing_layer(layer);
    layer->set_direction(direction); layer->set_width(width);
    layer->set_resistance(0.2); layer->set_capacitance(0.01); layer->set_edge_capacitance(0.001);
    return layer;
  };
  auto* h = addLayer("M1", idb::IdbLayerDirection::kHorizontal, 200);
  addLayer("M2", idb::IdbLayerDirection::kVertical, 400);
  addLayer("M3", idb::IdbLayerDirection::kHorizontal, 800);
  const auto technology = [&] { return izh::preparePreRouteRcTechnology(layout, 1000, "M1", "M3"); };
  const auto rc = technology();
  require(rc.horizontal.name == "M1" && rc.vertical.name == "M2", "wrong default RC layers");
  near(rc.horizontal.resistance_per_um, 1); near(rc.vertical.resistance_per_um, 0.5);
  near(rc.horizontal.area_capacitance_per_um, 0.002);
  require(izh::preparePreRouteRcTechnology(layout, 1000, "M1", "M3", "M3").horizontal.name == "M3",
          "explicit layer ignored");
  reject([&] { (void) izh::preparePreRouteRcTechnology(layout, 2000, "M1", "M3"); });
  reject([&] { (void) izh::preparePreRouteRcTechnology(layout, 1000, "M2", "M3", "M1"); });
  reject([&] { (void) izh::preparePreRouteRcTechnology(layout, 1000, "M1", "M3", "M2"); });
  reject([&] { (void) izh::preparePreRouteRcTechnology(layout, 1000, "M3", "M1"); });
  h->set_edge_capacitance(-1); reject([&] { (void) technology(); }); h->set_edge_capacitance(0.001);
  h->set_resistance(std::numeric_limits<double>::quiet_NaN());
  reject([&] { (void) technology(); }); h->set_resistance(0.2);
  idb::IdbLayerRouting absent;
  require(absent.get_edge_capacitance() < 0, "missing edge capacitance lacks a defined marker");

  const auto build = [&](const std::vector<izh::PreRouteTerminal>& pins) { return izh::buildPreRouteRcNet("n", pins, rc); };
  for (const auto vertical : {false, true}) {
    const std::vector<izh::PreRouteTerminal> pins{{"driver", 0, 0}, {"sink", vertical ? 0 : 10000, vertical ? 10000 : 0}};
    const auto net = build(pins); connected(net, pins);
    double resistance = 0; for (const auto& edge : net.get_resistor_list()) resistance += edge.get_resistance();
    near(resistance, vertical ? 5 : 10);
    near(net.get_lumped_capacitance(), vertical ? 0.0608 : 0.0404);
  }
  std::vector<izh::PreRouteTerminal> pins{{"driver", 0, 0}, {"sink", 10000, 10000}};
  auto diagonal = build(pins); connected(diagonal, pins);
  double resistance = 0; for (const auto& edge : diagonal.get_resistor_list()) resistance += edge.get_resistance();
  near(resistance, 15); near(diagonal.get_lumped_capacitance(), 0.1012);
  pins = {{"driver", 0, 0}, {"b", 0, 0}, {"a", 0, 0}};
  auto coincident = build(pins); connected(coincident, pins); near(coincident.get_lumped_capacitance(), 0);
  for (const auto& edge : coincident.get_resistor_list()) near(edge.get_resistance(), 0);
  pins.resize(1); auto single = build(pins); connected(single, pins); near(single.get_lumped_capacitance(), 0);
  pins = {{"driver", 0, 0}, {"d", 10000, 0}, {"b", 0, 10000}, {"c", 5000, 6000}, {"a", 5000, 6000}};
  auto tree = build(pins); connected(tree, pins);
  std::reverse(pins.begin() + 1, pins.end());
  require(rcValues(build(pins)) == rcValues(tree), "load input order changed RC");
  const std::vector<izh::PreRouteTerminal> reserved{{"steiner_3", 0, 0}, {"terminal_0", 10000, 10000}, {"n:__incregpusta_rc_0", 20000, 0}};
  connected(build(reserved), reserved);
  pins.push_back(pins.back()); reject([&] { (void) build(pins); });
  pins = {{"driver", std::numeric_limits<int>::min(), 0}, {"sink", std::numeric_limits<int>::max(), 0}};
  reject([&] { (void) build(pins); });
  reject([&] { (void) build({}); });
}
}
int main()
{
  try { run(); std::cout << "pre-route RC units, geometry and identity passed\n"; }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
