// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalGreedyScoring.hpp"
#include "BufferInsertionGain.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrTableLookup.hpp"
#include "NativeBufferCellValidation.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditByteOrder.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace ista::physical_greedy {
using namespace timing_edit;
double pinCapacitance(const csr::LibraryPortModel& pin)
{
  double value = 0.0;
  for (const auto& transition : pin.transition_capacitance[0])
    value = std::max(value, transition.value_or(pin.default_capacitance));
  if (!std::isfinite(value) || value < 0.0) throw std::runtime_error("greedy: invalid pin capacitance");
  return value;
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
std::optional<PhysicalGreedyCandidate> score(const Net& net, std::span<const Buffer> choices,
    const CreationToken& token, std::int32_t dbu_per_um)
{
  std::optional<PhysicalGreedyCandidate> best;
  Bytes best_pin, best_buffer;
  for (const auto& pin : net.sinks) {
    const auto pin_key = canonicalStableNodeRefBytes(pin.identity);
    for (const auto& buffer : choices) {
      const auto cell = estimate(*buffer.model, net.input_slew, pin.load);
      if (!cell) continue;
      const auto gain = bufferInsertionGain(net.driver_resistance + pin.path_resistance, pin.load,
          buffer.capacitance, cell->resistance, cell->delay);
      if (!gain || gain->gain_ns <= 0.0) continue;
      if (best && (gain->gain_ns < best->predicted_gain_ns
          || (gain->gain_ns == best->predicted_gain_ns && !(pin_key != best_pin
              ? timing_edit::detail::canonicalBytesLess(pin_key, best_pin)
              : timing_edit::detail::canonicalBytesLess(buffer.key, best_buffer))))) continue;
      std::array<std::int32_t, 2> target;
      for (std::size_t axis = 0; axis < target.size(); ++axis) {
        const double midpoint = (net.driver_dbu[axis] + pin.position_dbu[axis]) * 0.5;
        if (!std::isfinite(midpoint) || midpoint < std::numeric_limits<std::int32_t>::min()
            || midpoint > std::numeric_limits<std::int32_t>::max())
          throw std::runtime_error("greedy: invalid physical target");
        target[axis] = static_cast<std::int32_t>(std::llround(midpoint));
      }
      BufferInsertionMutation edit{net.identity, {pin.identity}, buffer.reference,
          PlacedInputs{target[0] / static_cast<double>(dbu_per_um), target[1] / static_cast<double>(dbu_per_um)},
          ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree}, token};
      best = PhysicalGreedyCandidate{std::move(edit), target, gain->gain_ns, pin.slack};
      best_pin = pin_key;
      best_buffer = buffer.key;
    }
  }
  return best;
}
}  // namespace ista::physical_greedy
