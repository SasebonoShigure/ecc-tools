// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalGreedySelection.hpp"
#include "BufferInsertionGain.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrTableLookup.hpp"
#include "Database.hpp"
#include "NativeBufferCellValidation.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditValidation.hpp"
#include "TimingEditByteOrder.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace ista {
namespace {
using namespace timing_edit;
using Bytes = std::vector<std::uint8_t>;
struct Buffer {
  LibraryCellRef reference;
  const csr::CellTransferModel* model;
  double capacitance;
  Bytes key;
};
double pinCapacitance(const csr::LibraryPortModel& pin)
{
  double value = 0.0;
  for (const auto& transition : pin.transition_capacitance[0])
    value = std::max(value, transition.value_or(pin.default_capacitance));
  if (!std::isfinite(value) || value < 0.0) throw std::runtime_error("greedy: invalid pin capacitance");
  return value;
}
const csr::LibraryPortModel& pinModel(const Database& database, csr::detail::NativeLibraryModels& models, const Pin& pin)
{
  const auto& cell = models.cell(database.get_instance_map().at(pin.get_instance_name()).get_cell_name());
  const auto port = std::ranges::find(cell.ports, pin.get_pin_name(), &csr::LibraryPortModel::name);
  if (port == cell.ports.end()) throw std::runtime_error("greedy: missing library pin");
  return *port;
}
std::vector<Buffer> buffers(const Database& database, csr::detail::NativeLibraryModels& models,
                            std::span<const std::string> names)
{
  std::vector<Buffer> result;
  std::set<std::string> seen;
  for (const auto& name : names) {
    if (!seen.insert(name).second) throw std::invalid_argument("greedy: duplicate buffer cell");
    const auto& cell = models.cell(name);
    if (!cell.present || cell.ports.size() != 2) throw std::invalid_argument("greedy: invalid buffer cell");
    const auto input = std::ranges::find(cell.ports, csr::PinDirection::kInput, &csr::LibraryPortModel::direction);
    const auto output = std::ranges::find(cell.ports, csr::PinDirection::kOutput, &csr::LibraryPortModel::direction);
    if (input == cell.ports.end() || output == cell.ports.end())
      throw std::invalid_argument("greedy: buffer ports are not input/output");
    const csr::CellTransferModel* model = nullptr;
    for (const auto& arc : cell.cell_arcs)
      if (!arc.transfer.is_disabled && !arc.transfer.is_clock_arc && !arc.transfer.is_clear_or_preset) {
        if (model || arc.source_port != input->name || arc.sink_port != output->name)
          throw std::invalid_argument("greedy: buffer has ambiguous arcs");
        model = &arc.transfer;
      }
    if (!model) throw std::invalid_argument("greedy: buffer arc is missing");
    LibraryCellRef reference{cell.library_name, name, input->name, output->name, {}};
    for (const auto& child : model->children()) reference.ordered_arc_signatures.push_back(child.signature);
    (void) requireLegalNativeBufferCell(database, reference);
    auto key = canonicalLibraryCellRefBytes(reference);
    result.push_back({std::move(reference), model, pinCapacitance(*input), std::move(key)});
  }
  return result;
}
std::optional<double> slew(const TimingPoint& point, bool clock)
{
  const auto& lanes = clock ? point.get_physical_clock_slew_map() : point.get_data_slew_map();
  const auto maximum = lanes.find(AnalysisType::kMax);
  if (maximum == lanes.end()) return std::nullopt;
  std::optional<double> result;
  for (const auto& [transition, value] : maximum->second) {
    (void) transition;
    if (std::isfinite(value) && value >= 0.0) result = std::max(result.value_or(0.0), value);
  }
  return result;
}
std::optional<double> setupSlack(const TimingPoint& point)
{
  if (point.get_is_clock_point() || !std::isfinite(point.get_slack())
      || !std::isfinite(point.get_required()) || !std::isfinite(point.get_arrival())) return std::nullopt;
  const auto maximum = point.get_path_state_map().find(AnalysisType::kMax);
  if (maximum == point.get_path_state_map().end()) return std::nullopt;
  for (const auto& [source, transitions] : maximum->second) {
    (void) source;
    for (const auto& [transition, clocks] : transitions) {
      (void) transition;
      for (const auto& [clock, path] : clocks) {
        (void) clock;
        if (std::isfinite(path.get_arrival())) return point.get_slack();
      }
    }
  }
  return std::nullopt;
}
struct DriverEstimate { double delay, resistance; };
std::optional<DriverEstimate> estimate(const csr::CellTransferModel& model, double input_slew, double load)
{
  std::optional<DriverEstimate> result;
  for (const auto& child : model.children()) {
    for (std::size_t transition = 0; transition < csr::kTransitionCount; ++transition) {
      const auto lookup = [&](double cap) {
        return csr::lookupCsrTimingTableRawLoad(child.delay_tables[transition], input_slew, cap,
            child.time_unit_scale, child.capacitance_unit_scale);
      };
      const auto delay = lookup(load), next = lookup(load + 1E-3);
      if (!delay || !next) continue;
      double threshold = child.output_threshold_pct[transition];
      if (threshold > 1.0) threshold *= 0.01;
      // Same Liberty slope convention as native DMP; these are gain estimates,
      // never inputs to the selected STA backend or acceptance timing.
      const double resistance = -std::log(threshold) * std::abs(*next - *delay) / ((load + 1E-3) - load);
      if (!std::isfinite(*delay) || *delay < 0.0 || !std::isfinite(resistance) || resistance < 0.0
          || threshold <= 0.0 || threshold >= 1.0)
        throw std::runtime_error("greedy: invalid Liberty driver estimate");
      if (!result) result = DriverEstimate{*delay, resistance};
      else { result->delay = std::max(result->delay, *delay); result->resistance = std::max(result->resistance, resistance); }
    }
  }
  return result;
}
std::optional<double> driverResistance(const Database& database, csr::detail::NativeLibraryModels& models,
                                       const Pin& driver, double load)
{
  // A port with an external driver model needs a separate candidate model; it
  // remains editable through fixed batches. Do not invent a zero-R driver.
  if (driver.get_is_port()) return std::nullopt;
  const auto& instance = database.get_instance_map().at(driver.get_instance_name());
  const auto& cell = models.cell(instance.get_cell_name());
  std::optional<double> result;
  for (const auto& arc : cell.cell_arcs) {
    if (arc.sink_port != driver.get_pin_name() || arc.transfer.is_disabled || arc.transfer.is_clear_or_preset) continue;
    for (const auto& name : instance.get_pin_name_list()) {
      const auto& source = database.get_pin_map().at(name);
      if (source.get_pin_name() != arc.source_port) continue;
      const auto& point = database.get_timing_point_map().at(name);
      const auto current_slew = slew(point, arc.transfer.is_clock_arc);
      if (!current_slew) continue;
      if (const auto value = estimate(arc.transfer, *current_slew, load))
        result = std::max(result.value_or(0.0), value->resistance);
    }
  }
  return result;
}
struct RcPath { double resistance_kohm, capacitance_pf; };
std::map<std::string, RcPath> rcPaths(const ParasiticNet& net, const std::string& driver)
{
  std::map<std::string, std::vector<std::pair<std::string, double>>> adjacent;
  for (const auto& edge : net.get_resistor_list()) {
    adjacent[edge.get_source_node()].emplace_back(edge.get_sink_node(), edge.get_resistance() * 1E-3);
    adjacent[edge.get_sink_node()].emplace_back(edge.get_source_node(), edge.get_resistance() * 1E-3);
  }
  std::map<std::string, RcPath> result{{driver, {0.0, 0.0}}};
  std::vector<std::string> queue{driver};
  for (std::size_t index = 0; index < queue.size(); ++index) {
    const auto name = queue[index];
    for (const auto& [next, resistance] : adjacent[name]) {
      if (result.contains(next)) continue;
      const auto path = result.at(name);
      const RcPath next_path{path.resistance_kohm + resistance, path.capacitance_pf + net.get_node_map().at(next).get_capacitance()};
      if (!std::isfinite(next_path.resistance_kohm) || !std::isfinite(next_path.capacitance_pf))
        throw std::runtime_error("greedy: RC path overflow");
      result.emplace(next, next_path);
      queue.push_back(next);
    }
  }
  return result;
}
struct CriticalNet {
  const Net* net;
  const Pin* driver;
  double slack;
  StableNetRef identity;
  Bytes key;
};
bool dataSink(const Database& database, const Pin& pin)
{
  try {
    timing_edit::detail::requireSupportedMovedSink(database, pin);
    return !pin.get_is_port();  // Instance candidates; port edits remain explicit batches.
  } catch (const std::invalid_argument&) {
    return false;
  }
}
}
PhysicalGreedySelection selectPhysicalBuffers(const Database& database, csr::detail::NativeLibraryModels& models,
    const timing_edit::TimingEditResolverState& resolver, std::uint64_t generation,
    std::span<const std::string> buffer_cells, std::uint64_t top_k, std::uint64_t max_buffers, std::int32_t dbu_per_um)
{
  if (!top_k || !max_buffers || dbu_per_um <= 0 || generation == std::numeric_limits<std::uint64_t>::max())
    throw std::invalid_argument("greedy: invalid budget, generation or DBU");
  const auto& parasitics = database.get_parasitic_library();
  if (parasitics.get_capacitive_unit() != "PF" || parasitics.get_resistance_unit() != "OHM")
    throw std::invalid_argument("greedy: expected canonical pre-route PF/OHM");
  const auto choices = buffers(database, models, buffer_cells);
  if (choices.empty()) throw std::invalid_argument("greedy: buffer allowlist is empty");
  std::vector<CriticalNet> nets;
  for (const auto& [name, net] : database.get_net_map()) {
    if (net.get_driver_pin_list().size() != 1 || net.get_load_pin_list().empty()) continue;
    try { timing_edit::detail::requireDataOnlyNet(database, name); }
    catch (const std::invalid_argument&) { continue; }
    const auto& driver = database.get_pin_map().at(net.get_driver_pin_list().front());
    if (driver.get_is_port() || database.get_timing_point_map().at(driver.get_full_name()).get_is_clock_point()) continue;
    std::optional<double> worst;
    bool clock_net = false;
    for (const auto& sink : net.get_load_pin_list()) {
      const auto& point = database.get_timing_point_map().at(sink);
      clock_net |= point.get_is_clock_point();
      if (!dataSink(database, database.get_pin_map().at(sink))) continue;
      if (const auto slack = setupSlack(point)) worst = std::min(worst.value_or(*slack), *slack);
    }
    if (clock_net || !worst) continue;
    auto identity = resolver.stableNetForNativeName(name);
    auto key = canonicalStableNetRefBytes(identity);
    nets.push_back({&net, &driver, *worst, std::move(identity), std::move(key)});
  }
  std::sort(nets.begin(), nets.end(), [](const auto& left, const auto& right) {
    return left.slack != right.slack ? left.slack < right.slack : timing_edit::detail::canonicalBytesLess(left.key, right.key);
  });
  PhysicalGreedySelection result{generation, {}};
  std::set<Bytes, timing_edit::detail::CanonicalBytesLess> accepted_nodes;
  for (std::size_t index = 0; index < std::min<std::uint64_t>(top_k, nets.size()) && result.candidates.size() < max_buffers; ++index) {
    const auto& critical = nets[index];
    const auto& driver = *critical.driver;
    const auto& net = *critical.net;
    const auto& rc = parasitics.get_net_map().at(driver.get_net_name());
    const auto paths = rcPaths(rc, driver.get_full_name());
    double total_load = rc.get_lumped_capacitance();
    for (const auto& name : net.get_load_pin_list()) {
      const auto& pin = database.get_pin_map().at(name);
      if (!pin.get_is_port()) {
        total_load += pinCapacitance(pinModel(database, models, pin));
      } else {
        const auto& constraints = database.get_timing_constraint().get_port_constraint_map();
        const auto constraint = constraints.find(name);
        if (constraint != constraints.end() && constraint->second.get_has_load()) {
          const double load = constraint->second.get_load();
          if (!std::isfinite(load) || load < 0.0) throw std::runtime_error("greedy: invalid port load");
          total_load += load;
        }
      }
    }
    const auto resistance = driverResistance(database, models, driver, total_load);
    const auto input_slew = slew(database.get_timing_point_map().at(driver.get_full_name()), false);
    if (!resistance || !input_slew) continue;
    const auto driver_key = canonicalStableNodeRefBytes(resolver.stableNodeForNativeName(driver.get_full_name()));
    if (accepted_nodes.contains(driver_key)) continue;
    std::optional<PhysicalGreedyCandidate> best;
    Bytes best_pin, best_buffer;
    for (const auto& name : net.get_load_pin_list()) {
      const auto& pin = database.get_pin_map().at(name);
      const auto slack = setupSlack(database.get_timing_point_map().at(name));
      if (!dataSink(database, pin) || !slack) continue;
      auto stable_pin = resolver.stableNodeForNativeName(name);
      const auto pin_key = canonicalStableNodeRefBytes(stable_pin);
      if (accepted_nodes.contains(pin_key)) continue;
      const auto path = paths.at(name);
      const double load = pinCapacitance(pinModel(database, models, pin)) + path.capacitance_pf;
      for (const auto& buffer : choices) {
        const auto cell = estimate(*buffer.model, *input_slew, load);
        if (!cell) continue;
        const auto gain = bufferInsertionGain(*resistance + path.resistance_kohm, load, buffer.capacitance,
                                               cell->resistance, cell->delay);
        if (!gain || gain->gain_ns <= 0.0) continue;
        if (best && (gain->gain_ns < best->predicted_gain_ns
            || (gain->gain_ns == best->predicted_gain_ns && !(pin_key != best_pin ? timing_edit::detail::canonicalBytesLess(pin_key, best_pin)
                                              : timing_edit::detail::canonicalBytesLess(buffer.key, best_buffer))))) continue;
        std::array<std::int32_t, 2> target;
        const std::array midpoint{(driver.get_x() + pin.get_x()) * 0.5, (driver.get_y() + pin.get_y()) * 0.5};
        for (std::size_t axis = 0; axis < target.size(); ++axis) {
          if (!std::isfinite(midpoint[axis]) || midpoint[axis] < std::numeric_limits<std::int32_t>::min()
              || midpoint[axis] > std::numeric_limits<std::int32_t>::max())
            throw std::runtime_error("greedy: invalid physical target");
          target[axis] = static_cast<std::int32_t>(std::llround(midpoint[axis]));
        }
        CreationToken token{.case_id = database.get_design_name(), .profile_id = "physical-greedy",
            .batch_ordinal = generation + 1, .accepted_ordinal = result.candidates.size()};
        BufferInsertionMutation edit{critical.identity, {stable_pin}, buffer.reference,
            PlacedInputs{target[0] / static_cast<double>(dbu_per_um), target[1] / static_cast<double>(dbu_per_um)},
            ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree}, std::move(token)};
        best = PhysicalGreedyCandidate{std::move(edit), target, gain->gain_ns, *slack};
        best_pin = pin_key;
        best_buffer = buffer.key;
      }
    }
    if (best) {
      const auto names = defaultTokenMaterialization(best->edit.creation_token, best->edit.library_cell);
      if (database.get_instance_map().contains(names.instance_name) || database.get_net_map().contains(names.output_net_name)
          || database.get_pin_map().contains(names.input_pin_name) || database.get_pin_map().contains(names.output_pin_name))
        throw std::runtime_error("greedy: materialized name collision");
      accepted_nodes.insert(driver_key);
      accepted_nodes.insert(best_pin);
      result.candidates.push_back(std::move(*best));
    }
  }
  return result;
}
}  // namespace ista
