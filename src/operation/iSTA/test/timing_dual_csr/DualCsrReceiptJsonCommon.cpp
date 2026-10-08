// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrReceiptJsonCommon.hpp"

#include <cmath>
#include <stdexcept>

#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr::test::json_detail {

Json number(double value)
{
  if (std::isnan(value))
    throw std::runtime_error("dual CSR receipt contains NaN");
  if (std::isinf(value))
    return {{"kind", std::signbit(value) ? "neg_inf" : "pos_inf"}};
  return {{"kind", "finite"},
          {"value", value == 0.0 ? 0.0 : value},
          {"negative_zero", value == 0.0 && std::signbit(value)}};
}

Json laneMatrix(const csr::OptionalLaneMatrix& value)
{
  return arrayOf(value, [](const auto& analysis) {
    return arrayOf(analysis, [](const auto& lane) {
      return optionalValue(lane,
                           [](double present) { return number(present); });
    });
  });
}

Json loadMatrix(const csr::ArcOutputLoadMatrix& value)
{
  return arrayOf(value, [](const auto& analysis) {
    return arrayOf(analysis,
                   [](double lane) { return number(lane); });
  });
}

Json timingTable(const csr::TimingTableModel& value)
{
  return {{"present", value.present},
          {"variable1", csr::token(value.variable1)},
          {"variable2", csr::token(value.variable2)},
          {"axes", arrayOf(value.axes, [](const auto& axis) {
             return arrayOf(axis,
                            [](double item) { return number(item); });
           })},
          {"values", arrayOf(value.values,
                             [](double item) { return number(item); })}};
}

Json childArc(const csr::ChildTimingArcModel& value)
{
  const auto numbers = [](const auto& values) {
    return arrayOf(values,
                   [](double item) { return number(item); });
  };
  return {{"signature", librarySignature(value.signature)},
          {"time_unit_scale", number(value.time_unit_scale)},
          {"capacitance_unit_scale", number(value.capacitance_unit_scale)},
          {"slew_derate", number(value.slew_derate)},
          {"slew_lower_threshold_pct", numbers(value.slew_lower_threshold_pct)},
          {"slew_upper_threshold_pct", numbers(value.slew_upper_threshold_pct)},
          {"input_threshold_pct", numbers(value.input_threshold_pct)},
          {"output_threshold_pct", numbers(value.output_threshold_pct)},
          {"delay_tables", arrayOf(value.delay_tables, timingTable)},
          {"slew_tables", arrayOf(value.slew_tables, timingTable)},
          {"check_tables", arrayOf(value.check_tables, timingTable)}};
}

Json transfer(const csr::CellTransferModel& value)
{
  return {{"delay", number(value.delay)},
          {"delay_min", number(value.delay_min)},
          {"delay_max", number(value.delay_max)},
          {"is_clock_arc", value.is_clock_arc},
          {"is_disabled", value.is_disabled},
          {"is_clear_or_preset", value.is_clear_or_preset},
          {"children", arrayOf(value.children(), childArc)}};
}

Json libraryPort(const csr::LibraryPortModel& value)
{
  return {{"name", value.name},
          {"direction", csr::token(value.direction)},
          {"is_clock", value.is_clock},
          {"is_output", value.is_output},
          {"default_capacitance", number(value.default_capacitance)},
          {"transition_capacitance", laneMatrix(value.transition_capacitance)}};
}

Json libraryCell(const csr::ReferencedLibraryCellModel& value)
{
  const auto cell_arc = [](const csr::LibraryCellArcModel& arc) {
    return Json{{"source_port", arc.source_port},
                {"sink_port", arc.sink_port},
                {"transfer", transfer(arc.transfer)}};
  };
  const auto check = [](const csr::LibraryTimingCheckArcModel& arc) {
    return Json{{"clock_port", arc.clock_port},
                {"data_port", arc.data_port},
                {"kind", csr::token(arc.kind)},
                {"check_time", number(arc.check_time)},
                {"clock_transition", csr::token(arc.clock_transition)},
                {"children", arrayOf(arc.children(), childArc)}};
  };
  return {{"cell_name", value.cell_name},
          {"present", value.present},
          {"library_name", value.library_name},
          {"is_sequential", value.is_sequential},
          {"is_clock_gating", value.is_clock_gating},
          {"is_macro", value.is_macro},
          {"clock_pin_name", value.clock_pin_name},
          {"data_pin_name", value.data_pin_name},
          {"output_pin_name", value.output_pin_name},
          {"slew_derate", number(value.slew_derate)},
          {"slew_lower_threshold_pct", arrayOf(value.slew_lower_threshold_pct, number)},
          {"slew_upper_threshold_pct", arrayOf(value.slew_upper_threshold_pct, number)},
          {"input_threshold_pct", arrayOf(value.input_threshold_pct, number)},
          {"ports", arrayOf(value.ports, libraryPort)},
          {"cell_arcs", arrayOf(value.cell_arcs, cell_arc)},
          {"timing_checks", arrayOf(value.timing_checks, check)}};
}

Json parasiticNet(const csr::ParasiticNetModel& value)
{
  const auto node = [](const csr::ParasiticNodeModel& item) {
    return Json{{"map_key", item.map_key}, {"embedded_name", item.embedded_name},
                {"capacitance", number(item.capacitance)}, {"x", number(item.x)}, {"y", number(item.y)}};
  };
  const auto resistor = [](const csr::ParasiticResistorModel& item) {
    return Json{{"source", item.source}, {"sink", item.sink}, {"resistance", number(item.resistance)}};
  };
  return {{"map_key", value.map_key}, {"embedded_name", value.embedded_name},
          {"lumped_capacitance", number(value.lumped_capacitance)},
          {"nodes", arrayOf(value.nodes, node)}, {"resistors", arrayOf(value.resistors, resistor)}};
}

Json parasitics(const csr::ParasiticModel& value)
{
  return {{"capacitance_unit", value.capacitance_unit},
          {"resistance_unit", value.resistance_unit},
          {"input_port_thresholds", {{"slew_derate", number(value.input_port_thresholds.slew_derate)},
              {"lower", arrayOf(value.input_port_thresholds.lower, number)},
              {"upper", arrayOf(value.input_port_thresholds.upper, number)}}},
          {"nets", arrayOf(value.nets, parasiticNet)}};
}

}  // namespace ista::timing_dual_csr::test::json_detail
