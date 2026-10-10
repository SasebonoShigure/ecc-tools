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
struct Member { const csr::NodeRecord* node; idb::IdbPin* pin; };
struct CriticalNet {
  idb::IdbNet* physical;
  Member driver;
  double slack;
  StableNetRef identity;
  Bytes key;
};
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
std::optional<double> driverResistance(const timing_dual_csr::DualCsrState& graph,
    csr::detail::NativeLibraryModels& models, const csr::NodeRecord& driver,
    std::span<const csr::BufferCandidateTiming> timing, double load)
{
  const auto& instances = graph.reference_payload.instances;
  const auto instance = std::ranges::find(instances, driver.key.instance_name, &csr::InstanceRecord::instance_name);
  if (instance == instances.end()) throw std::logic_error("greedy: driver instance is missing");
  const auto& cell = models.cell(driver.instance_cell);
  std::optional<double> result;
  for (const auto& arc : cell.cell_arcs) {
    if (arc.sink_port != driver.key.pin_name || arc.transfer.is_disabled || arc.transfer.is_clear_or_preset) continue;
    for (const auto id : instance->ordered_pins) {
      if (graph.nodes.at(id.value()).key.pin_name != arc.source_port) continue;
      const auto& point = timing[id.value()];
      const auto current_slew = arc.transfer.is_clock_arc ? point.clock_slew : point.data_slew;
      if (current_slew < 0.0) continue;
      if (const auto value = estimate(arc.transfer, current_slew, load))
        result = std::max(result.value_or(0.0), value->resistance);
    }
  }
  return result;
}
}  // namespace

PhysicalGreedySelection selectCsrPhysicalBuffers(const Database& library, csr::detail::NativeLibraryModels& models,
    idb::IdbDesign& design, const timing_dual_csr::DualCsrState& graph,
    const timing_dual_csr::DualCsrArcGroupIndex& index, const timing_edit::TimingEditResolverState& resolver,
    std::span<const csr::BufferCandidateTiming> timing, std::span<const std::string> buffer_cells,
    std::uint64_t top_k, std::uint64_t max_buffers, std::int32_t dbu_per_um)
{
  const auto generation = graph.generation.value();
  if (!top_k || !max_buffers || dbu_per_um <= 0 || generation == std::numeric_limits<std::uint64_t>::max()
      || timing.size() != graph.nodes.size())
    throw std::invalid_argument("greedy: invalid budget, generation, DBU or timing size");
  const auto& parasitics = graph.reference_payload.parasitics;
  if (parasitics.capacitance_unit != "PF" || parasitics.resistance_unit != "OHM")
    throw std::invalid_argument("greedy: expected canonical pre-route PF/OHM");
  const auto choices = buffers(library, models, buffer_cells);
  if (choices.empty()) throw std::invalid_argument("greedy: buffer allowlist is empty");
  // Borrow names for this protected query; no native graph or persistent duplicate mapping.
  std::unordered_map<std::string_view, const csr::NodeRecord*> by_name;
  by_name.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) by_name.emplace(node.key.full_name, &node);
  const auto members = [&](idb::IdbNet& net) {
    std::vector<Member> result;
    for (auto* pins : {net.get_io_pins(), net.get_instance_pin_list()})
      for (auto* pin : pins->get_pin_list()) {
        if (!pin) throw std::logic_error("greedy: null physical terminal");
        const auto* node = by_name.at(idbTimingPinName(*pin));
        if (node->net_name != net.get_net_name()) throw std::logic_error("greedy: physical/CSR net differs");
        result.push_back({node, pin});
      }
    // Match native refreshNetDriverLoad's existing load summation order.
    std::ranges::sort(result, [](const auto& a, const auto& b) { return a.node->key.full_name < b.node->key.full_name; });
    return result;
  };
  const auto is_driver = [](const csr::NodeRecord& node) {
    return node.direction == (node.is_port ? csr::PinDirection::kInput : csr::PinDirection::kOutput);
  };
  std::vector<CriticalNet> nets;
  for (const auto& [name, slot] : graph.parasitic_net_slots) {
    static_cast<void>(slot);
    auto* net = design.get_net_list()->find_net(name);
    if (!net) throw std::logic_error("greedy: CSR net is absent from physical design");
    const auto pins = members(*net);
    std::optional<Member> driver;
    std::optional<double> worst;
    bool clock = false, multiple_drivers = false;
    for (const auto& member : pins) {
      const auto& node = *member.node;
      const auto& point = timing[node.id.value()];
      clock |= point.clock_point || index.isClockSourceNode(node.id)
               || (!node.is_port && portModel(models, node).is_clock);
      if (is_driver(node)) {
        multiple_drivers |= driver.has_value();
        driver = member;
      } else if (dataSink(library, node) && std::isfinite(point.setup_slack)) {
        worst = std::min(worst.value_or(point.setup_slack), point.setup_slack);
      }
    }
    if (clock || multiple_drivers || !driver || driver->node->is_port || !worst) continue;
    auto identity = resolver.stableNetForNativeName(name);
    auto key = canonicalStableNetRefBytes(identity);
    nets.push_back({net, *driver, *worst, std::move(identity), std::move(key)});
  }
  std::sort(nets.begin(), nets.end(), [](const auto& left, const auto& right) {
    return left.slack != right.slack ? left.slack < right.slack : timing_edit::detail::canonicalBytesLess(left.key, right.key);
  });
  PhysicalGreedySelection result{generation, {}};
  std::set<Bytes, timing_edit::detail::CanonicalBytesLess> accepted_nodes;
  for (std::size_t n = 0; n < std::min<std::uint64_t>(top_k, nets.size()) && result.candidates.size() < max_buffers; ++n) {
    const auto& critical = nets[n];
    const auto& driver = *critical.driver.node;
    const auto& rc = parasitics.nets.at(graph.parasitic_net_slots.at(driver.net_name));
    const auto paths = rcPaths(rc, driver.key.full_name);
    const auto pins = members(*critical.physical);
    double total_load = rc.lumped_capacitance;
    for (const auto& member : pins) {
      const auto& node = *member.node;
      if (is_driver(node)) continue;
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
    const auto resistance = driverResistance(graph, models, driver, timing, total_load);
    const auto input_slew = timing[driver.id.value()].data_slew;
    if (!resistance || input_slew < 0.0) continue;
    const auto driver_key = canonicalStableNodeRefBytes(resolver.stableNodeForNativeName(driver.key.full_name));
    if (accepted_nodes.contains(driver_key)) continue;
    physical_greedy::Net candidate_net{critical.identity, position(critical.driver), input_slew, *resistance, {}};
    for (const auto& member : pins) {
      const auto& node = *member.node;
      const auto slack = timing[node.id.value()].setup_slack;
      if (is_driver(node) || !dataSink(library, node) || !std::isfinite(slack)) continue;
      auto stable_pin = resolver.stableNodeForNativeName(node.key.full_name);
      if (accepted_nodes.contains(canonicalStableNodeRefBytes(stable_pin))) continue;
      const auto path = paths.at(node.key.full_name);
      candidate_net.sinks.push_back({std::move(stable_pin), position(member), slack,
          pinCapacitance(portModel(models, node)) + path.capacitance, path.resistance});
    }
    const CreationToken token{.case_id = library.get_design_name(), .profile_id = "physical-greedy",
        .batch_ordinal = generation + 1, .accepted_ordinal = result.candidates.size()};
    auto best = physical_greedy::score(candidate_net, choices, token, dbu_per_um);
    if (best) {
      const auto names = defaultTokenMaterialization(best->edit.creation_token, best->edit.library_cell);
      if (by_name.contains(names.input_pin_name) || by_name.contains(names.output_pin_name)
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
