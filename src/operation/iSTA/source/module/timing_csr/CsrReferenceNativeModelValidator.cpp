// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

#include "CsrReferenceNativeModelValidatorInternal.hpp"
#include "CsrReferenceValidatorDetail.hpp"

namespace ista::csr {
namespace {

bool hasOuterWhitespace(const std::string& value)
{
  return !value.empty() && (std::isspace(static_cast<unsigned char>(value.front())) || std::isspace(static_cast<unsigned char>(value.back())));
}

bool isCanonicalTemperature(const std::string& value)
{
  if (value == "__unspecified_temperature__")
    return true;
  if (value.empty() || value == "-0")
    return false;
  std::size_t cursor = value.front() == '-' ? 1 : 0;
  if (cursor == value.size())
    return false;
  if (value[cursor] == '0') {
    ++cursor;
  } else {
    if (value[cursor] < '1' || value[cursor] > '9')
      return false;
    while (++cursor < value.size() && std::isdigit(static_cast<unsigned char>(value[cursor]))) {
    }
  }
  if (cursor == value.size())
    return true;
  if (value[cursor++] != '.' || cursor == value.size())
    return false;
  while (cursor < value.size() && std::isdigit(static_cast<unsigned char>(value[cursor])))
    ++cursor;
  return cursor == value.size() && value.back() >= '1' && value.back() <= '9';
}

}  // namespace

std::vector<CsrContractViolation> validateAnalysisContextIntrinsic(const StaAnalysisContext& context)
{
  detail::Violations errors;
  if (context.liberty_corner.empty() || context.temperature.empty() || context.rc_corner.empty())
    detail::reject(errors, "analysis_context", "all analysis-context values must be nonempty");
  if (hasOuterWhitespace(context.liberty_corner) || hasOuterWhitespace(context.temperature) || hasOuterWhitespace(context.rc_corner))
    detail::reject(errors, "analysis_context", "analysis-context values must not have leading or trailing whitespace");
  if (!isCanonicalTemperature(context.temperature))
    detail::reject(errors, "analysis_context.temperature", "temperature is not the canonical decimal or unspecified token");
  return errors;
}

namespace detail {
namespace {

void validateOptionalDouble(const std::optional<double>& value, bool nonnegative, Violations& errors, const std::string& field)
{
  if (value && (!isFinite(*value) || (nonnegative && *value < 0)))
    reject(errors, field, "present value has invalid numeric domain");
}

void validateOptionalMatrix(const OptionalLaneMatrix& values, bool nonnegative, Violations& errors, const std::string& field)
{
  for (const auto& row : values)
    for (const auto& value : row)
      validateOptionalDouble(value, nonnegative, errors, field);
}

void validateTableModel(const TimingTableModel& table, const TimingTableSignature* signature, Violations& errors, const std::string& field)
{
  if (signature && !(*signature == TimingTableSignature{table.present, table.variable1, table.variable2}))
    reject(errors, field, "model presence/variables differ from ArcKey signature");
  const bool variables_known = knownEnum(table.variable1) && knownEnum(table.variable2);
  if (!variables_known)
    reject(errors, field + ".variables", "table variable enum is unknown");
  if (!table.present) {
    if (table.variable1 != TableVariable::kNone || table.variable2 != TableVariable::kNone || !table.axes.empty() || !table.values.empty())
      reject(errors, field, "absent table must use none variables and own no data");
    return;
  }
  if (!variables_known)
    return;
  if (table.variable1 == TableVariable::kNone && table.variable2 != TableVariable::kNone)
    reject(errors, field + ".variables", "variable2 requires variable1");
  const std::size_t dimensions
      = static_cast<std::size_t>(table.variable1 != TableVariable::kNone) + static_cast<std::size_t>(table.variable2 != TableVariable::kNone);
  const bool axes_match = table.axes.size() == dimensions;
  if (!axes_match)
    reject(errors, field + ".axes", "axis count must match declared variables");
  for (const auto& axis : table.axes) {
    for (std::size_t index = 0; index < axis.size(); ++index) {
      if (!isFinite(axis[index]))
        reject(errors, field + ".axes", "axis value must be finite");
      if (index != 0 && !(axis[index - 1] < axis[index]))
        reject(errors, field + ".axes", "axis values must be strictly increasing");
    }
  }
  for (double value : table.values)
    if (!isFinite(value))
      reject(errors, field + ".values", "table value must be finite");
  if (table.values.empty())
    return;
  std::size_t expected = 1;
  bool populated_shape_valid = axes_match;
  for (const auto& axis : table.axes) {
    if (axis.empty()) {
      reject(errors, field + ".axes", "populated table axis must not be empty");
      populated_shape_valid = false;
    } else if (populated_shape_valid && !checkedProduct(expected, axis.size(), expected, errors, field + ".axes")) {
      populated_shape_valid = false;
    }
  }
  if (populated_shape_valid && table.values.size() != expected)
    reject(errors, field + ".values", "value count does not match axes");
}

void validateChild(const ChildTimingArcModel& child, Violations& errors, const std::string& field)
{
  if (!knownEnum(child.signature.sense) || !knownEnum(child.signature.trigger_transition) || !knownEnum(child.signature.check_transition))
    reject(errors, field + ".signature", "child timing signature contains an unknown enum");

  if (!isFinite(child.time_unit_scale) || child.time_unit_scale <= 0 || !isFinite(child.capacitance_unit_scale) || child.capacitance_unit_scale <= 0
      || !isFinite(child.slew_derate) || child.slew_derate <= 0) {
    reject(errors, field, "child scale or derate is invalid");
  }
  for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
    const bool thresholds_valid = isFinite(child.slew_lower_threshold_pct[transition]) && isFinite(child.slew_upper_threshold_pct[transition])
                                  && isFinite(child.input_threshold_pct[transition]) && isFinite(child.output_threshold_pct[transition])
                                  && child.slew_lower_threshold_pct[transition] >= 0 && child.slew_upper_threshold_pct[transition] <= 1
                                  && child.slew_lower_threshold_pct[transition] < child.slew_upper_threshold_pct[transition]
                                  && child.input_threshold_pct[transition] >= 0 && child.input_threshold_pct[transition] <= 1
                                  && child.output_threshold_pct[transition] >= 0 && child.output_threshold_pct[transition] <= 1;
    if (!thresholds_valid)
      reject(errors, field + ".thresholds", "threshold percentages are invalid");
    validateTableModel(child.delay_tables[transition], &child.signature.delay_tables[transition], errors, field + ".delay_tables");
    validateTableModel(child.slew_tables[transition], &child.signature.slew_tables[transition], errors, field + ".slew_tables");
    validateTableModel(child.check_tables[transition], nullptr, errors, field + ".check_tables");
  }
}

}  // namespace

void validateTransfer(const CellTransferModel& transfer, Violations& errors, const std::string& field)
{
  if (!isFinite(transfer.delay) || !isFinite(transfer.delay_min) || !isFinite(transfer.delay_max))
    reject(errors, field, "fallback delay is non-finite");
  std::vector<std::int32_t> child_indices;
  child_indices.reserve(transfer.children().size());
  for (const auto& child : transfer.children()) {
    validateChild(child, errors, field + ".children");
    child_indices.push_back(child.signature.arc_index);
  }
  std::ranges::sort(child_indices);
  if (std::ranges::adjacent_find(child_indices) != child_indices.end())
    reject(errors, field + ".children.signature.arc_index", "arc indices must be unique");
}
namespace {

const ReferencedLibraryCellModel* findReferencedCell(const CsrReferenceInput& input, const std::string& cell_name)
{
  const auto found = std::lower_bound(input.referenced_library_cells.begin(), input.referenced_library_cells.end(), cell_name,
                                      [](const ReferencedLibraryCellModel& cell, const std::string& name) { return byteStringLess(cell.cell_name, name); });
  return found != input.referenced_library_cells.end() && found->cell_name == cell_name ? &*found : nullptr;
}

const InstanceRecord* findInstance(const CsrReferenceInput& input, const std::string& instance_name)
{
  const auto found = std::lower_bound(input.instances.begin(), input.instances.end(), instance_name,
                                      [](const InstanceRecord& instance, const std::string& name) { return byteStringLess(instance.instance_name, name); });
  return found != input.instances.end() && found->instance_name == instance_name ? &*found : nullptr;
}

const LibraryPortModel* findLibraryPort(const ReferencedLibraryCellModel& cell, const std::string& port_name)
{
  const auto found = std::lower_bound(cell.ports.begin(), cell.ports.end(), port_name,
                                      [](const LibraryPortModel& port, const std::string& name) { return byteStringLess(port.name, name); });
  return found != cell.ports.end() && found->name == port_name ? &*found : nullptr;
}

bool isInputLike(PinDirection direction)
{
  return direction == PinDirection::kInput || direction == PinDirection::kInout;
}

bool isOutputLike(PinDirection direction)
{
  return direction == PinDirection::kOutput || direction == PinDirection::kInout;
}

void validateLibraryRoleDerivation(const ReferencedLibraryCellModel& cell, Violations& errors)
{
  const LibraryTimingCheckArcModel* representative = nullptr;
  for (const auto& check : cell.timing_checks) {
    if (representative == nullptr)
      representative = &check;
    if (check.kind == TimingCheckKind::kSetup) {
      representative = &check;
      break;
    }
  }
  const std::string expected_clock = representative == nullptr ? std::string{} : representative->clock_port;
  const std::string expected_data = representative == nullptr ? std::string{} : representative->data_port;

  std::string expected_output;
  const auto clock_arc = std::ranges::find_if(cell.cell_arcs, [](const LibraryCellArcModel& arc) { return arc.transfer.is_clock_arc; });
  if (clock_arc != cell.cell_arcs.end()) {
    expected_output = clock_arc->sink_port;
  } else {
    const auto output = std::ranges::find_if(cell.ports, [](const LibraryPortModel& port) { return port.is_output && !port.is_clock; });
    if (output != cell.ports.end())
      expected_output = output->name;
  }

  if (cell.clock_pin_name != expected_clock || cell.data_pin_name != expected_data || cell.output_pin_name != expected_output)
    reject(errors, "referenced_library_cells.roles", "cell timing roles differ from the frozen first-check/first-clock-arc derivation");
}

void validateInstanceLibraryMirrors(const CsrReferenceInput& input, Violations& errors)
{
  for (const auto& instance : input.instances) {
    const auto* cell = findReferencedCell(input, instance.cell_name);
    const bool present = cell != nullptr && cell->present;
    if (instance.is_sequential != (present && cell->is_sequential) || instance.is_clock_gating != (present && cell->is_clock_gating)
        || instance.is_macro != (present && cell->is_macro))
      reject(errors, "instances.library_roles", "instance flags differ from its referenced Liberty cell");

    for (NodeId pin_id : instance.ordered_pins) {
      if (pin_id.value() >= input.nodes.size())
        continue;
      const auto& pin = input.nodes[pin_id.value()];
      const bool expected_clock = present && pin.key.pin_name == cell->clock_pin_name;
      const bool expected_data = present && pin.key.pin_name == cell->data_pin_name;
      const bool expected_output = present && pin.key.pin_name == cell->output_pin_name;
      if (pin.is_sequential != instance.is_sequential || pin.is_clock_pin != expected_clock || pin.is_data_pin != expected_data
          || pin.is_output_pin != expected_output)
        reject(errors, "nodes.library_roles", "pin roles differ from its referenced Liberty cell");
      if (present) {
        const auto* port = findLibraryPort(*cell, pin.key.pin_name);
        if (port != nullptr && pin.direction != port->direction)
          reject(errors, "nodes.direction", "pin direction differs from its referenced Liberty port");
      }
    }
  }
}

bool isUnitFallbackTransfer(const ArcRecord& arc)
{
  return arc.transfer.delay == 1.0 && arc.transfer.delay_min == 1.0 && arc.transfer.delay_max == 1.0 && arc.transfer.is_clock_arc == arc.is_clock_arc
         && arc.transfer.is_disabled == arc.is_disabled && !arc.transfer.is_clear_or_preset && arc.transfer.children().empty();
}

using FallbackArcKey = std::tuple<std::string, std::uint64_t, std::uint64_t>;

void validateGraphTransferMirrors(const CsrReferenceInput& input, Violations& errors)
{
  std::set<FallbackArcKey> actual_fallback;
  for (const auto& arc : input.arcs) {
    if (arc.kind != ArcKind::kCell && arc.native_cell_arc_ordinal)
      reject(errors, "arcs.native_cell_arc_ordinal", "net arc cannot carry a Liberty cell-arc identity");
    if (arc.kind != ArcKind::kCell || arc.source.value() >= input.nodes.size() || arc.sink.value() >= input.nodes.size())
      continue;
    const auto& source = input.nodes[arc.source.value()];
    const auto& sink = input.nodes[arc.sink.value()];
    const auto* instance = findInstance(input, source.key.instance_name);
    const auto* cell = instance == nullptr ? nullptr : findReferencedCell(input, instance->cell_name);
    const bool fallback_cell = cell == nullptr || !cell->present || cell->cell_arcs.empty();
    if (fallback_cell) {
      if (arc.native_cell_arc_ordinal)
        reject(errors, "arcs.native_cell_arc_ordinal", "fallback arc cannot carry a Liberty cell-arc identity");
      if (!isInputLike(source.direction) || !isOutputLike(sink.direction) || arc.source == arc.sink || !isUnitFallbackTransfer(arc))
        reject(errors, "arcs.transfer", "fallback cell arc must use one exact input-like to output-like unit transfer");
      if (!actual_fallback.emplace(source.key.instance_name, arc.source.value(), arc.sink.value()).second)
        reject(errors, "arcs.transfer", "fallback cell arc pair is duplicated");
      continue;
    }

    bool mirrored = false;
    if (arc.native_cell_arc_ordinal && *arc.native_cell_arc_ordinal < cell->cell_arcs.size()) {
      const auto& model = cell->cell_arcs[*arc.native_cell_arc_ordinal];
      mirrored = model.source_port == source.key.pin_name && model.sink_port == sink.key.pin_name && model.transfer == arc.transfer;
    }
    if (!mirrored)
      reject(errors, "arcs.transfer", "bound cell transfer must exactly mirror one referenced Liberty cell arc");
  }

  std::set<FallbackArcKey> expected_fallback;
  for (const auto& instance : input.instances) {
    const auto* cell = findReferencedCell(input, instance.cell_name);
    if (cell != nullptr && cell->present && !cell->cell_arcs.empty())
      continue;
    for (NodeId source : instance.ordered_pins) {
      if (source.value() >= input.nodes.size() || !isInputLike(input.nodes[source.value()].direction))
        continue;
      for (NodeId sink : instance.ordered_pins)
        if (sink.value() < input.nodes.size() && source != sink && isOutputLike(input.nodes[sink.value()].direction))
          expected_fallback.emplace(instance.instance_name, source.value(), sink.value());
    }
  }
  if (actual_fallback != expected_fallback)
    reject(errors, "arcs.transfer", "fallback cell arcs must exactly equal the per-instance Cartesian product");
}

void validateConvertedLoadRanges(const CsrReferenceInput& input, Violations& errors)
{
  constexpr double kStaError = 1e-6;
  for (const auto& arc : input.arcs) {
    if (!arc.output_load)
      continue;
    for (const auto& child : arc.transfer.children()) {
      if (std::abs(child.capacitance_unit_scale - 1.0) < kStaError)
        continue;
      for (const auto& analysis : *arc.output_load) {
        for (double load : analysis) {
          const double converted = std::ceil(load * child.capacitance_unit_scale);
          if (!isFinite(converted) || converted < 0.0 || converted > static_cast<double>(std::numeric_limits<int>::max()))
            reject(errors, "arcs.output_load", "converted load is outside the native table-index range");
        }
      }
    }
  }
}

void validateLibraryClosure(const CsrReferenceInput& input, Violations& errors)
{
  std::vector<std::string> expected_cells;
  expected_cells.reserve(input.instances.size());
  for (const auto& instance : input.instances)
    expected_cells.push_back(instance.cell_name);
  std::ranges::sort(expected_cells, byteStringLess);
  expected_cells.erase(std::ranges::unique(expected_cells).begin(), expected_cells.end());
  std::vector<std::string> actual_cells;
  actual_cells.reserve(input.referenced_library_cells.size());
  for (const auto& cell : input.referenced_library_cells)
    actual_cells.push_back(cell.cell_name);
  if (actual_cells != expected_cells)
    reject(errors, "referenced_library_cells", "closure must exactly cover sorted unique referenced cell names");

  for (const auto& cell : input.referenced_library_cells)
    validateLibraryCell(cell, errors);
  validateInstanceLibraryMirrors(input, errors);
}

void validateParasitics(const ParasiticModel& parasitics, Violations& errors)
{
  if (parasitics.nets.empty()) {
    if (!parasitics.capacitance_unit.empty() || !parasitics.resistance_unit.empty()
        || parasitics.input_port_thresholds != InputPortRcThresholds{})
      reject(errors, "parasitics", "empty parasitic domain must normalize unit strings to empty");
    return;
  }
  const auto& thresholds = parasitics.input_port_thresholds;
  if (!isFinite(thresholds.slew_derate)
      || std::ranges::any_of(thresholds.lower, [](double value) { return !isFinite(value); })
      || std::ranges::any_of(thresholds.upper, [](double value) { return !isFinite(value); }))
    reject(errors, "parasitics.input_port_thresholds", "global thresholds must be finite");
  if (parasitics.capacitance_unit.empty() || parasitics.resistance_unit.empty())
    reject(errors, "parasitics", "nonempty parasitic domain requires both unit strings");
  for (std::size_t net_index = 0; net_index < parasitics.nets.size(); ++net_index) {
    const auto& net = parasitics.nets[net_index];
    if (net_index != 0 && !byteStringLess(parasitics.nets[net_index - 1].map_key, net.map_key))
      reject(errors, "parasitics.nets", "nets must follow strict native map-key byte order");
    validateParasiticNet(net, errors);
  }
}

}  // namespace

void validateParasiticNet(const ParasiticNetModel& net, Violations& errors)
{
  if (!isFinite(net.lumped_capacitance) || net.lumped_capacitance < 0)
    reject(errors, "parasitics.nets.lumped_capacitance", "lumped capacitance must be finite and nonnegative");
  for (std::size_t node_index = 0; node_index < net.nodes.size(); ++node_index) {
    const auto& node = net.nodes[node_index];
    if (node_index != 0 && !byteStringLess(net.nodes[node_index - 1].map_key, node.map_key))
      reject(errors, "parasitics.nets.nodes", "nodes must follow strict native map-key byte order");
    if (!isFinite(node.capacitance) || node.capacitance < 0 || !isFinite(node.x) || !isFinite(node.y))
      reject(errors, "parasitics.nets.nodes", "node capacitance and coordinates are invalid");
  }
  for (const auto& resistor : net.resistors)
    if (!isFinite(resistor.resistance) || resistor.resistance < 0)
      reject(errors, "parasitics.nets.resistors", "resistance must be finite and nonnegative");
}

std::vector<CsrContractViolation> validateParasiticContextCrossField(const CsrReferenceInput& input)
{
  Violations errors;
  if (!input.parasitics.nets.empty() && input.analysis_context.rc_corner == "__no_rc_corner__")
    reject(errors, "analysis_context.rc_corner", "parasitic input requires an explicit RC corner");
  return errors;
}

void validateLibraryCell(const ReferencedLibraryCellModel& cell, Violations& errors)
{
    if (!isFinite(cell.slew_derate))
      reject(errors, "referenced_library_cells.slew_derate", "load slew derate must be finite");
    for (const auto& thresholds : {cell.slew_lower_threshold_pct, cell.slew_upper_threshold_pct, cell.input_threshold_pct})
      for (double value : thresholds)
        if (!isFinite(value) || value < 0.0 || value > 1.0)
          reject(errors, "referenced_library_cells.thresholds", "load thresholds must be normalized finite values");
    if (!cell.present) {
      const ReferencedLibraryCellModel empty;
      if (cell.slew_derate != empty.slew_derate || cell.slew_lower_threshold_pct != empty.slew_lower_threshold_pct
          || cell.slew_upper_threshold_pct != empty.slew_upper_threshold_pct || cell.input_threshold_pct != empty.input_threshold_pct
          || !cell.library_name.empty() || cell.is_sequential || cell.is_clock_gating || cell.is_macro || !cell.clock_pin_name.empty()
          || !cell.data_pin_name.empty() || !cell.output_pin_name.empty() || !cell.ports.empty() || !cell.cell_arcs.empty() || !cell.timing_checks.empty()) {
        reject(errors, "referenced_library_cells", "missing cell must own no present payload");
      }
      return;
    }
    if (cell.library_name.empty())
      reject(errors, "referenced_library_cells.library_name", "present cell requires a library name");
    for (std::size_t port_index = 0; port_index < cell.ports.size(); ++port_index) {
      const auto& port = cell.ports[port_index];
      if (!knownEnum(port.direction))
        reject(errors, "referenced_library_cells.ports.direction", "port direction enum is unknown");
      if (port_index != 0 && !byteStringLess(cell.ports[port_index - 1].name, port.name))
        reject(errors, "referenced_library_cells.ports", "ports must follow strict native map-key byte order");
      const bool output_direction = port.direction == PinDirection::kOutput || port.direction == PinDirection::kInout;
      if (port.is_output != output_direction)
        reject(errors, "referenced_library_cells.ports.is_output", "output role differs from direction");
      if (!isFinite(port.default_capacitance) || port.default_capacitance < 0)
        reject(errors, "referenced_library_cells.ports.default_capacitance", "capacitance must be finite and nonnegative");
      validateOptionalMatrix(port.transition_capacitance, true, errors, "referenced_library_cells.ports.transition_capacitance");
    }
    for (const auto& arc : cell.cell_arcs) {
      if (arc.source_port.empty() || arc.sink_port.empty())
        reject(errors, "referenced_library_cells.cell_arcs", "cell arc ports must be nonempty");
      else if (findLibraryPort(cell, arc.source_port) == nullptr || findLibraryPort(cell, arc.sink_port) == nullptr)
        reject(errors, "referenced_library_cells.cell_arcs", "cell arc ports must resolve in the referenced port map");
      validateTransfer(arc.transfer, errors, "referenced_library_cells.cell_arcs.transfer");
    }
    for (const auto& check : cell.timing_checks) {
      if (!knownEnum(check.kind))
        reject(errors, "referenced_library_cells.timing_checks.kind", "timing-check kind is unknown");
      if (!isFinite(check.check_time))
        reject(errors, "referenced_library_cells.timing_checks.check_time", "check time must be finite");
      if (!knownEnum(check.clock_transition) || check.clock_transition == Transition::kNone)
        reject(errors, "referenced_library_cells.timing_checks.clock_transition", "clock transition must be rise or fall");
      for (const auto& child : check.children())
        validateChild(child, errors, "referenced_library_cells.timing_checks.children");
    }
    validateLibraryRoleDerivation(cell, errors);
}

void validateReferenceNativeModels(const CsrReferenceInput& input, Violations& errors)
{
  for (const auto& arc : input.arcs) {
    validateTransfer(arc.transfer, errors, "arcs.transfer");
    if (arc.kind == ArcKind::kNet) {
      CellTransferModel canonical;
      canonical.delay = 0.0;
      canonical.delay_min = 0.0;
      canonical.delay_max = 0.0;
      if (!(arc.transfer == canonical) || arc.is_clock_arc)
        reject(errors, "arcs.transfer", "net arc transfer must use the canonical unbound model");
    } else if (arc.transfer.is_clock_arc != arc.is_clock_arc || arc.transfer.is_disabled != arc.is_disabled) {
      reject(errors, "arcs.transfer", "cell transfer and ArcRecord flags differ");
    }
    if (arc.transfer.children().size() != arc.key.base.library_arcs.size())
      reject(errors, "arcs.transfer.children", "child count differs from ArcKey signature");
    for (std::size_t child = 0; child < arc.transfer.children().size() && child < arc.key.base.library_arcs.size(); ++child)
      if (!(arc.transfer.children()[child].signature == arc.key.base.library_arcs[child]))
        reject(errors, "arcs.transfer.children.signature", "child signature differs from ArcKey");
    if (arc.output_load)
      for (const auto& analysis : *arc.output_load)
        for (double value : analysis)
          if (!isFinite(value) || value < 0)
            reject(errors, "arcs.output_load", "load must be finite and nonnegative");
  }
  std::set<std::string_view> rc_nets;
  for (const auto& net : input.parasitics.nets)
    rc_nets.insert(net.map_key);
  for (const auto& arc : input.arcs) {
    if (arc.sink.value() >= input.nodes.size())
      continue;  // Structural validation reports the invalid NodeId.
    const bool loads_present = !rc_nets.contains(input.nodes[arc.sink.value()].net_name);
    if (arc.output_load.has_value() != loads_present)
      reject(errors, "arcs.output_load", "load presence must follow its output net's parasitic state");
  }

  for (const auto& clock : input.clocks) {
    if (!isFinite(clock.period))
      reject(errors, "clocks.period", "period must be finite");
    for (double edge : clock.edges)
      if (!isFinite(edge))
        reject(errors, "clocks.edges", "edge must be finite");
  }
  for (const auto& constraint : input.port_constraints) {
    for (const auto& value : constraint.input_delay)
      validateOptionalDouble(value, false, errors, "port_constraints.input_delay");
    validateOptionalDouble(constraint.input_transition, true, errors, "port_constraints.input_transition");
    for (const auto& value : constraint.output_delay)
      validateOptionalDouble(value, false, errors, "port_constraints.output_delay");
    validateOptionalDouble(constraint.output_load, true, errors, "port_constraints.output_load");
  }
  for (const auto& seed : input.start_seeds) {
    validateOptionalMatrix(seed.arrival, false, errors, "start_seeds.arrival");
    validateOptionalMatrix(seed.slew, true, errors, "start_seeds.slew");
  }
  for (const auto& endpoint : input.endpoint_required) {
    if (!isFinite(endpoint.clock_period))
      reject(errors, "endpoint_required.clock_period", "resolved clock period must be finite");
    for (const auto& value : endpoint.output_delay)
      validateOptionalDouble(value, false, errors, "endpoint_required.output_delay");
  }
  for (const auto& capacitance : input.cell_pin_capacitances) {
    if (!isFinite(capacitance.default_capacitance) || capacitance.default_capacitance < 0)
      reject(errors, "cell_pin_capacitances.default", "capacitance must be finite and nonnegative");
    validateOptionalMatrix(capacitance.transition_capacitance, true, errors, "cell_pin_capacitances.transition");
  }
  validateLibraryClosure(input, errors);
  validateGraphTransferMirrors(input, errors);
  validateConvertedLoadRanges(input, errors);
  validateParasitics(input.parasitics, errors);
}

}  // namespace detail
}  // namespace ista::csr
