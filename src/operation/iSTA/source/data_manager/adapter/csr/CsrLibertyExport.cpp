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
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::detail {
namespace {

std::size_t transitionIndex(ista::TransType transition)
{
  switch (transition) {
    case ista::TransType::kRise:
      return 0;
    case ista::TransType::kFall:
      return 1;
    case ista::TransType::kNone:
      invalidReferenceTable();
  }
  invalidReferenceTable();
}

std::size_t analysisIndex(ista::AnalysisType analysis)
{
  switch (analysis) {
    case ista::AnalysisType::kMax:
      return 0;
    case ista::AnalysisType::kMin:
      return 1;
    case ista::AnalysisType::kNone:
      malformedReferenceInput();
  }
  malformedReferenceInput();
}

TimingTableSignature tableSignature(const std::map<ista::TransType, ista::TimingTable>& tables, ista::TransType transition)
{
  const auto found = tables.find(transition);
  if (found == tables.end())
    return {};
  return {true, convertReferenceTableVariable(found->second.get_variable_type1()), convertReferenceTableVariable(found->second.get_variable_type2())};
}

void exportTableFamily(const std::map<ista::TransType, ista::TimingTable>& native_tables, std::array<TimingTableModel, kTransitionCount>& tables)
{
  for (const auto& [transition, table] : native_tables)
    tables[transitionIndex(transition)] = exportReferenceTimingTable(table);
}

void validateChildScalars(const ChildTimingArcModel& child)
{
  requireFiniteReference(child.time_unit_scale);
  requireFiniteReference(child.capacitance_unit_scale);
  requireFiniteReference(child.slew_derate);
  if (child.time_unit_scale <= 0.0 || child.capacitance_unit_scale <= 0.0 || child.slew_derate <= 0.0)
    malformedReferenceInput();

  for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
    const double lower = child.slew_lower_threshold_pct[transition];
    const double upper = child.slew_upper_threshold_pct[transition];
    const double input = child.input_threshold_pct[transition];
    const double output = child.output_threshold_pct[transition];
    requireFiniteReference(lower);
    requireFiniteReference(upper);
    requireFiniteReference(input);
    requireFiniteReference(output);
    if (lower < 0.0 || lower >= upper || upper > 1.0 || input < 0.0 || input > 1.0 || output < 0.0 || output > 1.0)
      malformedReferenceInput();
  }
}

LibraryPortModel exportLibraryPort(const ista::TimingCellPort& native_port)
{
  LibraryPortModel port;
  port.name = native_port.get_port_name();
  port.direction = convertReferencePortDirection(native_port.get_is_input(), native_port.get_is_output());
  port.is_clock = native_port.get_is_clock();
  port.is_output = native_port.get_is_output();
  port.default_capacitance = native_port.get_capacitance();
  requireNonnegativeReference(port.default_capacitance);

  for (const auto& [analysis, transitions] : native_port.get_trans_capacitance_map()) {
    const std::size_t analysis_index = analysisIndex(analysis);
    for (const auto& [transition, value] : transitions) {
      const std::size_t transition_index = transitionIndex(transition);
      requireNonnegativeReference(value);
      port.transition_capacitance[analysis_index][transition_index] = value;
    }
  }
  return port;
}

LibraryTimingCheckArcModel exportLibraryTimingCheck(const ista::TimingCheckArc& native_check)
{
  LibraryTimingCheckArcModel check;
  check.clock_port = native_check.get_clock_port();
  check.data_port = native_check.get_data_port();
  check.kind = convertReferenceTimingCheckKind(native_check.get_check_type());
  check.check_time = native_check.get_check_time();
  check.clock_transition = convertReferenceTransition(native_check.get_clock_trans_type());
  requireFiniteReference(check.check_time);
  if (check.clock_transition == Transition::kNone)
    malformedReferenceInput();
  check.children.reserve(native_check.get_timing_arc_list().size());
  for (const auto& native_child : native_check.get_timing_arc_list())
    check.children.push_back(exportReferenceChildTimingArc(native_child));
  return check;
}

void assignLibraryRoles(const ista::TimingCell& native_cell, ReferencedLibraryCellModel& cell)
{
  const ista::TimingCellArc* selected_clock_arc = nullptr;
  for (const auto& native_arc : native_cell.get_cell_arc_list()) {
    if (native_arc.get_is_clock_arc()) {
      selected_clock_arc = &native_arc;
      break;
    }
  }
  if (selected_clock_arc != nullptr)
    cell.output_pin_name = selected_clock_arc->get_sink_port();

  const ista::TimingCheckArc* representative_check = nullptr;
  for (const auto& native_check : native_cell.get_check_arc_list()) {
    if (representative_check == nullptr || native_check.get_check_type() == ista::TimingCheckType::kSetup)
      representative_check = &native_check;
    if (native_check.get_check_type() == ista::TimingCheckType::kSetup)
      break;
  }
  if (representative_check != nullptr) {
    cell.clock_pin_name = representative_check->get_clock_port();
    cell.data_pin_name = representative_check->get_data_port();
  }

  if (cell.output_pin_name.empty()) {
    for (const auto& [map_key, native_port] : native_cell.get_port_map()) {
      static_cast<void>(map_key);
      if (native_port.get_is_output() && !native_port.get_is_clock()) {
        cell.output_pin_name = native_port.get_port_name();
        break;
      }
    }
  }
}

void exportReferencedCells(ReferenceBuildState& state)
{
  std::set<std::string, decltype(&referenceByteStringLess)> names(&referenceByteStringLess);
  for (const auto& instance : state.input.instances)
    names.insert(instance.cell_name);

  state.input.referenced_library_cells.reserve(names.size());
  for (const auto& cell_name : names) {
    ReferencedLibraryCellModel cell;
    cell.cell_name = cell_name;
    const ista::TimingCell* native_cell = findNativeTimingCell(state, cell_name);
    if (native_cell != nullptr) {
      cell.present = true;
      cell.library_name = native_cell->get_library_name();
      if (cell.library_name.empty())
        malformedReferenceInput();
      cell.is_sequential = native_cell->get_is_sequential();
      cell.is_clock_gating = native_cell->get_is_clock_gating();
      cell.is_macro = native_cell->get_is_macro();
      assignLibraryRoles(*native_cell, cell);

      cell.ports.reserve(native_cell->get_port_map().size());
      for (const auto& [map_key, native_port] : native_cell->get_port_map()) {
        if (map_key != native_port.get_port_name())
          malformedReferenceInput();
        cell.ports.push_back(exportLibraryPort(native_port));
      }

      cell.cell_arcs.reserve(native_cell->get_cell_arc_list().size());
      for (const auto& native_arc : native_cell->get_cell_arc_list()) {
        if (native_arc.get_source_port().empty() || native_arc.get_sink_port().empty()
            || native_cell->get_port_map().find(native_arc.get_source_port()) == native_cell->get_port_map().end()
            || native_cell->get_port_map().find(native_arc.get_sink_port()) == native_cell->get_port_map().end())
          malformedReferenceInput();
        cell.cell_arcs.push_back({native_arc.get_source_port(), native_arc.get_sink_port(), exportReferenceCellTransfer(native_arc)});
      }

      cell.timing_checks.reserve(native_cell->get_check_arc_list().size());
      for (const auto& native_check : native_cell->get_check_arc_list())
        cell.timing_checks.push_back(exportLibraryTimingCheck(native_check));
    }

    const std::size_t index = state.input.referenced_library_cells.size();
    if (!state.referenced_cell_indices_by_name.emplace(cell.cell_name, index).second)
      malformedReferenceInput();
    state.input.referenced_library_cells.push_back(std::move(cell));
  }
}

const ReferencedLibraryCellModel& requireReferencedCell(const ReferenceBuildState& state, const std::string& cell_name)
{
  const auto found = state.referenced_cell_indices_by_name.find(cell_name);
  if (found == state.referenced_cell_indices_by_name.end() || found->second >= state.input.referenced_library_cells.size())
    malformedReferenceInput();
  return state.input.referenced_library_cells[found->second];
}

void refreshInstanceRoles(ReferenceBuildState& state)
{
  for (auto& instance : state.input.instances) {
    const auto& cell = requireReferencedCell(state, instance.cell_name);
    instance.is_sequential = cell.present && cell.is_sequential;
    instance.is_clock_gating = cell.present && cell.is_clock_gating;
    instance.is_macro = cell.present && cell.is_macro;

    for (NodeId node_id : instance.ordered_pins) {
      NodeRecord& node = state.input.nodes[node_id.value()];
      node.is_sequential = instance.is_sequential;
      node.is_clock_pin = cell.present && node.key.pin_name == cell.clock_pin_name;
      node.is_data_pin = cell.present && node.key.pin_name == cell.data_pin_name;
      node.is_output_pin = cell.present && node.key.pin_name == cell.output_pin_name;
    }
  }
}

void exportGraphTransfers(ReferenceBuildState& state)
{
  for (auto& arc : state.input.arcs) {
    if (arc.kind == ArcKind::kNet) {
      arc.transfer = {};
      arc.transfer.delay = 0.0;
      arc.transfer.delay_min = 0.0;
      arc.transfer.delay_max = 0.0;
      continue;
    }

    const ista::Arc& native_arc = requireNativeArc(state, arc);
    const ista::TimingCellArc* native_cell_arc = native_arc.get_timing_cell_arc();
    if (native_cell_arc == nullptr) {
      arc.transfer = {};
      arc.transfer.is_clock_arc = arc.is_clock_arc;
      arc.transfer.is_disabled = arc.is_disabled;
    } else {
      arc.transfer = exportReferenceCellTransfer(*native_cell_arc);
    }

    if (arc.transfer.is_clock_arc != arc.is_clock_arc || arc.transfer.is_disabled != arc.is_disabled
        || arc.transfer.children.size() != arc.key.base.library_arcs.size())
      malformedReferenceInput();
    for (std::size_t child = 0; child < arc.transfer.children.size(); ++child)
      if (!(arc.transfer.children[child].signature == arc.key.base.library_arcs[child]))
        malformedReferenceInput();
  }
}

void exportCellPinCapacitances(ReferenceBuildState& state)
{
  std::vector<std::size_t> node_order;
  node_order.reserve(state.input.nodes.size());
  for (std::size_t node = 0; node < state.input.nodes.size(); ++node)
    if (!state.input.nodes[node].is_port)
      node_order.push_back(node);
  std::ranges::sort(node_order, [&](std::size_t lhs, std::size_t rhs) {
    return referenceByteStringLess(state.input.nodes[lhs].key.full_name, state.input.nodes[rhs].key.full_name);
  });

  state.input.cell_pin_capacitances.reserve(node_order.size());
  for (std::size_t node_index : node_order) {
    const NodeRecord& node = state.input.nodes[node_index];
    CellPinCapacitanceModel capacitance;
    capacitance.node = node.id;

    const auto instance = state.database.get_instance_map().find(node.key.instance_name);
    if (instance == state.database.get_instance_map().end())
      malformedReferenceInput();
    const ista::TimingCell* native_cell = findNativeTimingCell(state, instance->second.get_cell_name());
    if (native_cell != nullptr) {
      const auto port = native_cell->get_port_map().find(node.key.pin_name);
      if (port != native_cell->get_port_map().end()) {
        const LibraryPortModel exported = exportLibraryPort(port->second);
        capacitance.default_capacitance = exported.default_capacitance;
        capacitance.transition_capacitance = exported.transition_capacitance;
      }
    }

    const std::size_t cap_index = state.input.cell_pin_capacitances.size();
    state.capacitance_indices_by_node[node.id.value()] = cap_index;
    state.input.cell_pin_capacitances.push_back(std::move(capacitance));
  }
}

}  // namespace

Transition convertReferenceTransition(ista::TransType transition)
{
  switch (transition) {
    case ista::TransType::kRise:
      return Transition::kRise;
    case ista::TransType::kFall:
      return Transition::kFall;
    case ista::TransType::kNone:
      return Transition::kNone;
  }
  malformedReferenceInput();
}

TimingSense convertReferenceTimingSense(ista::TimingArcSense sense)
{
  switch (sense) {
    case ista::TimingArcSense::kNone:
      return TimingSense::kNoneUsesPositiveFallback;
    case ista::TimingArcSense::kPositive:
      return TimingSense::kPositiveUnate;
    case ista::TimingArcSense::kNegative:
      return TimingSense::kNegativeUnate;
    case ista::TimingArcSense::kNonUnate:
      return TimingSense::kNonUnate;
  }
  malformedReferenceInput();
}

TableVariable convertReferenceTableVariable(ista::TimingTableVariableType variable)
{
  switch (variable) {
    case ista::TimingTableVariableType::kNone:
      return TableVariable::kNone;
    case ista::TimingTableVariableType::kInputTransition:
      return TableVariable::kInputTransition;
    case ista::TimingTableVariableType::kOutputCapacitance:
      return TableVariable::kOutputCapacitance;
    case ista::TimingTableVariableType::kConstrainedTransition:
      return TableVariable::kConstrainedTransition;
  }
  invalidReferenceTable();
}

TimingCheckKind convertReferenceTimingCheckKind(ista::TimingCheckType kind)
{
  switch (kind) {
    case ista::TimingCheckType::kNone:
      return TimingCheckKind::kNone;
    case ista::TimingCheckType::kSetup:
      return TimingCheckKind::kSetup;
    case ista::TimingCheckType::kHold:
      return TimingCheckKind::kHold;
    case ista::TimingCheckType::kRecovery:
      return TimingCheckKind::kRecovery;
    case ista::TimingCheckType::kRemoval:
      return TimingCheckKind::kRemoval;
    case ista::TimingCheckType::kWidth:
      return TimingCheckKind::kWidth;
    case ista::TimingCheckType::kPeriod:
      return TimingCheckKind::kPeriod;
  }
  malformedReferenceInput();
}

PinDirection convertReferencePortDirection(bool is_input, bool is_output)
{
  if (is_input && is_output)
    return PinDirection::kInout;
  if (is_input)
    return PinDirection::kInput;
  if (is_output)
    return PinDirection::kOutput;
  return PinDirection::kNone;
}

TimingTableModel exportReferenceTimingTable(const ista::TimingTable& table)
{
  TimingTableModel model;
  model.present = true;
  model.variable1 = convertReferenceTableVariable(table.get_variable_type1());
  model.variable2 = convertReferenceTableVariable(table.get_variable_type2());
  model.axes = table.get_axis_list();
  model.values = table.get_value_list();

  if (model.variable1 == TableVariable::kNone && model.variable2 != TableVariable::kNone)
    invalidReferenceTable();
  const std::size_t dimension = model.variable1 == TableVariable::kNone ? 0 : (model.variable2 == TableVariable::kNone ? 1 : 2);
  if (model.axes.size() != dimension)
    invalidReferenceTable();

  for (const auto& axis : model.axes) {
    for (double value : axis)
      requireFiniteReference(value);
    if (!std::ranges::is_sorted(axis, std::less<>{}) || std::ranges::adjacent_find(axis) != axis.end())
      invalidReferenceTable();
  }
  for (double value : model.values)
    requireFiniteReference(value);

  if (model.values.empty())
    return model;
  if (dimension == 0) {
    if (model.values.size() != 1)
      invalidReferenceTable();
    return model;
  }

  std::size_t product = 1;
  for (const auto& axis : model.axes) {
    if (axis.empty() || product > std::numeric_limits<std::size_t>::max() / axis.size())
      invalidReferenceTable();
    product *= axis.size();
  }
  if (model.values.size() != product)
    invalidReferenceTable();
  return model;
}

LibraryArcSignature exportReferenceLibraryArcSignature(const ista::TimingArc& arc)
{
  LibraryArcSignature signature;
  signature.library_name = arc.get_library_name();
  signature.arc_index = arc.get_arc_idx();
  signature.sense = convertReferenceTimingSense(arc.get_sense());
  signature.trigger_transition = convertReferenceTransition(arc.get_trigger_trans_type());
  signature.check_transition = convertReferenceTransition(arc.get_check_trans_type());
  signature.sdf_condition = arc.get_sdf_cond();
  for (ista::TransType transition : {ista::TransType::kRise, ista::TransType::kFall}) {
    const std::size_t index = transitionIndex(transition);
    signature.delay_tables[index] = tableSignature(arc.get_delay_table_map(), transition);
    signature.slew_tables[index] = tableSignature(arc.get_slew_table_map(), transition);
  }
  for (const auto& [transition, table] : arc.get_delay_table_map()) {
    static_cast<void>(table);
    static_cast<void>(transitionIndex(transition));
  }
  for (const auto& [transition, table] : arc.get_slew_table_map()) {
    static_cast<void>(table);
    static_cast<void>(transitionIndex(transition));
  }
  for (const auto& [transition, table] : arc.get_check_table_map()) {
    static_cast<void>(table);
    static_cast<void>(transitionIndex(transition));
  }
  return signature;
}

ChildTimingArcModel exportReferenceChildTimingArc(const ista::TimingArc& arc)
{
  ChildTimingArcModel child;
  child.signature = exportReferenceLibraryArcSignature(arc);
  child.time_unit_scale = arc.get_time_unit_scale();
  child.capacitance_unit_scale = arc.get_cap_unit_scale();
  child.slew_derate = arc.get_slew_derate();
  child.slew_lower_threshold_pct = {arc.get_slew_lower_threshold_pct_rise(), arc.get_slew_lower_threshold_pct_fall()};
  child.slew_upper_threshold_pct = {arc.get_slew_upper_threshold_pct_rise(), arc.get_slew_upper_threshold_pct_fall()};
  child.input_threshold_pct = {arc.get_input_threshold_pct_rise(), arc.get_input_threshold_pct_fall()};
  child.output_threshold_pct = {arc.get_output_threshold_pct_rise(), arc.get_output_threshold_pct_fall()};
  validateChildScalars(child);
  exportTableFamily(arc.get_delay_table_map(), child.delay_tables);
  exportTableFamily(arc.get_slew_table_map(), child.slew_tables);
  exportTableFamily(arc.get_check_table_map(), child.check_tables);
  return child;
}

CellTransferModel exportReferenceCellTransfer(const ista::TimingCellArc& cell_arc)
{
  CellTransferModel transfer;
  transfer.delay = cell_arc.get_delay();
  transfer.delay_min = cell_arc.get_delay_min();
  transfer.delay_max = cell_arc.get_delay_max();
  transfer.is_clock_arc = cell_arc.get_is_clock_arc();
  transfer.is_disabled = cell_arc.get_is_disable_arc();
  transfer.is_clear_or_preset = cell_arc.get_is_clear_preset_arc();
  requireFiniteReference(transfer.delay);
  requireFiniteReference(transfer.delay_min);
  requireFiniteReference(transfer.delay_max);
  transfer.children.reserve(cell_arc.get_timing_arc_list().size());
  std::set<std::int32_t> child_indices;
  for (const auto& child : cell_arc.get_timing_arc_list()) {
    ChildTimingArcModel exported = exportReferenceChildTimingArc(child);
    if (!child_indices.insert(exported.signature.arc_index).second)
      malformedReferenceInput();
    transfer.children.push_back(std::move(exported));
  }
  return transfer;
}

void exportReferenceLiberty(ReferenceBuildState& state)
{
  exportReferencedCells(state);
  refreshInstanceRoles(state);
  exportGraphTransfers(state);
  exportCellPinCapacitances(state);
}

}  // namespace ista::csr::detail
