// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "Database.hpp"
#include "NativeBufferCellValidation.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_edit::detail {
namespace {
[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("native buffer insertion: " + std::move(message));
}
bool isDriver(const Pin& pin)
{
  return pin.get_is_port() ? pin.get_direction() == PinDirection::kInput : pin.get_direction() == PinDirection::kOutput;
}

PinDirection requirePortDirection(const TimingCellPort& port, std::string_view role)
{
  if (role == "input" && port.get_is_input() && !port.get_is_output() && !port.get_is_clock()) {
    return PinDirection::kInput;
  }
  if (role == "output" && port.get_is_output() && !port.get_is_input() && !port.get_is_clock()) {
    return PinDirection::kOutput;
  }
  invalid("selected library " + std::string(role) + " port has an incompatible direction");
}

const PlacedInputs* placedInputs(const PlacementInputs& inputs)
{
  const auto* placed = std::get_if<PlacedInputs>(&inputs);
  if (placed != nullptr && (!std::isfinite(placed->x_microns) || !std::isfinite(placed->y_microns))) {
    invalid("placement coordinates must be finite");
  }
  return placed;
}

std::string requireSingleDriver(const Database& database, const Net& net)
{
  std::string driver;
  for (const auto& pin_name : net.get_pin_name_list()) {
    const auto pin = database.get_pin_map().find(pin_name);
    if (pin == database.get_pin_map().end()) {
      invalid("source net contains a missing pin");
    }
    if (isDriver(pin->second)) {
      if (!driver.empty()) {
        invalid("source net has multiple drivers");
      }
      driver = pin_name;
    }
  }
  if (driver.empty()) {
    invalid("source net has no driver");
  }
  return driver;
}

std::uint64_t nextInstanceIdImpl(const Database& database)
{
  std::uint64_t maximum = 0;
  for (const auto& [name, instance] : database.get_instance_map()) {
    static_cast<void>(name);
    maximum = std::max(maximum, instance.get_instance_id());
  }
  if (!database.get_instance_map().empty() && maximum == std::numeric_limits<std::uint64_t>::max()) {
    invalid("native instance ID capacity is exhausted");
  }
  return database.get_instance_map().empty() ? 0 : maximum + 1;
}

void requireAbsentMaterialization(const Database& database, const TokenMaterialization& materialization)
{
  if (database.get_instance_map().contains(materialization.instance_name)) {
    invalid("materialized instance name already exists");
  }
  if (database.get_net_map().contains(materialization.output_net_name)) {
    invalid("materialized output net name already exists");
  }
  if (database.get_pin_map().contains(materialization.input_pin_name) || database.get_pin_map().contains(materialization.output_pin_name)) {
    invalid("materialized pin name already exists");
  }
}

Instance makeInstance(const LibraryCellRef& reference, const TimingCell& cell, const TokenMaterialization& materialization, std::uint64_t instance_id)
{
  Instance instance;
  instance.set_instance_id(instance_id);
  instance.set_instance_name(materialization.instance_name);
  instance.set_cell_name(reference.cell_name);
  std::vector<std::string> pins{materialization.input_pin_name, materialization.output_pin_name};
  std::ranges::sort(pins);
  instance.set_pin_name_list(pins);
  instance.set_output_pin_name(materialization.output_pin_name);
  instance.set_is_sequential(cell.get_is_sequential());
  instance.set_is_clock_gating(cell.get_is_clock_gating());
  instance.set_has_clear_arc(cell.get_has_clear_arc());
  instance.set_has_preset_arc(cell.get_has_preset_arc());
  instance.set_is_io_cell(false);
  return instance;
}

Pin makePin(std::string full_name, std::string port_name, std::string instance_name, PinDirection direction, const PlacedInputs* placement)
{
  Pin pin;
  pin.set_pin_name(port_name);
  pin.set_full_name(full_name);
  pin.set_instance_name(instance_name);
  pin.set_direction(direction);
  pin.set_is_port(false);
  if (placement != nullptr) {
    pin.set_x(placement->x_microns);
    pin.set_y(placement->y_microns);
  }
  return pin;
}

void eraseExactlyOnce(std::vector<std::string>& names, const std::string& value)
{
  const auto count = std::ranges::count(names, value);
  if (count != 1) {
    invalid("moved sink does not appear exactly once on its source net");
  }
  names.erase(std::ranges::find(names, value));
}

}  // namespace

void requireSupportedMovedSink(const Database& database, const Pin& pin)
{
  if (pin.get_is_port()) {
    if (pin.get_direction() != PinDirection::kOutput)
      invalid("moved top-level sink must be an output port");
    return;
  }
  if (pin.get_direction() != PinDirection::kInput)
    invalid("moved instance sink must be an input pin");
  const auto found_instance = database.get_instance_map().find(pin.get_instance_name());
  if (found_instance == database.get_instance_map().end())
    invalid("moved sink instance does not exist");
  const auto& instance = found_instance->second;
  const auto found_cell = database.get_timing_library().get_cell_map().find(instance.get_cell_name());
  if (found_cell == database.get_timing_library().get_cell_map().end())
    invalid("moved sink cell does not exist in the timing library");
  const auto& cell = found_cell->second;
  if (instance.get_is_clock_gating() || cell.get_is_clock_gating() || cell.get_is_macro())
    invalid("clock-gating and macro sinks are outside the data-only boundary");
  if (instance.get_is_sequential() != cell.get_is_sequential())
    invalid("moved sink instance/library sequential roles disagree");
  const auto port = cell.get_port_map().find(pin.get_pin_name());
  if (port == cell.get_port_map().end() || port->second.get_is_clock()
      || pin.get_full_name() == instance.get_clock_pin_name())
    invalid("clock or unresolved library sink is outside the data-only boundary");
  if (!port->second.get_is_input() || port->second.get_is_output())
    invalid("moved sink library direction is not an input");
  for (const auto& arc : cell.get_cell_arc_list()) {
    if (arc.get_source_port() == pin.get_pin_name()
        && (arc.get_is_clear_preset_arc() || arc.get_is_clock_arc()))
      invalid("clock or clear/preset control sink is outside the data-only boundary");
  }
  if (!cell.get_is_sequential())
    return;
  // The native data role alone may come from a recovery check. Require a real
  // setup/hold data endpoint and reject contradictory control-check roles.
  bool ordinary_data_check = false;
  for (const auto& check : cell.get_check_arc_list()) {
    if (check.get_clock_port() == pin.get_pin_name())
      invalid("timing-check clock sink is outside the data-only boundary");
    if (check.get_data_port() != pin.get_pin_name())
      continue;
    if (check.get_check_type() != TimingCheckType::kSetup && check.get_check_type() != TimingCheckType::kHold)
      invalid("sequential sink has a control timing-check role");
    ordinary_data_check = true;
  }
  if (instance.get_data_pin_name() != pin.get_full_name() || !ordinary_data_check)
    invalid("moved sink is not an ordinary sequential data input");
}

std::uint64_t nextNativeInstanceId(const Database& database)
{
  return nextInstanceIdImpl(database);
}

void applyNativeBufferInsertion(Database& database, const BufferInsertionMutation& edit, const TimingEditResolverState& resolver, std::uint64_t instance_id)
{
  const auto& materialization = resolver.materialization(edit.creation_token);
  requireAbsentMaterialization(database, materialization);
  if (materialization.input_pin_name != materialization.instance_name + ":" + edit.library_cell.input_port
      || materialization.output_pin_name != materialization.instance_name + ":" + edit.library_cell.output_port) {
    invalid("materialized pin names do not match the selected library ports");
  }

  const std::string source_net_name = resolver.resolveNet(edit.original_net);
  auto source_net = database.get_net_map().find(source_net_name);
  if (source_net == database.get_net_map().end()) {
    invalid("source net does not exist: " + source_net_name);
  }
  if (database.get_parasitic_library().get_net_map().contains(source_net_name)) {
    invalid("NoParasitic input disagrees with the candidate parasitic library");
  }
  const std::string source_driver = requireSingleDriver(database, source_net->second);

  if (edit.moved_sinks.empty()) {
    invalid("buffer insertion must move at least one sink");
  }
  std::vector<std::string> moved_sinks;
  moved_sinks.reserve(edit.moved_sinks.size());
  std::set<std::string> unique_sinks;
  for (const auto& sink_ref : edit.moved_sinks) {
    const std::string sink_name = resolver.resolveNode(sink_ref);
    if (!unique_sinks.insert(sink_name).second) {
      invalid("moved sink list contains a duplicate");
    }
    const auto sink = database.get_pin_map().find(sink_name);
    if (sink == database.get_pin_map().end() || sink->second.get_net_name() != source_net_name || isDriver(sink->second) || sink_name == source_driver) {
      invalid("moved sink is not a load on the source net");
    }
    requireSupportedMovedSink(database, sink->second);
    moved_sinks.push_back(sink_name);
  }

  const TimingCell& cell = requireLegalNativeBufferCell(database, edit.library_cell);
  const auto input_port = cell.get_port_map().find(edit.library_cell.input_port);
  const auto output_port = cell.get_port_map().find(edit.library_cell.output_port);
  const PlacedInputs* placement = placedInputs(edit.placement_inputs);

  Instance instance = makeInstance(edit.library_cell, cell, materialization, instance_id);
  Pin input_pin = makePin(materialization.input_pin_name, edit.library_cell.input_port, materialization.instance_name,
                          requirePortDirection(input_port->second, "input"), placement);
  Pin output_pin = makePin(materialization.output_pin_name, edit.library_cell.output_port, materialization.instance_name,
                           requirePortDirection(output_port->second, "output"), placement);
  input_pin.set_net_name(source_net_name);
  output_pin.set_net_name(materialization.output_net_name);

  for (const auto& sink_name : moved_sinks) {
    eraseExactlyOnce(source_net->second.get_pin_name_list(), sink_name);
  }
  source_net->second.get_pin_name_list().push_back(materialization.input_pin_name);

  Net output_net;
  output_net.set_net_name(materialization.output_net_name);
  std::vector<std::string> output_members{materialization.output_pin_name};
  output_members.insert(output_members.end(), moved_sinks.begin(), moved_sinks.end());
  output_net.set_pin_name_list(output_members);

  database.get_instance_map().emplace(materialization.instance_name, std::move(instance));
  database.get_pin_map().emplace(materialization.input_pin_name, std::move(input_pin));
  database.get_pin_map().emplace(materialization.output_pin_name, std::move(output_pin));
  database.get_net_map().emplace(materialization.output_net_name, std::move(output_net));
  for (const auto& sink_name : moved_sinks) {
    database.get_pin_map().at(sink_name).set_net_name(materialization.output_net_name);
  }
}

}  // namespace ista::timing_edit::detail
