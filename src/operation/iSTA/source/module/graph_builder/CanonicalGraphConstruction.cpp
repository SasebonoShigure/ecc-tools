// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#include "CanonicalGraphConstruction.hpp"

#include <algorithm>
#include <utility>

#include "Logger.hpp"
namespace ista::canonical_graph {
std::size_t appendArc(Database& database, Arc arc)
{
  const auto index = database.get_arc_list().size();
  database.get_outgoing_arc_list_map()[arc.get_source_pin()].push_back(index);
  database.get_incoming_arc_list_map()[arc.get_sink_pin()].push_back(index);
  database.get_arc_list().push_back(std::move(arc));
  return index;
}
Arc makeArc(const std::string& source_pin, const std::string& sink_pin, ArcType type, const std::string& owner_name, const std::string& library_source_port,
            const std::string& library_sink_port, bool is_clock_arc, bool is_disable_arc, TimingCellArc* timing_cell_arc)
{
  Arc arc;
  arc.set_arc_name(owner_name + ":" + source_pin + "->" + sink_pin);
  arc.set_source_pin(source_pin);
  arc.set_sink_pin(sink_pin);
  arc.set_owner_name(owner_name);
  arc.set_library_source_port(library_source_port);
  arc.set_library_sink_port(library_sink_port);
  arc.set_type(type);
  arc.set_is_clock_arc(is_clock_arc);
  arc.set_is_disable_arc(is_disable_arc);
  arc.set_timing_cell_arc(timing_cell_arc);

  return arc;
}

void addCellArc(Database& database, Instance& instance, TimingCellArc& timing_cell_arc)
{
  std::string source_pin = instance.get_instance_name() + ":" + timing_cell_arc.get_source_port();
  std::string sink_pin = instance.get_instance_name() + ":" + timing_cell_arc.get_sink_port();
  if (database.get_pin_map().count(source_pin) == 0 || database.get_pin_map().count(sink_pin) == 0) {
    return;
  }
  bool is_disable_arc = timing_cell_arc.get_is_disable_arc();
  appendArc(database, makeArc(source_pin, sink_pin, ArcType::kCell, instance.get_instance_name(), timing_cell_arc.get_source_port(),
                              timing_cell_arc.get_sink_port(), timing_cell_arc.get_is_clock_arc(), is_disable_arc, &timing_cell_arc));
}

bool appendLibraryCellArcs(Database& database, Instance& instance)
{
  std::map<std::string, TimingCell>& timing_cell_map = database.get_timing_library().get_cell_map();
  if (timing_cell_map.count(instance.get_cell_name()) == 0) {
    return false;
  }

  TimingCell& timing_cell = timing_cell_map[instance.get_cell_name()];
  if (timing_cell.get_cell_arc_list().empty()) {
    return false;
  }

  for (TimingCellArc& timing_cell_arc : timing_cell.get_cell_arc_list()) {
    if (!timing_cell_arc.get_is_timing_graph_arc()) {
      continue;
    }
    addCellArc(database, instance, timing_cell_arc);
  }
  return true;
}

bool shouldDisableNetArc(Database& database, const std::string& source_pin, const std::string& sink_pin)
{
  Pin& source_pin_inst = database.get_pin_map()[source_pin];
  Pin& sink_pin_inst = database.get_pin_map()[sink_pin];
  if (source_pin_inst.get_is_port() || sink_pin_inst.get_is_port()) {
    return false;
  }
  if (database.get_instance_map().count(source_pin_inst.get_instance_name()) == 0
      || database.get_instance_map().count(sink_pin_inst.get_instance_name()) == 0) {
    return false;
  }
  Instance& source_instance = database.get_instance_map()[source_pin_inst.get_instance_name()];
  Instance& sink_instance = database.get_instance_map()[sink_pin_inst.get_instance_name()];
  std::map<std::string, TimingCell>& timing_cell_map = database.get_timing_library().get_cell_map();
  if (timing_cell_map.count(source_instance.get_cell_name()) == 0) {
    return false;
  }
  TimingCell& source_cell = timing_cell_map[source_instance.get_cell_name()];
  return source_cell.get_is_sequential() && !source_cell.get_is_clock_gating() && !source_cell.get_is_macro() && sink_pin == sink_instance.get_clock_pin_name();
}

Arc makeNetArc(Database& database, const std::string& source, const std::string& sink, ArcType type, const std::string& owner)
{
  return makeArc(source, sink, type, owner, {}, {}, false, shouldDisableNetArc(database, source, sink), nullptr);
}
void refreshNetDriverLoad(Database& database, Net& net)
{
  net.get_driver_pin().clear();
  net.get_driver_pin_list().clear();
  net.get_load_pin_list().clear();

  for (std::string& pin_name : net.get_pin_name_list()) {
    Pin& pin = database.get_pin_map()[pin_name];
    if (pin.get_is_port() ? pin.get_direction() == PinDirection::kInput : pin.get_direction() == PinDirection::kOutput) {
      net.set_driver_pin(pin_name);
      net.get_driver_pin_list().push_back(pin_name);
    } else {
      net.get_load_pin_list().push_back(pin_name);
    }
  }

  std::ranges::sort(net.get_driver_pin_list());
  std::ranges::sort(net.get_load_pin_list());

  if (net.get_driver_pin_list().size() > 1) {
    std::string drivers;
    for (const auto& name : net.get_driver_pin_list()) {
      if (!drivers.empty())
        drivers += ", ";
      drivers += name;
    }
    STALOG.error(Loc::current(), "The net has multiple driver pins: net=", net.get_net_name(), " drivers=", drivers);
  }
}

}  // namespace ista::canonical_graph
