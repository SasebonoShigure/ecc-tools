// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalGreedyScoring.hpp"
#include "CsrBufferCandidateTiming.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "Database.hpp"
#include "DualCsrArcGroupIndex.hpp"
#include "IdbDesign.h"
#include "IdbTimingPinName.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditValidation.hpp"
#include "TimingEditByteOrder.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace ista {
namespace {
using namespace timing_edit;
using namespace physical_greedy;
using Member = PreparedCsrCandidates::Member;
struct CriticalNet {
  const PreparedCsrCandidates::Net* prepared;
  double slack;
  Bytes key;
};
bool isDriver(const csr::NodeRecord& node)
{
  return node.direction == (node.is_port ? csr::PinDirection::kInput : csr::PinDirection::kOutput);
}
csr::NodeKey nodeKey(idb::IdbPin& pin)
{
  const auto name = idbTimingPinName(pin);
  if (pin.is_io_pin()) return {csr::NodeKind::kPort, name, {}, {}};
  return {csr::NodeKind::kInstancePin, name, pin.get_instance()->get_name(), pin.get_pin_name()};
}
const csr::LibraryPortModel& portModel(csr::detail::NativeLibraryModels& models, const csr::NodeRecord& node)
{
  const auto& ports = models.cell(node.instance_cell).ports;
  const auto found = std::ranges::find(ports, node.key.pin_name, &csr::LibraryPortModel::name);
  if (found == ports.end()) throw std::logic_error("greedy: CSR library port is missing");
  return *found;
}
bool dataSink(const Database& library, const csr::NodeRecord& node)
{
  if (node.is_port) return false;
  try {
    timing_edit::detail::requireSupportedMovedSink(node, library.get_timing_library().get_cell_map().at(node.instance_cell));
    return true;
  } catch (const std::invalid_argument&) { return false; }
}
std::array<double, 2> position(const Member& member)
{
  auto* xy = member.pin->get_average_coordinate();
  if (!xy) throw std::logic_error("greedy: physical pin has no coordinate");
  return {static_cast<double>(xy->get_x()), static_cast<double>(xy->get_y())};
}
struct RcPath { double resistance, capacitance; };
std::map<std::string, RcPath> rcPaths(const csr::ParasiticNetModel& rc, const std::string& driver)
{
  std::unordered_map<std::string_view, double> caps;
  for (const auto& node : rc.nodes) caps.emplace(node.map_key, node.capacitance);
  std::map<std::string, std::vector<std::pair<std::string, double>>> adjacent;
  for (const auto& edge : rc.resistors) {
    adjacent[edge.source].emplace_back(edge.sink, edge.resistance * 1E-3);
    adjacent[edge.sink].emplace_back(edge.source, edge.resistance * 1E-3);
  }
  std::map<std::string, RcPath> result{{driver, {0.0, 0.0}}};
  std::vector<std::string> queue{driver};
  for (std::size_t index = 0; index < queue.size(); ++index) {
    const auto name = queue[index];
    for (const auto& [next, resistance] : adjacent[name]) {
      if (result.contains(next)) continue;
      const auto path = result.at(name);
      const RcPath next_path{path.resistance + resistance, path.capacitance + caps.at(next)};
      if (!std::isfinite(next_path.resistance) || !std::isfinite(next_path.capacitance))
        throw std::runtime_error("greedy: RC path overflow");
      result.emplace(next, next_path);
      queue.push_back(next);
    }
  }
  return result;
}
std::optional<double> driverResistance(const PreparedCsrCandidates::Net& net,
    std::span<const csr::BufferCandidateTiming> timing, double load)
{
  std::optional<double> result;
  for (const auto& input : net.inputs) {
    const auto& point = timing[input.timing_slot];
    const auto slew = input.transfer->is_clock_arc ? point.clock_slew : point.data_slew;
    if (slew < 0.0) continue;
    if (const auto value = estimate(*input.transfer, slew, load))
      result = std::max(result.value_or(0.0), value->resistance);
  }
  return result;
}
}  // namespace

PreparedCsrCandidates prepareCsrPhysicalBuffers(const Database& library, csr::detail::NativeLibraryModels& models,
    idb::IdbDesign& design, const timing_dual_csr::DualCsrState& graph,
    const timing_dual_csr::DualCsrArcGroupIndex& index, const TimingEditResolverState& resolver)
{
  const auto& parasitics = graph.reference_payload.parasitics;
  if (parasitics.capacitance_unit != "PF" || parasitics.resistance_unit != "OHM")
    throw std::invalid_argument("greedy: expected canonical pre-route PF/OHM");
  const TimingEditResolverView bindings{resolver};
  PreparedCsrCandidates result;
  std::unordered_map<std::uint64_t, std::size_t> slots;
  const auto slot = [&](csr::NodeId id) {
    const auto [entry, added] = slots.emplace(id.value(), result.node_ids.size());
    if (added) result.node_ids.push_back(id);
    return entry->second;
  };
  for (const auto& [name, rc_slot] : graph.parasitic_net_slots) {
    static_cast<void>(rc_slot);
    auto* physical = design.get_net_list()->find_net(name);
    if (!physical) throw std::logic_error("greedy: CSR net is absent from physical design");
    PreparedCsrCandidates::Net net{};
    for (auto* pins : {physical->get_io_pins(), physical->get_instance_pin_list()})
      for (auto* pin : pins->get_pin_list()) {
        if (!pin) throw std::logic_error("greedy: null physical terminal");
        const auto key = nodeKey(*pin);
        const auto* entry = bindings.findNodeEntryForKey(key);
        if (!entry || entry->second.tombstone) throw std::logic_error("greedy: physical terminal has no CSR binding");
        const auto& node = graph.nodes.at(entry->second.slot.value());
        if (node.key != key || node.net_name != name) throw std::logic_error("greedy: physical/CSR net differs");
        net.members.push_back({&node, pin, &entry->first});
      }
    // Preserve native load summation order and the existing candidate tie-break.
    std::ranges::sort(net.members, [](const auto& a, const auto& b) { return a.node->key.full_name < b.node->key.full_name; });
    bool clock = false, has_sink = false;
    std::size_t drivers = 0;
    for (std::size_t i = 0; i < net.members.size(); ++i) {
      auto& member = net.members[i];
      const auto& node = *member.node;
      clock |= index.isClockSourceNode(node.id) || (!node.is_port && portModel(models, node).is_clock);
      if (isDriver(node)) { ++drivers; net.driver = i; }
      else { member.data_sink = dataSink(library, node); has_sink |= member.data_sink; }
    }
    if (clock || drivers != 1 || net.members[net.driver].node->is_port || !has_sink) continue;
    for (auto& member : net.members) member.timing_slot = slot(member.node->id);
    const auto& driver = net.members[net.driver];
    auto* instance = driver.pin->get_instance();
    if (!instance) throw std::logic_error("greedy: driver instance is missing");
    for (const auto& arc : models.cell(driver.node->instance_cell).cell_arcs) {
      if (arc.sink_port != driver.node->key.pin_name || arc.transfer.is_disabled || arc.transfer.is_clear_or_preset) continue;
      auto* pin = instance->get_pin_list()->find_pin_by_term(arc.source_port);
      if (!pin) continue;
      const auto* entry = bindings.findNodeEntryForKey(nodeKey(*pin));
      if (!entry || entry->second.tombstone) continue;
      const auto& node = graph.nodes.at(entry->second.slot.value());
      net.inputs.push_back({&arc.transfer, slot(node.id)});
    }
    const auto* identity = bindings.findNetEntryForName(name);
    if (!identity || identity->second.tombstone) throw std::logic_error("greedy: physical net has no CSR binding");
    net.identity = &identity->first;
    result.nets.push_back(std::move(net));
  }
  return result;
}

PhysicalGreedySelection selectCsrPhysicalBuffers(const Database& library, csr::detail::NativeLibraryModels& models,
    idb::IdbDesign& design, const timing_dual_csr::DualCsrState& graph, const PreparedCsrCandidates& prepared,
    std::span<const csr::BufferCandidateTiming> timing, std::span<const std::string> buffer_cells,
    std::uint64_t top_k, std::uint64_t max_buffers, std::int32_t dbu_per_um)
{
  const auto generation = graph.generation.value();
  if (!top_k || !max_buffers || dbu_per_um <= 0 || generation == std::numeric_limits<std::uint64_t>::max()
      || timing.size() != prepared.node_ids.size())
    throw std::invalid_argument("greedy: invalid budget, generation, DBU or timing size");
  const auto choices = buffers(library, models, buffer_cells);
  if (choices.empty()) throw std::invalid_argument("greedy: buffer allowlist is empty");
  std::vector<CriticalNet> nets;
  for (const auto& net : prepared.nets) {
    std::optional<double> worst;
    bool clock = false;
    for (const auto& member : net.members) {
      const auto& point = timing[member.timing_slot];
      clock |= point.clock_point;
      if (member.data_sink && std::isfinite(point.setup_slack))
        worst = std::min(worst.value_or(point.setup_slack), point.setup_slack);
    }
    if (!clock && worst) nets.push_back({&net, *worst, canonicalStableNetRefBytes(*net.identity)});
  }
  std::sort(nets.begin(), nets.end(), [](const auto& left, const auto& right) {
    return left.slack != right.slack ? left.slack < right.slack : timing_edit::detail::canonicalBytesLess(left.key, right.key);
  });
  PhysicalGreedySelection result{generation, {}};
  std::set<Bytes, timing_edit::detail::CanonicalBytesLess> accepted_nodes;
  for (std::size_t n = 0; n < std::min<std::uint64_t>(top_k, nets.size()) && result.candidates.size() < max_buffers; ++n) {
    const auto& net = *nets[n].prepared;
    const auto& driver = net.members[net.driver];
    const auto& rc = graph.reference_payload.parasitics.nets.at(graph.parasitic_net_slots.at(driver.node->net_name));
    const auto paths = rcPaths(rc, driver.node->key.full_name);
    double total_load = rc.lumped_capacitance;
    for (const auto& member : net.members) {
      const auto& node = *member.node;
      if (isDriver(node)) continue;
      if (!node.is_port) total_load += pinCapacitance(portModel(models, node));
      else {
        const auto& constraints = graph.reference_payload.port_constraints;
        const auto constraint = std::ranges::find(constraints, node.key.full_name, &csr::PortConstraintModel::map_key);
        if (constraint != constraints.end() && constraint->output_load) {
          const auto load = *constraint->output_load;
          if (!std::isfinite(load) || load < 0.0) throw std::runtime_error("greedy: invalid port load");
          total_load += load;
        }
      }
    }
    const auto resistance = driverResistance(net, timing, total_load);
    const auto input_slew = timing[driver.timing_slot].data_slew;
    if (!resistance || input_slew < 0.0) continue;
    const auto driver_key = canonicalStableNodeRefBytes(*driver.identity);
    if (accepted_nodes.contains(driver_key)) continue;
    physical_greedy::Net candidate_net{*net.identity, position(driver), input_slew, *resistance, {}};
    for (const auto& member : net.members) {
      const auto& node = *member.node;
      const auto slack = timing[member.timing_slot].setup_slack;
      if (!member.data_sink || !std::isfinite(slack)) continue;
      if (accepted_nodes.contains(canonicalStableNodeRefBytes(*member.identity))) continue;
      const auto path = paths.at(node.key.full_name);
      candidate_net.sinks.push_back({*member.identity, position(member), slack,
          pinCapacitance(portModel(models, node)) + path.capacitance, path.resistance});
    }
    const CreationToken token{.case_id = library.get_design_name(), .profile_id = "physical-greedy",
        .batch_ordinal = generation + 1, .accepted_ordinal = result.candidates.size()};
    auto best = physical_greedy::score(candidate_net, choices, token, dbu_per_um);
    if (best) {
      const auto names = defaultTokenMaterialization(best->edit.creation_token, best->edit.library_cell);
      if (design.get_io_pin_list()->find_pin(names.input_pin_name) || design.get_io_pin_list()->find_pin(names.output_pin_name)
          || design.get_instance_list()->find_instance(names.instance_name)
          || design.get_net_list()->find_net(names.output_net_name))
        throw std::runtime_error("greedy: materialized name collision");
      accepted_nodes.insert(driver_key);
      accepted_nodes.insert(canonicalStableNodeRefBytes(best->edit.moved_sinks.front()));
      result.candidates.push_back(std::move(*best));
    }
  }
  return result;
}
}  // namespace ista
