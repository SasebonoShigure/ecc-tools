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

template <class PinLookup>
std::string requireSingleDriver(const Net& net, const PinLookup& pin_for)
{
  std::string driver;
  for (const auto& name : net.get_pin_name_list()) {
    if (isDriver(pin_for(name))) {
      if (!driver.empty())
        invalid("source net has multiple drivers");
      driver = name;
    }
  }
  if (driver.empty())
    invalid("source net has no driver");
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
  if (instance.get_is_clock_gating())
    invalid("clock-gating sink is outside the data-only boundary");
  csr::NodeRecord node;
  node.key.pin_name = pin.get_pin_name();
  node.direction = csr::PinDirection::kInput;
  node.is_sequential = instance.get_is_sequential();
  node.is_clock_pin = pin.get_full_name() == instance.get_clock_pin_name();
  node.is_data_pin = pin.get_full_name() == instance.get_data_pin_name();
  requireSupportedMovedSink(node, cell);
}

void requireSupportedMovedSink(const csr::NodeRecord& pin, const TimingCell& cell)
{
  if (pin.is_port || pin.direction != csr::PinDirection::kInput)
    invalid("moved CSR instance sink must be an input pin");
  if (cell.get_is_clock_gating() || cell.get_is_macro())
    invalid("clock-gating and macro sinks are outside the data-only boundary");
  if (pin.is_sequential != cell.get_is_sequential())
    invalid("moved sink instance/library sequential roles disagree");
  const auto port = cell.get_port_map().find(pin.key.pin_name);
  if (port == cell.get_port_map().end() || port->second.get_is_clock()
      || pin.is_clock_pin)
    invalid("clock or unresolved library sink is outside the data-only boundary");
  if (!port->second.get_is_input() || port->second.get_is_output())
    invalid("moved sink library direction is not an input");
  for (const auto& arc : cell.get_cell_arc_list()) {
    if (arc.get_source_port() == pin.key.pin_name
        && (arc.get_is_clear_preset_arc() || arc.get_is_clock_arc()))
      invalid("clock or clear/preset control sink is outside the data-only boundary");
  }
  if (!cell.get_is_sequential())
    return;
  // The native data role alone may come from a recovery check. Require a real
  // setup/hold data endpoint and reject contradictory control-check roles.
  bool ordinary_data_check = false;
  for (const auto& check : cell.get_check_arc_list()) {
    if (check.get_clock_port() == pin.key.pin_name)
      invalid("timing-check clock sink is outside the data-only boundary");
    if (check.get_data_port() != pin.key.pin_name)
      continue;
    if (check.get_check_type() != TimingCheckType::kSetup && check.get_check_type() != TimingCheckType::kHold)
      invalid("sequential sink has a control timing-check role");
    ordinary_data_check = true;
  }
  if (!pin.is_data_pin || !ordinary_data_check)
    invalid("moved sink is not an ordinary sequential data input");
}

namespace {
void requireDataNetCondition(bool value, const char* message)
{
  if (!value)
    invalid(message);
}
}  // namespace
void requireDataOnlyNet(const Database& database, const std::string& name)
{
  const auto& net = database.get_net_map().at(name);
  requireDataNetCondition(net.get_driver_pin_list().size() == 1 && !net.get_driver_pin().empty(), "targeted source requires one driver");
  for (const auto& [clock_name, clock] : database.get_timing_constraint().get_clock_map()) {
    static_cast<void>(clock_name);
    for (const auto& source : clock.get_source_list())
      requireDataNetCondition(std::ranges::find(net.get_pin_name_list(), source) == net.get_pin_name_list().end(), "targeted edit touches clock source");
  }
  for (const auto& name : net.get_pin_name_list()) {
    const auto& pin = database.get_pin_map().at(name);
    requireDataNetCondition(pin.get_net_name() == net.get_net_name(), "targeted net membership mismatch");
    if (pin.get_is_port())
      continue;
    const auto& instance = database.get_instance_map().at(pin.get_instance_name());
    const auto& cell = database.get_timing_library().get_cell_map().at(instance.get_cell_name());
    requireDataNetCondition(!cell.get_port_map().at(pin.get_pin_name()).get_is_clock(), "targeted edit touches library clock pin");
  }
}
std::uint64_t nextNativeInstanceId(const Database& database)
{
  return nextInstanceIdImpl(database);
}

PreparedNativeBufferInsertions prepareNativeBufferInsertions(
    Database& database, std::span<const BufferInsertionMutation> edits,
    TimingEditResolverView resolver, std::uint64_t instance_id,
    std::map<std::string, ParasiticNet> prepared_rc)
{
  PreparedNativeBufferInsertions prepared;
  prepared.database = &database;
  const auto pin_for = [&](const std::string& name) -> const Pin& {
    const auto added = prepared.pins.find(name);
    if (added != prepared.pins.end())
      return added->second;
    const auto existing = database.get_pin_map().find(name);
    if (existing == database.get_pin_map().end())
      invalid("source net contains a missing pin");
    return existing->second;
  };
  for (const auto& edit : edits) {
    const auto& names = resolver.materialization(edit.creation_token);
    requireAbsentMaterialization(database, names);
    if (prepared.instances.contains(names.instance_name) || prepared.nets.contains(names.output_net_name)
        || prepared.pins.contains(names.input_pin_name) || prepared.pins.contains(names.output_pin_name))
      invalid("batch materialized name collision");
    if (names.input_pin_name != names.instance_name + ":" + edit.library_cell.input_port
        || names.output_pin_name != names.instance_name + ":" + edit.library_cell.output_port)
      invalid("materialized pin names do not match the selected library ports");

    const std::string source_name = resolver.resolveNet(edit.original_net);
    const auto added_net = prepared.nets.find(source_name);
    const auto original_net = database.get_net_map().find(source_name);
    if (added_net == prepared.nets.end() && original_net == database.get_net_map().end())
      invalid("source net does not exist: " + source_name);
    const auto* rc = std::get_if<ParasiticRcInputs>(&edit.rc_inputs);
    if (rc) {
      if (rc->parasitic_policy != ParasiticEditPolicy::kPreRouteTree
          || !std::holds_alternative<PlacedInputs>(edit.placement_inputs))
        invalid("raw RC edits require prepared pre-route trees and placed inputs");
      if (!database.get_parasitic_library().get_net_map().contains(source_name)
          || !prepared_rc.contains(source_name) || !prepared_rc.contains(names.output_net_name))
        invalid("pre-route edit is missing its old or replacement RC nets");
    } else if (database.get_parasitic_library().get_net_map().contains(source_name)
               || prepared_rc.contains(source_name) || prepared_rc.contains(names.output_net_name)) {
      invalid("NoParasitic input disagrees with the candidate parasitic library");
    }
    const Net& source_net = added_net != prepared.nets.end() ? added_net->second : original_net->second;
    const std::string driver = requireSingleDriver(source_net, pin_for);
    if (edit.moved_sinks.empty())
      invalid("buffer insertion must move at least one sink");

    const auto& cell = requireLegalNativeBufferCell(database, edit.library_cell);
    const auto* placement = placedInputs(edit.placement_inputs);
    Instance instance = makeInstance(edit.library_cell, cell, names, instance_id);
    Pin input = makePin(names.input_pin_name, edit.library_cell.input_port, names.instance_name,
                        requirePortDirection(cell.get_port_map().at(edit.library_cell.input_port), "input"), placement);
    Pin output = makePin(names.output_pin_name, edit.library_cell.output_port, names.instance_name,
                         requirePortDirection(cell.get_port_map().at(edit.library_cell.output_port), "output"), placement);
    input.set_net_name(source_name);
    output.set_net_name(names.output_net_name);

    auto& members = added_net != prepared.nets.end()
                        ? added_net->second.get_pin_name_list()
                        : prepared.memberships.try_emplace(source_name, source_net.get_pin_name_list()).first->second;
    std::vector<std::string> output_members{names.output_pin_name};
    std::set<std::string> unique;
    for (const auto& ref : edit.moved_sinks) {
      const std::string name = resolver.resolveNode(ref);
      const auto& pin = pin_for(name);
      const auto changed = prepared.connections.find(name);
      const auto& current_net = changed == prepared.connections.end() ? pin.get_net_name() : changed->second;
      if (!unique.insert(name).second || current_net != source_name || isDriver(pin) || name == driver)
        invalid("moved sink is not a unique load on the source net");
      if (prepared.pins.contains(name)) {
        if (pin.get_direction() != PinDirection::kInput)
          invalid("new moved buffer pin is not an input");
      } else {
        requireSupportedMovedSink(database, pin);
      }
      eraseExactlyOnce(members, name);
      output_members.push_back(name);
      if (auto added_pin = prepared.pins.find(name); added_pin != prepared.pins.end())
        added_pin->second.set_net_name(names.output_net_name);
      else
        prepared.connections[name] = names.output_net_name;
    }
    members.push_back(names.input_pin_name);
    Net net;
    net.set_net_name(names.output_net_name);
    net.get_pin_name_list() = std::move(output_members);
    prepared.instances.emplace(names.instance_name, std::move(instance));
    prepared.pins.emplace(names.input_pin_name, std::move(input));
    prepared.pins.emplace(names.output_pin_name, std::move(output));
    prepared.nets.emplace(names.output_net_name, std::move(net));
    prepared.instance_keys.push_back(names.instance_name);
    prepared.pin_keys.push_back(names.input_pin_name);
    prepared.pin_keys.push_back(names.output_pin_name);
    prepared.net_keys.push_back(names.output_net_name);
    if (instance_id == std::numeric_limits<std::uint64_t>::max() && &edit != &edits.back())
      invalid("native instance ID capacity is exhausted within the batch");
    ++instance_id;
  }
  prepared.prepareParasitics(std::move(prepared_rc));
  return prepared;
}

void PreparedNativeBufferInsertions::apply() noexcept
{
  for (auto& [name, members] : memberships)
    database->get_net_map().at(name).get_pin_name_list().swap(members);
  for (auto& [name, net] : connections)
    database->get_pin_map().at(name).get_net_name().swap(net);
  database->get_instance_map().merge(instances);
  database->get_pin_map().merge(pins);
  database->get_net_map().merge(nets);
  auto& rc_nets = database->get_parasitic_library().get_net_map();
  for (auto& [name, previous] : previous_parasitics) {
    previous = rc_nets.extract(name);
    rc_nets.insert(parasitics.extract(name));
  }
}

void PreparedNativeBufferInsertions::undo() noexcept
{
  auto& rc_nets = database->get_parasitic_library().get_net_map();
  for (auto& [name, previous] : previous_parasitics) {
    parasitics.insert(rc_nets.extract(name));
    if (!previous.empty())
      rc_nets.insert(std::move(previous));
  }
  for (auto& [name, members] : memberships)
    database->get_net_map().at(name).get_pin_name_list().swap(members);
  for (auto& [name, net] : connections)
    database->get_pin_map().at(name).get_net_name().swap(net);
  for (const auto& name : instance_keys)
    database->get_instance_map().erase(name);
  for (const auto& name : pin_keys)
    database->get_pin_map().erase(name);
  for (const auto& name : net_keys)
    database->get_net_map().erase(name);
}

void applyNativeBufferInsertion(Database& database, const BufferInsertionMutation& edit,
                                TimingEditResolverView resolver, std::uint64_t instance_id)
{
  auto prepared = prepareNativeBufferInsertions(database, {&edit, 1}, resolver, instance_id);
  prepared.apply();
}

void PreparedNetDriverLoad::swap() noexcept
{
  net->get_driver_pin().swap(driver);
  net->get_driver_pin_list().swap(drivers);
  net->get_load_pin_list().swap(loads);
}

void PreparedNativeGraphPatch::apply(Database& database) noexcept
{
  auto apply_rows = [](auto& current, auto& patch) {
    for (auto& [name, row] : patch)
      if (const auto found = current.find(name); found != current.end())
        found->second.swap(row);
    current.merge(patch);
  };
  apply_rows(database.get_outgoing_arc_list_map(), outgoing);
  apply_rows(database.get_incoming_arc_list_map(), incoming);
  for (auto& [slot, arc] : replacements)
    std::swap(database.get_arc_list()[slot], arc);
  // Capacity was reserved before the transaction took ownership.
  for (auto& arc : appends)
    database.get_arc_list().push_back(std::move(arc));
  database.get_timing_point_map().merge(points);
  for (auto& cache : net_caches)
    cache.swap();
}

void PreparedNativeGraphPatch::undo(Database& database) noexcept
{
  for (auto& cache : net_caches)
    cache.swap();
  for (const auto& name : new_points)
    database.get_timing_point_map().erase(name);
  database.get_arc_list().resize(arc_count_before);
  for (auto& [slot, arc] : replacements)
    std::swap(database.get_arc_list()[slot], arc);
  auto restore_rows = [](auto& current, auto& saved, const auto& appended) {
    for (auto& [name, row] : saved)
      current.at(name).swap(row);
    for (const auto& name : appended)
      current.erase(name);
  };
  restore_rows(database.get_outgoing_arc_list_map(), outgoing, new_outgoing);
  restore_rows(database.get_incoming_arc_list_map(), incoming, new_incoming);
}
}  // namespace ista::timing_edit::detail
