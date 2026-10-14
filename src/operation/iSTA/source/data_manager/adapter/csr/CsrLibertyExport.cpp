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

#include "CanonicalTimingRecordProjectorInternal.hpp"
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

void exportReferencedCells(ReferenceBuildState& state)
{
  std::set<std::string, decltype(&referenceByteStringLess)> names(&referenceByteStringLess);
  for (const auto& instance : state.input.instances)
    names.insert(instance.cell_name);

  state.input.referenced_library_cells.reserve(names.size());
  for (const auto& cell_name : names) {
    ReferencedLibraryCellModel cell
        = state.models.cell(cell_name);

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
    applyCanonicalInstanceLibraryRoles(cell, instance);

    for (NodeId node_id : instance.ordered_pins) {
      NodeRecord& node = state.input.nodes[node_id.value()];
      applyCanonicalNodeLibraryRoles(cell, node);
    }
  }
}

void exportGraphTransfers(ReferenceBuildState& state)
{
  const auto& native_arcs = state.database.get_arc_list();
  for (auto& arc : state.input.arcs) {
    // The graph producer bound this rank before entering the controlled reference export.
    const ista::Arc& native_arc = native_arcs[static_cast<std::size_t>(arc.native_traversal_rank)];
    projectCanonicalArcTransfer(state.database, state.models, arc, native_arc);

    if (arc.kind == ArcKind::kNet)
      continue;

    if (arc.transfer.is_clock_arc != arc.is_clock_arc || arc.transfer.is_disabled != arc.is_disabled
        || arc.transfer.children().size() != arc.key.base.library_arcs.size())
      malformedReferenceInput();
    for (std::size_t child = 0; child < arc.transfer.children().size(); ++child)
      if (!(arc.transfer.children()[child].signature == arc.key.base.library_arcs[child]))
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
    CellPinCapacitanceModel capacitance
        = projectCanonicalCellPinCapacitance(
            state.database, node.id, node.key);
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
  std::vector<ChildTimingArcModel> children;
  children.reserve(cell_arc.get_timing_arc_list().size());
  std::set<std::int32_t> child_indices;
  for (const auto& child : cell_arc.get_timing_arc_list()) {
    ChildTimingArcModel exported = exportReferenceChildTimingArc(child);
    if (!child_indices.insert(exported.signature.arc_index).second)
      malformedReferenceInput();
    children.push_back(std::move(exported));
  }
  transfer.setChildren(children);
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
