// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PreparedPhysicalBatch.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace izh {
namespace {
void require(bool value, const char* message)
{
  if (!value) throw std::invalid_argument(message);
}
void addReference(idb::IdbNet& net, idb::IdbPin* pin)
{
  if (pin->is_io_pin()) net.add_io_pin_unique(pin);
  else {
    net.add_instance_pin_unique(pin);
    require(pin->get_instance() && net.get_instance_list()->add_instance_ref(pin->get_instance()),
            "physical RC pin has no unique instance owner");
  }
}
bool isSink(idb::IdbPin& pin)
{
  return pin.get_term() && pin.get_term()->get_type() != idb::IdbConnectType::kClock
      && pin.get_term()->get_direction() == (pin.is_io_pin() ? idb::IdbConnectDirection::kOutput : idb::IdbConnectDirection::kInput);
}
}
PreparedPhysicalBatch::PreparedPhysicalBatch(idb::IdbDesign& design, std::span<const PhysicalBufferInsertion> edits)
    : _design(&design)
{
  require(!edits.empty() && design.get_layout() && design.get_instance_list() && design.get_net_list(),
          "physical batch requires a loaded design and edits");
  auto& instances = *design.get_instance_list();
  auto& nets = *design.get_net_list();
  _instance_count = instances._instance_list.size(); _net_count = nets._net_list.size();
  _instance_id = instances._mutex_index; _net_id = nets._mutex_index;
  require(edits.size() <= std::numeric_limits<std::uint64_t>::max() - std::max(_instance_id, _net_id),
          "physical object ID capacity exhausted");
  std::set<idb::IdbNet*> touched;
  _instances.reserve(edits.size()); _nets.reserve(edits.size()); _references.reserve(edits.size());
  for (const auto& edit : edits) {
    require(edit.source_net && touched.insert(edit.source_net).second && edit.source_net->is_signal()
        && edit.source_net->get_wire_list()->get_wire_list().empty(), "physical edit requires a distinct unrouted data net");
    const auto old_name = edit.source_net->get_net_name();
    const auto owned = nets._net_map.find(old_name);
    require(owned != nets._net_map.end() && owned->second == edit.source_net, "physical source net owner mismatch");
    require(edit.master && design.get_layout()->get_cell_master_list()->find_cell_master(edit.master->get_name()) == edit.master,
            "physical cell master owner mismatch");
    require(!edit.instance_name.empty() && !edit.net_name.empty() && !edit.sinks.empty()
        && !instances._instance_map.contains(edit.instance_name) && !_instance_entries.contains(edit.instance_name)
        && !nets._net_map.contains(edit.net_name) && !_net_entries.contains(edit.net_name),
        "physical insertion name conflict or empty sinks");
    require(edit.orientation == idb::IdbOrient::kN_R0 || edit.orientation == idb::IdbOrient::kFS_MX
        || edit.orientation == idb::IdbOrient::kFN_MY || edit.orientation == idb::IdbOrient::kS_R180,
        "physical buffer requires a standard-cell row orientation");
    require(edit.master->get_width() > 0 && edit.master->get_height() > 0
        && edit.x >= 0 && edit.y >= 0
        && std::int64_t{edit.x} + edit.master->get_width() <= std::numeric_limits<std::int32_t>::max()
        && std::int64_t{edit.y} + edit.master->get_height() <= std::numeric_limits<std::int32_t>::max(),
        "physical buffer geometry exceeds DBU capacity");
    std::set<idb::IdbPin*> sinks;
    for (auto* pin : edit.sinks)
      require(pin && sinks.insert(pin).second && pin->get_net() == edit.source_net && !pin->get_special_net() && isSink(*pin)
          && (pin->is_io_pin() ? edit.source_net->has_io_pin(pin) : edit.source_net->has_instance_pin(pin)),
          "physical moved sink is repeated, stale, clock-related or not an input");

    auto instance = std::make_unique<idb::IdbInstance>();
    instance->set_name(edit.instance_name); instance->set_id(_instance_id + _instances.size());
    instance->set_cell_master(edit.master); instance->set_type(idb::IdbInstanceType::kTiming);
    instance->set_orient(edit.orientation, false); instance->set_coodinate(edit.x, edit.y, false);
    instance->set_status(idb::IdbPlacementStatus::kPlaced);
    require(instance->set_bounding_box(), "physical buffer bounding box is invalid");
    instance->set_pin_list_coodinate(); instance->set_obs_box_list();
    auto* input = instance->get_pin(edit.input_pin); auto* output = instance->get_pin(edit.output_pin);
    require(input && output && input != output && isSink(*input)
        && output->get_term()->get_direction() == idb::IdbConnectDirection::kOutput,
        "physical buffer input/output is invalid");
    auto net = std::make_unique<idb::IdbNet>();
    net->set_net_name(edit.net_name); net->set_connect_type(idb::IdbConnectType::kSignal);
    net->set_id(_net_id + _nets.size());
    auto replacement = std::make_unique<idb::IdbNet>();
    replacement->set_net_name(old_name);
    for (auto* list : {edit.source_net->get_io_pins(), edit.source_net->get_instance_pin_list()})
      for (auto* pin : list->get_pin_list())
        if (!sinks.contains(pin)) addReference(*replacement, pin);
    addReference(*replacement, input); addReference(*net, output);
    _pins.push_back({input, nullptr, edit.source_net, old_name});
    _pins.push_back({output, nullptr, net.get(), edit.net_name});
    for (auto* pin : edit.sinks) {
      addReference(*net, pin);
      _pins.push_back({pin, edit.source_net, net.get(), edit.net_name});
    }
    _references.emplace_back(edit.source_net, std::move(replacement));
    _instance_entries.emplace(edit.instance_name, instance.get()); _net_entries.emplace(edit.net_name, net.get());
    _instances.push_back(std::move(instance)); _nets.push_back(std::move(net));
  }
  // Only capacity changes touch canonical containers during preparation.
  instances._instance_list.reserve(_instance_count + edits.size());
  instances._instance_map.reserve(instances._instance_map.size() + edits.size());
  nets._net_list.reserve(_net_count + edits.size());
  nets._net_map.reserve(nets._net_map.size() + edits.size());
}
PreparedPhysicalBatch::PreparedPhysicalBatch(PreparedPhysicalBatch&& other) noexcept
    : _design(std::exchange(other._design, nullptr)), _applied(std::exchange(other._applied, false)), _finished(other._finished),
      _instance_count(other._instance_count), _net_count(other._net_count), _instance_id(other._instance_id), _net_id(other._net_id),
      _instances(std::move(other._instances)), _nets(std::move(other._nets)), _references(std::move(other._references)),
      _pins(std::move(other._pins)), _instance_entries(std::move(other._instance_entries)), _net_entries(std::move(other._net_entries))
{
}
PreparedPhysicalBatch::~PreparedPhysicalBatch() noexcept { rollback(); }
void PreparedPhysicalBatch::swapReferences(idb::IdbNet& left, idb::IdbNet& right) noexcept
{
  const auto pins = [](idb::IdbPins& a, idb::IdbPins& b) {
    a._pin_list.swap(b._pin_list); a._pin_ref_index.swap(b._pin_ref_index);
  };
  pins(*left._io_pin_list, *right._io_pin_list); pins(*left._instance_pin_list, *right._instance_pin_list);
  left._instance_list->_instance_list.swap(right._instance_list->_instance_list);
  left._instance_list->_instance_map.swap(right._instance_list->_instance_map);
  std::swap(left._average_coordinate, right._average_coordinate);
}
std::map<std::string, ista::ParasiticNet> PreparedPhysicalBatch::prepareRc(const PreRouteRcTechnology& technology) const
{
  require(_design && !_applied && !_finished, "physical RC preparation requires an unapplied batch");
  std::map<std::string, ista::ParasiticNet> result;
  for (const auto& [original, replacement] : _references) {
    (void) original;
    result.emplace(replacement->get_net_name(), buildPreRouteRcNet(*replacement, technology));
  }
  for (const auto& net : _nets)
    result.emplace(net->get_net_name(), buildPreRouteRcNet(*net, technology));
  return result;
}
bool PreparedPhysicalBatch::apply() noexcept
{
  if (!_design || _applied || _finished) return false;
  auto& instances = *_design->get_instance_list(); auto& nets = *_design->get_net_list();
  if (instances._instance_list.size() != _instance_count || nets._net_list.size() != _net_count
      || instances._mutex_index != _instance_id || nets._mutex_index != _net_id) return false;
  for (const auto& pin : _pins) if (pin.pin->_net != pin.before) return false;
  for (const auto& [name, pointer] : _instance_entries) { (void) pointer; if (instances._instance_map.contains(name)) return false; }
  for (const auto& [name, pointer] : _net_entries) { (void) pointer; if (nets._net_map.contains(name)) return false; }
  // Hash nodes, buckets, vectors, pin names and reference indexes are prepared.
  instances._instance_map.merge(_instance_entries); nets._net_map.merge(_net_entries);
  for (const auto& instance : _instances) instances._instance_list.push_back(instance.get());
  for (const auto& net : _nets) nets._net_list.push_back(net.get());
  instances._mutex_index += _instances.size(); nets._mutex_index += _nets.size();
  for (auto& [net, replacement] : _references) swapReferences(*net, *replacement);
  for (auto& pin : _pins) { pin.pin->_net_name.swap(pin.name); pin.pin->_net = pin.after; }
  _applied = true;
  return true;
}
void PreparedPhysicalBatch::rollback() noexcept
{
  if (!_applied) return;
  for (auto& pin : _pins) { pin.pin->_net_name.swap(pin.name); pin.pin->_net = pin.before; }
  for (auto& [net, replacement] : _references) swapReferences(*net, *replacement);
  auto& instances = *_design->get_instance_list(); auto& nets = *_design->get_net_list();
  for (const auto& instance : _instances) instances._instance_map.erase(instance->get_name());
  for (const auto& net : _nets) nets._net_map.erase(net->_net_name);
  instances._instance_list.resize(_instance_count); nets._net_list.resize(_net_count);
  instances._mutex_index = _instance_id; nets._mutex_index = _net_id;
  _applied = false; _finished = true;
}
void PreparedPhysicalBatch::commit() noexcept
{
  if (!_applied) return;
  for (auto& instance : _instances) (void) instance.release();
  for (auto& net : _nets) (void) net.release();
  _applied = false; _finished = true;
  _pins.clear(); _references.clear();
}
}  // namespace izh
