// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// Explicit ON-only readback of the committed production result. No propagation
// or reference computation is performed by this test binding.
#include "StaTimingEditSessionProbeAccess.hpp"
#include "StaEditOperation.hpp"
#include "PhysicalStaSessionState.hpp"
#include "CsrRealDifferentialOracleEncoding.hpp"
#include "DataManager.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include <algorithm>

static_assert(ista::incregpusta_debug_checks_enabled);
namespace ista {
namespace {
using csr::real_diff::Json;
Json levelEdgesJson(const Database& database)
{
  const auto& points = database.get_timing_point_map();
  std::int32_t previous = -1;
  for (const auto& name : database.get_timing_order_list()) {
    const auto level = points.at(name).get_level();
    if (level < 0 || level < previous) throw std::runtime_error("diagnostic: materialized level order is not ascending");
    previous = level;
  }
  Json edges = Json::array();
  for (const auto& arc : database.get_arc_list()) {
    if (arc.get_is_disable_arc() || arc.get_is_loop_disable()) continue;
    if (points.at(arc.get_source_pin()).get_level() >= points.at(arc.get_sink_pin()).get_level())
      throw std::runtime_error("diagnostic: active edge violates materialized levels");
    edges.push_back({arc.get_source_pin(), arc.get_sink_pin()});
  }
  std::sort(edges.begin(), edges.end());
  return edges;
}
Json physicalStateJson(idb::IdbDesign& design)
{
  if (!design.validateConnectivity().ok)
    throw std::runtime_error("diagnostic: physical connectivity is inconsistent");
  Json instances = Json::object(), ports = Json::object(), nets = Json::object();
  const auto pin = [](idb::IdbPin* value) {
    if (!value || !value->get_average_coordinate()) throw std::runtime_error("diagnostic: incomplete physical pin");
    return Json{{"instance", value->get_instance() ? value->get_instance()->get_name() : ""},
                {"pin", value->get_pin_name()},
                {"x", value->get_average_coordinate()->get_x()}, {"y", value->get_average_coordinate()->get_y()}};
  };
  for (auto* instance : design.get_instance_list()->get_instance_list()) {
    if (!instance || !instance->get_cell_master() || !instance->get_coordinate())
      throw std::runtime_error("diagnostic: incomplete physical instance");
    instances[instance->get_name()] = {
      {"master", instance->get_cell_master()->get_name()},
      {"x", instance->get_coordinate()->get_x()}, {"y", instance->get_coordinate()->get_y()},
      {"orientation", static_cast<int>(instance->get_orient())}};
  }
  for (auto* port : design.get_io_pin_list()->get_pin_list()) {
    if (!port || !port->get_average_coordinate())
      throw std::runtime_error("diagnostic: incomplete physical port");
    ports[port->get_pin_name()] = {port->get_average_coordinate()->get_x(), port->get_average_coordinate()->get_y()};
  }
  for (auto* net : design.get_net_list()->get_net_list()) {
    if (!net) throw std::runtime_error("diagnostic: null physical net");
    Json pins = Json::array();
    for (auto* value : net->get_instance_pin_list()->get_pin_list()) pins.push_back(pin(value));
    for (auto* value : net->get_io_pins()->get_pin_list()) pins.push_back(pin(value));
    std::sort(pins.begin(), pins.end());
    nets[net->get_net_name()] = std::move(pins);
  }
  return {{"instances", std::move(instances)}, {"ports", std::move(ports)}, {"nets", std::move(nets)}};
}
Json pathChecksJson(Database& database)
{
  Json groups = Json::object();
  const auto number = csr::real_diff::numberToJson;
  for (auto& group : database.get_timing_path_group_list()) {
    Json ends = Json::object();
    for (auto& [end, paths] : group.get_timing_path_end_map()) {
      Json values = Json::array();
      for (auto& path : paths.get_timing_path_list()) {
        Json points = Json::array();
        for (auto& point : path.get_point_list())
          points.push_back({{"pin", point.get_pin_name()}, {"source", point.get_source_pin()}, {"sink", point.get_sink_pin()},
              {"transition", static_cast<int>(point.get_trans_type())}, {"arrival", number(point.get_arrival())},
              {"required", number(point.get_required())}, {"slack", number(point.get_slack())}, {"delay", number(point.get_arc_delay())}});
        values.push_back({{"start", path.get_start_point()}, {"end", path.get_end_point()},
            {"analysis", static_cast<int>(path.get_analysis_type())}, {"source", static_cast<int>(path.get_source_type())},
            {"transition", static_cast<int>(path.get_trans_type())}, {"clock", path.get_clock_name()},
            {"check_type", static_cast<int>(path.get_check_type())}, {"check_time", number(path.get_check_time())},
            {"setup_time", number(path.get_setup_time())}, {"delay", number(path.get_path_delay())},
            {"required", number(path.get_required_time())}, {"slack", number(path.get_slack())},
            {"cell_delay", number(path.get_cell_delay())}, {"net_delay", number(path.get_net_delay())},
            {"launch", number(path.get_launch_time())}, {"capture", number(path.get_capture_time())},
            {"launch_clock_delay", number(path.get_launch_clock_network_delay())},
            {"capture_clock_delay", number(path.get_capture_clock_network_delay())},
            {"capture_clock_pin", path.get_capture_clock_pin()}, {"last_common_pin", path.get_last_common_pin()},
            {"crpr", number(path.get_clock_reconvergence_pessimism())}, {"points", std::move(points)}});
      }
      ends[end] = std::move(values);
    }
    if (!groups.emplace(group.get_group_name(), std::move(ends)).second)
      throw std::runtime_error("diagnostic: duplicate path group name");
  }
  return groups;
}
Json parasiticsJson(const Database& database)
{
  const auto& library = database.get_parasitic_library();
  Json nets = Json::object();
  for (const auto& [name, net] : library.get_net_map()) {
    Json nodes = Json::object(), resistors = Json::array();
    for (const auto& [key, node] : net.get_node_map())
      nodes[key] = {{"name", node.get_node_name()}, {"capacitance", csr::real_diff::numberToJson(node.get_capacitance())},
                    {"x", csr::real_diff::numberToJson(node.get_x())}, {"y", csr::real_diff::numberToJson(node.get_y())}};
    for (const auto& resistor : net.get_resistor_list())
      resistors.push_back({resistor.get_source_node(), resistor.get_sink_node(), csr::real_diff::numberToJson(resistor.get_resistance())});
    nets[name] = {{"name", net.get_net_name()}, {"total_capacitance", csr::real_diff::numberToJson(net.get_lumped_capacitance())},
                  {"nodes", std::move(nodes)}, {"resistors", std::move(resistors)}};
  }
  return {{"capacitance_unit", library.get_capacitive_unit()},
          {"resistance_unit", library.get_resistance_unit()}, {"nets", std::move(nets)}};
}
}
std::string StaTimingEditSessionProbeAccess::physicalDiagnostic(IncrementalStaSession& session)
{
  StaEditOperation operation(session);
  auto& context = operation.context();
  if (!context._physical)
    throw std::invalid_argument("diagnostic requires a committed physical baseline");
  auto& state = *context._physical;
  auto& database = STADM.getDatabase();
  state.prepareReport(database, context, operation.authority());
  return Json{{"schema_version", "physical-sta-diagnostic/2.0.0"},
              {"active_level_edges", levelEdgesJson(database)},
              {"generation", state.generation(context).value()},
              {"physical", physicalStateJson(state.design)}, {"parasitics", parasiticsJson(database)},
              {"path_checks", pathChecksJson(database)},
              {"timing", csr::real_diff::neutralWireToJson(csr::real_diff::readAndEncodeNativeTiming(database))}}.dump();
}
}
