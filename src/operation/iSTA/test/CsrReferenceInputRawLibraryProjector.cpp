// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <array>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputRawProjectionInternal.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingCellPort.hpp"
#include "TimingCheckArc.hpp"
#include "TimingLibrary.hpp"
#include "TimingTable.hpp"

namespace ista::csr::test {
namespace {

struct ByteLess
{
  bool operator()(const std::string& lhs, const std::string& rhs) const
  {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                        [](char left, char right) { return static_cast<unsigned char>(left) < static_cast<unsigned char>(right); });
  }
};

RawDirection portDirection(bool input, bool output)
{
  if (input && output)
    return RawDirection::kInout;
  if (input)
    return RawDirection::kInput;
  if (output)
    return RawDirection::kOutput;
  return RawDirection::kNone;
}

RawTransition transitionToken(ista::TransType transition)
{
  switch (transition) {
    case ista::TransType::kRise:
      return RawTransition::kRise;
    case ista::TransType::kFall:
      return RawTransition::kFall;
    case ista::TransType::kNone:
      return RawTransition::kNone;
  }
  throw std::runtime_error("raw library projector: unknown transition");
}

RawTimingSense senseToken(ista::TimingArcSense sense)
{
  switch (sense) {
    case ista::TimingArcSense::kNone:
      return RawTimingSense::kNonePositiveFallback;
    case ista::TimingArcSense::kPositive:
      return RawTimingSense::kPositiveUnate;
    case ista::TimingArcSense::kNegative:
      return RawTimingSense::kNegativeUnate;
    case ista::TimingArcSense::kNonUnate:
      return RawTimingSense::kNonUnate;
  }
  throw std::runtime_error("raw library projector: unknown timing sense");
}

RawTableVariable variableToken(ista::TimingTableVariableType variable)
{
  switch (variable) {
    case ista::TimingTableVariableType::kNone:
      return RawTableVariable::kNone;
    case ista::TimingTableVariableType::kInputTransition:
      return RawTableVariable::kInputTransition;
    case ista::TimingTableVariableType::kOutputCapacitance:
      return RawTableVariable::kOutputCapacitance;
    case ista::TimingTableVariableType::kConstrainedTransition:
      return RawTableVariable::kConstrainedTransition;
  }
  throw std::runtime_error("raw library projector: unknown table variable");
}

RawTimingCheckKind checkToken(ista::TimingCheckType kind)
{
  switch (kind) {
    case ista::TimingCheckType::kNone:
      return RawTimingCheckKind::kNone;
    case ista::TimingCheckType::kSetup:
      return RawTimingCheckKind::kSetup;
    case ista::TimingCheckType::kHold:
      return RawTimingCheckKind::kHold;
    case ista::TimingCheckType::kRecovery:
      return RawTimingCheckKind::kRecovery;
    case ista::TimingCheckType::kRemoval:
      return RawTimingCheckKind::kRemoval;
    case ista::TimingCheckType::kWidth:
      return RawTimingCheckKind::kWidth;
    case ista::TimingCheckType::kPeriod:
      return RawTimingCheckKind::kPeriod;
  }
  throw std::runtime_error("raw library projector: unknown check kind");
}

std::size_t transitionIndex(ista::TransType transition)
{
  if (transition == ista::TransType::kRise)
    return 0;
  if (transition == ista::TransType::kFall)
    return 1;
  throw std::runtime_error("raw library projector: table transition must be rise or fall");
}

std::size_t analysisIndex(ista::AnalysisType analysis)
{
  if (analysis == ista::AnalysisType::kMax)
    return 0;
  if (analysis == ista::AnalysisType::kMin)
    return 1;
  throw std::runtime_error("raw library projector: capacitance analysis must be max or min");
}

RawTimingTable projectTable(const ista::TimingTable& native)
{
  RawTimingTable table;
  table.present = true;
  table.variable1 = variableToken(native.get_variable_type1());
  table.variable2 = variableToken(native.get_variable_type2());
  if (native.get_axis_list().size() > 2)
    throw std::runtime_error("raw library projector: table has more than two axes");
  if (!native.get_axis_list().empty())
    table.axis1 = native.get_axis_list()[0];
  if (native.get_axis_list().size() == 2)
    table.axis2 = native.get_axis_list()[1];
  table.values = native.get_value_list();
  return table;
}

void projectTables(const std::map<ista::TransType, ista::TimingTable>& native, std::array<RawTimingTable, 2>& projected)
{
  for (const auto& [transition, table] : native)
    projected[transitionIndex(transition)] = projectTable(table);
}

RawLibraryArcChild projectChild(const ista::TimingArc& native)
{
  RawLibraryArcChild child;
  child.library_name = native.get_library_name();
  child.index = native.get_arc_idx();
  child.sense = senseToken(native.get_sense());
  child.trigger_transition = transitionToken(native.get_trigger_trans_type());
  child.check_transition = transitionToken(native.get_check_trans_type());
  child.sdf_condition = native.get_sdf_cond();
  child.time_scale = native.get_time_unit_scale();
  child.capacitance_scale = native.get_cap_unit_scale();
  child.slew_derate = native.get_slew_derate();
  child.slew_lower_thresholds = {native.get_slew_lower_threshold_pct_rise(), native.get_slew_lower_threshold_pct_fall()};
  child.slew_upper_thresholds = {native.get_slew_upper_threshold_pct_rise(), native.get_slew_upper_threshold_pct_fall()};
  child.input_thresholds = {native.get_input_threshold_pct_rise(), native.get_input_threshold_pct_fall()};
  child.output_thresholds = {native.get_output_threshold_pct_rise(), native.get_output_threshold_pct_fall()};
  projectTables(native.get_delay_table_map(), child.delay_tables);
  projectTables(native.get_slew_table_map(), child.slew_tables);
  projectTables(native.get_check_table_map(), child.check_tables);
  return child;
}

RawLibraryPort projectPort(const ista::TimingCellPort& native)
{
  RawLibraryPort port;
  port.name = native.get_port_name();
  port.direction = portDirection(native.get_is_input(), native.get_is_output());
  port.clock = native.get_is_clock();
  port.output = native.get_is_output();
  port.default_capacitance = native.get_capacitance();
  for (const auto& [analysis, transitions] : native.get_trans_capacitance_map())
    for (const auto& [transition, value] : transitions)
      port.transition_capacitance[analysisIndex(analysis)][transitionIndex(transition)] = value;
  return port;
}

RawLibraryCellArc projectCellArc(const ista::TimingCellArc& native)
{
  RawLibraryCellArc arc;
  arc.source_port = native.get_source_port();
  arc.sink_port = native.get_sink_port();
  arc.constant_delay = native.get_delay();
  arc.delay_min = native.get_delay_min();
  arc.delay_max = native.get_delay_max();
  arc.is_clock_arc = native.get_is_clock_arc();
  arc.disabled = native.get_is_disable_arc();
  arc.is_clear_or_preset = native.get_is_clear_preset_arc();
  for (const auto& child : native.get_timing_arc_list())
    arc.ordered_children.push_back(projectChild(child));
  return arc;
}

RawLibraryCheckArc projectCheck(const ista::TimingCheckArc& native)
{
  RawLibraryCheckArc check;
  check.raw_clock_port = native.get_clock_port();
  check.raw_data_port = native.get_data_port();
  check.kind = checkToken(native.get_check_type());
  check.check_time = native.get_check_time();
  check.clock_transition = transitionToken(native.get_clock_trans_type());
  for (const auto& child : native.get_timing_arc_list())
    check.ordered_children.push_back(projectChild(child));
  return check;
}

std::string capUnit(ista::TimingCapacitiveUnit unit)
{
  switch (unit) {
    case ista::TimingCapacitiveUnit::kNone:
      return {};
    case ista::TimingCapacitiveUnit::kPF:
      return "pf";
    case ista::TimingCapacitiveUnit::kFF:
      return "ff";
    case ista::TimingCapacitiveUnit::kF:
      return "f";
  }
  throw std::runtime_error("raw library projector: unknown capacitance unit");
}

std::string timeUnit(ista::TimingTimeUnit unit)
{
  switch (unit) {
    case ista::TimingTimeUnit::kNone:
      return {};
    case ista::TimingTimeUnit::kNS:
      return "ns";
    case ista::TimingTimeUnit::kPS:
      return "ps";
    case ista::TimingTimeUnit::kFS:
      return "fs";
  }
  throw std::runtime_error("raw library projector: unknown time unit");
}

void assignRoles(const ista::TimingCell& cell, RawLibraryCellPayload& payload)
{
  const ista::TimingCheckArc* representative = nullptr;
  for (const auto& check : cell.get_check_arc_list()) {
    if (representative == nullptr || check.get_check_type() == ista::TimingCheckType::kSetup)
      representative = &check;
    if (check.get_check_type() == ista::TimingCheckType::kSetup)
      break;
  }
  if (representative != nullptr) {
    payload.clock_pin_name = representative->get_clock_port();
    payload.data_pin_name = representative->get_data_port();
  }
  for (const auto& arc : cell.get_cell_arc_list())
    if (arc.get_is_clock_arc()) {
      payload.output_pin_name = arc.get_sink_port();
      return;
    }
  for (const auto& [key, port] : cell.get_port_map()) {
    static_cast<void>(key);
    if (port.get_is_output() && !port.get_is_clock()) {
      payload.output_pin_name = port.get_port_name();
      return;
    }
  }
}

RawLibraryCellPayload projectPayload(const ista::TimingLibrary& library, const ista::TimingCell& cell)
{
  RawLibraryCellPayload payload;
  payload.sequential = cell.get_is_sequential();
  payload.gating = cell.get_is_clock_gating();
  payload.macro = cell.get_is_macro();
  payload.capacitance_unit = capUnit(library.get_cap_unit());
  payload.time_unit = timeUnit(library.get_time_unit());
  payload.slew_derate = cell.get_slew_derate_from_library();
  payload.slew_lower_thresholds = {cell.get_slew_lower_threshold_pct_rise(), cell.get_slew_lower_threshold_pct_fall()};
  payload.slew_upper_thresholds = {cell.get_slew_upper_threshold_pct_rise(), cell.get_slew_upper_threshold_pct_fall()};
  payload.input_thresholds = {cell.get_input_threshold_pct_rise(), cell.get_input_threshold_pct_fall()};
  payload.output_thresholds = {cell.get_output_threshold_pct_rise(), cell.get_output_threshold_pct_fall()};
  assignRoles(cell, payload);
  for (const auto& [key, port] : cell.get_port_map()) {
    static_cast<void>(key);
    payload.ports.push_back(projectPort(port));
  }
  for (const auto& arc : cell.get_cell_arc_list())
    payload.cell_arcs.push_back(projectCellArc(arc));
  for (const auto& check : cell.get_check_arc_list())
    payload.check_arcs.push_back(projectCheck(check));
  return payload;
}

}  // namespace

void projectCsrReferenceInputRawLibrary(const ista::Database& database, CsrReferenceInputRawProjection& projection)
{
  std::set<std::string, ByteLess> referenced;
  for (const auto& [name, instance] : database.get_instance_map()) {
    static_cast<void>(name);
    referenced.insert(instance.get_cell_name());
  }
  const auto& library = database.get_timing_library();
  projection.referenced_library_cells.reserve(referenced.size());
  for (const auto& name : referenced) {
    RawReferencedLibraryCell raw;
    raw.cell_name = name;
    const auto found = library.get_cell_map().find(name);
    if (found != library.get_cell_map().end()) {
      raw.library_name = found->second.get_library_name();
      raw.payload = projectPayload(library, found->second);
    }
    projection.referenced_library_cells.push_back(std::move(raw));
  }
}

namespace raw_wire {
namespace {

void appendTable(Writer& writer, const RawTimingTable& table)
{
  writer.boolean(table.present);
  if (!table.present)
    return;
  if (!table.variable1 || !table.variable2)
    throw std::invalid_argument("present raw timing table lacks variable tokens");
  writer.enumeration(*table.variable1);
  writer.enumeration(*table.variable2);
  writer.vector(table.axis1, [](Writer& local, double value) { local.finite(value); });
  writer.vector(table.axis2, [](Writer& local, double value) { local.finite(value); });
  writer.vector(table.values, [](Writer& local, double value) { local.finite(value); });
}

void appendChild(Writer& writer, const RawLibraryArcChild& child)
{
  writer.string(child.library_name);
  writer.i32(child.index);
  writer.enumeration(child.sense);
  writer.enumeration(child.trigger_transition);
  writer.enumeration(child.check_transition);
  writer.string(child.sdf_condition);
  writer.finite(child.time_scale);
  writer.finite(child.capacitance_scale);
  writer.finite(child.slew_derate);
  for (const auto& pair : {child.slew_lower_thresholds, child.slew_upper_thresholds, child.input_thresholds, child.output_thresholds})
    for (double value : pair)
      writer.finite(value);
  for (const auto& table : child.delay_tables)
    appendTable(writer, table);
  for (const auto& table : child.slew_tables)
    appendTable(writer, table);
  for (const auto& table : child.check_tables)
    appendTable(writer, table);
}

void appendLibraryPort(Writer& writer, const RawLibraryPort& port)
{
  writer.string(port.name);
  writer.enumeration(port.direction);
  writer.boolean(port.clock);
  writer.boolean(port.output);
  writer.finite(port.default_capacitance);
  for (const auto& row : port.transition_capacitance)
    for (const auto& value : row)
      optionalDouble(writer, value);
}

void appendCellArc(Writer& writer, const RawLibraryCellArc& arc)
{
  writer.string(arc.source_port);
  writer.string(arc.sink_port);
  writer.finite(arc.constant_delay);
  writer.finite(arc.delay_min);
  writer.finite(arc.delay_max);
  writer.boolean(arc.is_clock_arc);
  writer.boolean(arc.disabled);
  writer.boolean(arc.is_clear_or_preset);
  writer.vector(arc.ordered_children, appendChild);
}

void appendLibraryCheck(Writer& writer, const RawLibraryCheckArc& check)
{
  writer.string(check.raw_clock_port);
  writer.string(check.raw_data_port);
  writer.enumeration(check.kind);
  writer.finite(check.check_time);
  writer.enumeration(check.clock_transition);
  writer.vector(check.ordered_children, appendChild);
}

void appendPayload(Writer& writer, const RawLibraryCellPayload& payload)
{
  writer.boolean(payload.sequential);
  writer.boolean(payload.gating);
  writer.boolean(payload.macro);
  writer.string(payload.clock_pin_name);
  writer.string(payload.data_pin_name);
  writer.string(payload.output_pin_name);
  writer.string(payload.capacitance_unit);
  writer.string(payload.time_unit);
  writer.finite(payload.slew_derate);
  for (const auto& pair : {payload.slew_lower_thresholds, payload.slew_upper_thresholds, payload.input_thresholds, payload.output_thresholds})
    for (double value : pair)
      writer.finite(value);
  writer.vector(payload.ports, appendLibraryPort);
  writer.vector(payload.cell_arcs, appendCellArc);
  writer.vector(payload.check_arcs, appendLibraryCheck);
}

}  // namespace

void appendLibraryCell(Writer& writer, const RawReferencedLibraryCell& cell)
{
  writer.string(cell.library_name);
  writer.string(cell.cell_name);
  writer.optional(cell.payload, appendPayload);
}

}  // namespace raw_wire
}  // namespace ista::csr::test
