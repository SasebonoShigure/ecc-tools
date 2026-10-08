// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include "CsrReferenceInputIndependentEncodingInternal.hpp"

namespace ista::csr::test::wire {
namespace {

void appendOptionalNodeId(Writer& writer, const std::optional<NodeId>& value)
{
  writer.appendOptional(value, appendNodeId);
}

void appendOptionalTransition(Writer& writer, const std::optional<Transition>& value)
{
  writer.appendOptional(value, [](Writer& local, Transition present) { local.appendEnum(present); });
}

void appendOptionalPathSource(Writer& writer, const std::optional<PathSource>& value)
{
  writer.appendOptional(value, [](Writer& local, PathSource present) { local.appendEnum(present); });
}

void appendOptionalMatrix(Writer& writer, const OptionalLaneMatrix& matrix)
{
  for (const auto& analysis : matrix)
    for (const auto& transition : analysis)
      appendOptionalDouble(writer, transition);
}

void appendPair(Writer& writer, const std::array<double, kTransitionCount>& pair)
{
  for (double value : pair)
    writer.appendFiniteDouble(value);
}

void appendTimingTable(Writer& writer, const TimingTableModel& table)
{
  writer.appendBool(table.present);
  writer.appendEnum(table.variable1);
  writer.appendEnum(table.variable2);
  writer.appendVector(table.axes, [](Writer& local, const std::vector<double>& axis) {
    local.appendVector(axis, [](Writer& nested, double value) { nested.appendFiniteDouble(value); });
  });
  writer.appendVector(table.values, [](Writer& local, double value) { local.appendFiniteDouble(value); });
}

void appendChild(Writer& writer, const ChildTimingArcModel& child)
{
  appendLibraryArcSignature(writer, child.signature);
  writer.appendFiniteDouble(child.time_unit_scale);
  writer.appendFiniteDouble(child.capacitance_unit_scale);
  writer.appendFiniteDouble(child.slew_derate);
  appendPair(writer, child.slew_lower_threshold_pct);
  appendPair(writer, child.slew_upper_threshold_pct);
  appendPair(writer, child.input_threshold_pct);
  appendPair(writer, child.output_threshold_pct);
  for (const auto& table : child.delay_tables)
    appendTimingTable(writer, table);
  for (const auto& table : child.slew_tables)
    appendTimingTable(writer, table);
  for (const auto& table : child.check_tables)
    appendTimingTable(writer, table);
}

void appendRawNodeReference(Writer& writer, const RawNodeReference& reference)
{
  writer.appendString(reference.full_name);
  appendOptionalNodeId(writer, reference.resolved_node);
}

void appendLibraryPort(Writer& writer, const LibraryPortModel& port)
{
  writer.appendString(port.name);
  writer.appendEnum(port.direction);
  writer.appendBool(port.is_clock);
  writer.appendBool(port.is_output);
  writer.appendFiniteDouble(port.default_capacitance);
  appendOptionalMatrix(writer, port.transition_capacitance);
}

void appendLibraryCellArc(Writer& writer, const LibraryCellArcModel& arc)
{
  writer.appendString(arc.source_port);
  writer.appendString(arc.sink_port);
  appendTransfer(writer, arc.transfer);
}

void appendLibraryCheck(Writer& writer, const LibraryTimingCheckArcModel& check)
{
  writer.appendString(check.clock_port);
  writer.appendString(check.data_port);
  writer.appendEnum(check.kind);
  writer.appendFiniteDouble(check.check_time);
  writer.appendEnum(check.clock_transition);
  writer.appendVector(check.children(), appendChild);
}

void appendParasiticNode(Writer& writer, const ParasiticNodeModel& node)
{
  writer.appendString(node.map_key);
  writer.appendString(node.embedded_name);
  writer.appendFiniteDouble(node.capacitance);
  writer.appendFiniteDouble(node.x);
  writer.appendFiniteDouble(node.y);
}

void appendParasiticResistor(Writer& writer, const ParasiticResistorModel& resistor)
{
  writer.appendString(resistor.source);
  writer.appendString(resistor.sink);
  writer.appendFiniteDouble(resistor.resistance);
}

void appendParasiticNet(Writer& writer, const ParasiticNetModel& net)
{
  writer.appendString(net.map_key);
  writer.appendString(net.embedded_name);
  writer.appendFiniteDouble(net.lumped_capacitance);
  writer.appendVector(net.nodes, appendParasiticNode);
  writer.appendVector(net.resistors, appendParasiticResistor);
}

}  // namespace

void appendTransfer(Writer& writer, const CellTransferModel& transfer)
{
  writer.appendFiniteDouble(transfer.delay);
  writer.appendFiniteDouble(transfer.delay_min);
  writer.appendFiniteDouble(transfer.delay_max);
  writer.appendBool(transfer.is_clock_arc);
  writer.appendBool(transfer.is_disabled);
  writer.appendBool(transfer.is_clear_or_preset);
  writer.appendVector(transfer.children(), appendChild);
}

void appendClock(Writer& writer, const ClockConstraintModel& clock)
{
  writer.appendString(clock.map_key);
  writer.appendString(clock.embedded_clock_name);
  writer.appendVector(clock.ordered_sources, appendRawNodeReference);
  writer.appendFiniteDouble(clock.period);
  appendPair(writer, clock.edges);
  writer.appendBool(clock.propagated);
}

void appendPortConstraint(Writer& writer, const PortConstraintModel& port)
{
  writer.appendString(port.map_key);
  writer.appendString(port.embedded_port_name);
  appendOptionalNodeId(writer, port.resolved_node);
  for (const auto& value : port.input_delay)
    appendOptionalDouble(writer, value);
  appendOptionalDouble(writer, port.input_transition);
  for (const auto& value : port.output_delay)
    appendOptionalDouble(writer, value);
  appendOptionalDouble(writer, port.output_load);
  writer.appendString(port.clock_name);
}

void appendCaseConstraint(Writer& writer, const CaseAnalysisConstraintModel& constraint)
{
  writer.appendString(constraint.raw_name);
  appendOptionalNodeId(writer, constraint.resolved_node);
  writer.appendBool(constraint.value);
}

void appendTimingCheckEndpoint(Writer& writer, const TimingCheckEndpointModel& endpoint)
{
  writer.appendString(endpoint.instance_name);
  writer.appendU64(endpoint.referenced_check_ordinal);
  writer.appendEnum(endpoint.check_kind);
  writer.appendString(endpoint.clock_pin_raw_name);
  writer.appendString(endpoint.data_pin_raw_name);
  writer.appendString(endpoint.clock_pin_full_name);
  writer.appendString(endpoint.data_pin_full_name);
  appendOptionalNodeId(writer, endpoint.resolved_clock_node);
  appendOptionalNodeId(writer, endpoint.resolved_data_node);
}

void appendLibraryCell(Writer& writer, const ReferencedLibraryCellModel& cell)
{
  writer.appendString(cell.cell_name);
  writer.appendBool(cell.present);
  writer.appendString(cell.library_name);
  writer.appendBool(cell.is_sequential);
  writer.appendBool(cell.is_clock_gating);
  writer.appendBool(cell.is_macro);
  writer.appendString(cell.clock_pin_name);
  writer.appendString(cell.data_pin_name);
  writer.appendString(cell.output_pin_name);
  writer.appendFiniteDouble(cell.slew_derate);
  for (double value : cell.slew_lower_threshold_pct) writer.appendFiniteDouble(value);
  for (double value : cell.slew_upper_threshold_pct) writer.appendFiniteDouble(value);
  for (double value : cell.input_threshold_pct) writer.appendFiniteDouble(value);
  writer.appendVector(cell.ports, appendLibraryPort);
  writer.appendVector(cell.cell_arcs, appendLibraryCellArc);
  writer.appendVector(cell.timing_checks, appendLibraryCheck);
}

void appendParasitics(Writer& writer, const ParasiticModel& parasitics)
{
  writer.appendString(parasitics.capacitance_unit);
  writer.appendString(parasitics.resistance_unit);
  writer.appendFiniteDouble(parasitics.input_port_thresholds.slew_derate);
  for (double value : parasitics.input_port_thresholds.lower) writer.appendFiniteDouble(value);
  for (double value : parasitics.input_port_thresholds.upper) writer.appendFiniteDouble(value);
  writer.appendVector(parasitics.nets, appendParasiticNet);
}

void appendStartSeed(Writer& writer, const StartSeedModel& seed)
{
  writer.appendU64(seed.seed_node.value());
  writer.appendU64(seed.path_start_node.value());
  writer.appendEnum(seed.availability);
  appendOptionalPathSource(writer, seed.path_source);
  writer.appendString(seed.clock_tag);
  appendOptionalMatrix(writer, seed.arrival);
  appendOptionalMatrix(writer, seed.slew);
  appendOptionalNodeId(writer, seed.crpr_node);
  appendOptionalTransition(writer, seed.crpr_transition);
}

void appendEndpointRequired(Writer& writer, const EndpointRequiredModel& endpoint)
{
  writer.appendU64(endpoint.node.value());
  writer.appendString(endpoint.clock_name);
  writer.appendFiniteDouble(endpoint.clock_period);
  for (const auto& value : endpoint.output_delay)
    appendOptionalDouble(writer, value);
}

void appendPinCapacitance(Writer& writer, const CellPinCapacitanceModel& capacitance)
{
  writer.appendU64(capacitance.node.value());
  writer.appendFiniteDouble(capacitance.default_capacitance);
  appendOptionalMatrix(writer, capacitance.transition_capacitance);
}

}  // namespace ista::csr::test::wire
