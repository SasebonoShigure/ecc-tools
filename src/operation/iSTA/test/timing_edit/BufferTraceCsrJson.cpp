// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferTraceCsrJson.hpp"

#include <cmath>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "TimingSemanticKey.hpp"

namespace ista::buffer_trace::test {
namespace {

using Json = nlohmann::json;

Json number(double value)
{
  if (std::isnan(value)) {
    throw std::runtime_error("buffer trace receipt contains NaN");
  }
  if (std::isinf(value)) {
    return {{"kind", std::signbit(value) ? "neg_inf" : "pos_inf"}};
  }
  const bool negative_zero = value == 0.0 && std::signbit(value);
  return {{"kind", "finite"},
          {"value", value == 0.0 ? 0.0 : value},
          {"negative_zero", negative_zero}};
}

template <typename Range, typename Convert>
Json arrayOf(const Range& values, Convert convert)
{
  Json result = Json::array();
  for (const auto& value : values) {
    result.push_back(convert(value));
  }
  return result;
}

template <typename Range>
Json scalarArray(const Range& values)
{
  return arrayOf(values, [](const auto& value) { return Json(value); });
}

template <typename Id>
Json ids(const std::vector<Id>& values)
{
  return arrayOf(values, [](Id value) { return Json(value.value()); });
}

template <typename T, typename Convert>
Json optionalValue(const std::optional<T>& value, Convert convert)
{
  return value ? convert(*value) : Json(nullptr);
}

Json nodeKey(const csr::NodeKey& value)
{
  return {{"kind", csr::token(value.kind)},
          {"full_name", value.full_name},
          {"instance_name", value.instance_name},
          {"pin_name", value.pin_name}};
}

Json tableSignature(const csr::TimingTableSignature& value)
{
  return {{"present", value.present},
          {"variable1", csr::token(value.variable1)},
          {"variable2", csr::token(value.variable2)}};
}

Json librarySignature(const csr::LibraryArcSignature& value)
{
  return {{"library_name", value.library_name},
          {"arc_index", value.arc_index},
          {"sense", csr::token(value.sense)},
          {"trigger_transition", csr::token(value.trigger_transition)},
          {"check_transition", csr::token(value.check_transition)},
          {"sdf_condition", value.sdf_condition},
          {"delay_tables", arrayOf(value.delay_tables, tableSignature)},
          {"slew_tables", arrayOf(value.slew_tables, tableSignature)}};
}

Json arcKey(const csr::ArcKey& value)
{
  return {{"source", nodeKey(value.base.source)},
          {"sink", nodeKey(value.base.sink)},
          {"kind", csr::token(value.base.kind)},
          {"owner_name", value.base.owner_name},
          {"library_source_port", value.base.library_source_port},
          {"library_sink_port", value.base.library_sink_port},
          {"library_arcs", arrayOf(value.base.library_arcs, librarySignature)},
          {"parallel_ordinal", value.parallel_ordinal}};
}

Json identity(const csr::CsrSnapshotIdentity& value)
{
  return {{"fingerprint_schema_version", value.fingerprint_schema_version},
          {"session_token", value.session_token},
          {"graph_generation", value.graph_generation},
          {"topology_fingerprint", value.topology_fingerprint},
          {"clock_constraint_fingerprint", value.clock_constraint_fingerprint},
          {"library_fingerprint", value.library_fingerprint},
          {"corner_fingerprint", value.corner_fingerprint},
          {"parasitic_fingerprint", value.parasitic_fingerprint},
          {"timing_input_fingerprint", value.timing_input_fingerprint}};
}

Json laneMatrix(const csr::OptionalLaneMatrix& value)
{
  return arrayOf(value, [](const auto& analysis) {
    return arrayOf(analysis, [](const auto& lane) {
      return optionalValue(lane, [](double item) { return number(item); });
    });
  });
}

Json loadMatrix(const csr::ArcOutputLoadMatrix& value)
{
  return arrayOf(value, [](const auto& analysis) {
    return arrayOf(analysis, [](double lane) { return number(lane); });
  });
}

Json timingTable(const csr::TimingTableModel& value)
{
  return {{"present", value.present},
          {"variable1", csr::token(value.variable1)},
          {"variable2", csr::token(value.variable2)},
          {"axes", arrayOf(value.axes, [](const auto& axis) {
             return arrayOf(axis, [](double item) { return number(item); });
           })},
          {"values", arrayOf(value.values, [](double item) { return number(item); })}};
}

Json childArc(const csr::ChildTimingArcModel& value)
{
  const auto numbers = [](const auto& values) {
    return arrayOf(values, [](double item) { return number(item); });
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

Json nodeRecord(const csr::NodeRecord& value)
{
  return {{"id", value.id.value()},
          {"key", nodeKey(value.key)},
          {"direction", csr::token(value.direction)},
          {"instance_cell", value.instance_cell},
          {"net_name", value.net_name},
          {"is_port", value.is_port},
          {"is_startpoint", value.is_startpoint},
          {"is_endpoint", value.is_endpoint},
          {"is_sequential", value.is_sequential},
          {"is_clock_pin", value.is_clock_pin},
          {"is_data_pin", value.is_data_pin},
          {"is_output_pin", value.is_output_pin}};
}

Json instanceRecord(const csr::InstanceRecord& value)
{
  return {{"instance_name", value.instance_name},
          {"cell_name", value.cell_name},
          {"ordered_pins", ids(value.ordered_pins)},
          {"is_sequential", value.is_sequential},
          {"has_timing_checks", value.has_timing_checks},
          {"is_clock_gating", value.is_clock_gating},
          {"is_macro", value.is_macro}};
}

Json arcRecord(const csr::ArcRecord& value)
{
  return {{"id", value.id.value()},
          {"key", arcKey(value.key)},
          {"source", value.source.value()},
          {"sink", value.sink.value()},
          {"kind", csr::token(value.kind)},
          {"native_traversal_rank", value.native_traversal_rank},
          {"is_clock_arc", value.is_clock_arc},
          {"is_disabled", value.is_disabled},
          {"is_loop_disabled", value.is_loop_disabled},
          {"eligibility", {{"levelization", value.eligibility.levelization},
                           {"data_arrival", value.eligibility.data_arrival},
                           {"data_slew", value.eligibility.data_slew},
                           {"clock", value.eligibility.clock}}},
          {"transfer", transfer(value.transfer)},
          {"output_load", optionalValue(value.output_load, loadMatrix)}};
}

Json rawNode(const csr::RawNodeReference& value)
{
  return {{"full_name", value.full_name},
          {"resolved_node", optionalValue(value.resolved_node, [](csr::NodeId id) {
             return Json(id.value());
           })}};
}

Json clock(const csr::ClockConstraintModel& value)
{
  return {{"map_key", value.map_key},
          {"embedded_clock_name", value.embedded_clock_name},
          {"ordered_sources", arrayOf(value.ordered_sources, rawNode)},
          {"period", number(value.period)},
          {"edges", arrayOf(value.edges, [](double item) { return number(item); })},
          {"propagated", value.propagated}};
}

Json portConstraint(const csr::PortConstraintModel& value)
{
  const auto optional_number = [](const std::optional<double>& item) {
    return optionalValue(item, [](double present) { return number(present); });
  };
  return {{"map_key", value.map_key},
          {"embedded_port_name", value.embedded_port_name},
          {"resolved_node", optionalValue(value.resolved_node, [](csr::NodeId id) {
             return Json(id.value());
           })},
          {"input_delay", arrayOf(value.input_delay, optional_number)},
          {"input_transition", optional_number(value.input_transition)},
          {"output_delay", arrayOf(value.output_delay, optional_number)},
          {"output_load", optional_number(value.output_load)},
          {"clock_name", value.clock_name}};
}

Json timingCheck(const csr::TimingCheckEndpointModel& value)
{
  const auto optional_id = [](const std::optional<csr::NodeId>& item) {
    return optionalValue(item, [](csr::NodeId id) { return Json(id.value()); });
  };
  return {{"instance_name", value.instance_name},
          {"referenced_check_ordinal", value.referenced_check_ordinal},
          {"check_kind", csr::token(value.check_kind)},
          {"clock_pin_raw_name", value.clock_pin_raw_name},
          {"data_pin_raw_name", value.data_pin_raw_name},
          {"clock_pin_full_name", value.clock_pin_full_name},
          {"data_pin_full_name", value.data_pin_full_name},
          {"resolved_clock_node", optional_id(value.resolved_clock_node)},
          {"resolved_data_node", optional_id(value.resolved_data_node)}};
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
          {"ports", arrayOf(value.ports, libraryPort)},
          {"cell_arcs", arrayOf(value.cell_arcs, cell_arc)},
          {"timing_checks", arrayOf(value.timing_checks, check)}};
}

Json parasitics(const csr::ParasiticModel& value)
{
  const auto node = [](const csr::ParasiticNodeModel& item) {
    return Json{{"map_key", item.map_key},
                {"embedded_name", item.embedded_name},
                {"capacitance", number(item.capacitance)},
                {"x", number(item.x)},
                {"y", number(item.y)}};
  };
  const auto resistor = [](const csr::ParasiticResistorModel& item) {
    return Json{{"source", item.source},
                {"sink", item.sink},
                {"resistance", number(item.resistance)}};
  };
  const auto net = [&](const csr::ParasiticNetModel& item) {
    return Json{{"map_key", item.map_key},
                {"embedded_name", item.embedded_name},
                {"lumped_capacitance", number(item.lumped_capacitance)},
                {"nodes", arrayOf(item.nodes, node)},
                {"resistors", arrayOf(item.resistors, resistor)}};
  };
  return {{"capacitance_unit", value.capacitance_unit},
          {"resistance_unit", value.resistance_unit},
          {"nets", arrayOf(value.nets, net)}};
}

Json startSeed(const csr::StartSeedModel& value)
{
  return {{"seed_node", value.seed_node.value()},
          {"path_start_node", value.path_start_node.value()},
          {"availability", csr::token(value.availability)},
          {"path_source", optionalValue(value.path_source, [](csr::PathSource item) {
             return Json(csr::token(item));
           })},
          {"clock_tag", value.clock_tag},
          {"arrival", laneMatrix(value.arrival)},
          {"slew", laneMatrix(value.slew)},
          {"crpr_node", optionalValue(value.crpr_node, [](csr::NodeId id) {
             return Json(id.value());
           })},
          {"crpr_transition", optionalValue(value.crpr_transition, [](csr::Transition item) {
             return Json(csr::token(item));
           })}};
}

template <typename T, typename Convert>
Json optionalSoa(const csr::OptionalSoa<T>& value, Convert convert)
{
  return {{"presence", scalarArray(value.presence)},
          {"values", arrayOf(value.values, convert)}};
}

Json clockResult(const csr::ClockResultSoa& value)
{
  return {{"arrivals", optionalSoa(value.arrivals, number)},
          {"slews", optionalSoa(value.slews, number)},
          {"predecessor_nodes", optionalSoa(value.predecessor_nodes,
                                               [](csr::NodeId id) { return Json(id.value()); })},
          {"predecessor_arc_delays", optionalSoa(value.predecessor_arc_delays, number)},
          {"predecessor_transitions", optionalSoa(value.predecessor_transitions,
                                                     [](csr::Transition item) {
                                                       return Json(csr::token(item));
                                                     })}};
}

Json nodeResult(const csr::NodeResultSoa& value)
{
  return {{"is_clock_point", optionalSoa(value.is_clock_point,
                                           [](std::uint8_t item) { return Json(item); })},
          {"arrivals", optionalSoa(value.arrivals, number)},
          {"requireds", optionalSoa(value.requireds, number)},
          {"slacks", optionalSoa(value.slacks, number)},
          {"launches", optionalSoa(value.launches, number)},
          {"clock_names", optionalSoa(value.clock_names,
                                        [](const std::string& item) { return Json(item); })},
          {"predecessor_nodes", optionalSoa(value.predecessor_nodes,
                                               [](csr::NodeId id) { return Json(id.value()); })},
          {"predecessor_arcs", optionalSoa(value.predecessor_arcs,
                                              [](csr::ArcId id) { return Json(id.value()); })}};
}

Json pathResult(const csr::PathResultSoa& value)
{
  return {{"row_offsets", scalarArray(value.row_offsets)},
          {"clock_tags", optionalSoa(value.clock_tags,
                                      [](csr::ClockTagId id) { return Json(id.value()); })},
          {"arrivals", optionalSoa(value.arrivals, number)},
          {"slews", optionalSoa(value.slews, number)},
          {"launches", optionalSoa(value.launches, number)},
          {"start_nodes", optionalSoa(value.start_nodes,
                                       [](csr::NodeId id) { return Json(id.value()); })},
          {"predecessor_nodes", optionalSoa(value.predecessor_nodes,
                                             [](csr::NodeId id) { return Json(id.value()); })},
          {"predecessor_arcs", optionalSoa(value.predecessor_arcs,
                                            [](csr::ArcId id) { return Json(id.value()); })},
          {"predecessor_arc_delays", optionalSoa(value.predecessor_arc_delays, number)},
          {"predecessor_transitions", optionalSoa(value.predecessor_transitions,
                                                    [](csr::Transition item) {
                                                      return Json(csr::token(item));
                                                    })},
          {"crpr_nodes", optionalSoa(value.crpr_nodes,
                                     [](csr::NodeId id) { return Json(id.value()); })},
          {"crpr_transitions", optionalSoa(value.crpr_transitions,
                                            [](csr::Transition item) {
                                              return Json(csr::token(item));
                                            })}};
}

Json arcResult(const csr::ArcResultSoa& value)
{
  return {{"delays", optionalSoa(value.delays, number)},
          {"min_delays", optionalSoa(value.min_delays, number)},
          {"max_delays", optionalSoa(value.max_delays, number)},
          {"transition_delays", optionalSoa(value.transition_delays, number)},
          {"input_output_delays", optionalSoa(value.input_output_delays, number)},
          {"graph_delays", optionalSoa(value.graph_delays, number)},
          {"transition_types", optionalSoa(value.transition_types,
                                             [](csr::Transition item) {
                                               return Json(csr::token(item));
                                             })},
          {"timing_arc_row_offsets", scalarArray(value.timing_arc_row_offsets)},
          {"timing_arc_indices", scalarArray(value.timing_arc_indices)},
          {"timing_arc_delays", optionalSoa(value.timing_arc_delays, number)},
          {"timing_arc_fallbacks", optionalSoa(value.timing_arc_fallbacks,
                                                [](std::uint8_t item) { return Json(item); })}};
}

}  // namespace

Json csrReferenceInputToJson(const csr::CsrReferenceInput& input)
{
  const auto node_lookup = [](const csr::NodeIdLookupEntry& value) {
    return Json{{"key", nodeKey(value.key)}, {"id", value.id.value()}};
  };
  const auto arc_lookup = [](const csr::ArcIdLookupEntry& value) {
    return Json{{"key", arcKey(value.key)}, {"id", value.id.value()}};
  };
  const auto optional_text = [](const std::optional<std::string>& value) {
    return optionalValue(value, [](const std::string& item) { return Json(item); });
  };
  const auto case_analysis = [](const csr::CaseAnalysisConstraintModel& value) {
    return Json{{"raw_name", value.raw_name},
                {"resolved_node", optionalValue(value.resolved_node,
                                                 [](csr::NodeId id) { return Json(id.value()); })},
                {"value", value.value}};
  };
  const auto endpoint = [](const csr::EndpointRequiredModel& value) {
    return Json{{"node", value.node.value()},
                {"clock_name", value.clock_name},
                {"clock_period", number(value.clock_period)},
                {"output_delay", arrayOf(value.output_delay, [](const auto& item) {
                   return optionalValue(item, [](double present) { return number(present); });
                 })}};
  };
  const auto pin_cap = [](const csr::CellPinCapacitanceModel& value) {
    return Json{{"node", value.node.value()},
                {"default_capacitance", number(value.default_capacitance)},
                {"transition_capacitance", laneMatrix(value.transition_capacitance)}};
  };
  return {{"schema_version", input.schema_version},
          {"source_identity", identity(input.source_identity)},
          {"analysis_context", {{"liberty_corner", input.analysis_context.liberty_corner},
                                  {"temperature", input.analysis_context.temperature},
                                  {"rc_corner", input.analysis_context.rc_corner}}},
          {"analysis_order", arrayOf(input.analysis_order,
                                      [](csr::AnalysisMode item) { return Json(csr::token(item)); })},
          {"path_source_order", arrayOf(input.path_source_order,
                                         [](csr::PathSource item) { return Json(csr::token(item)); })},
          {"transition_order", arrayOf(input.transition_order,
                                        [](csr::Transition item) { return Json(csr::token(item)); })},
          {"nodes", arrayOf(input.nodes, nodeRecord)},
          {"node_ids_by_key", arrayOf(input.node_ids_by_key, node_lookup)},
          {"instances", arrayOf(input.instances, instanceRecord)},
          {"arcs", arrayOf(input.arcs, arcRecord)},
          {"arc_ids_by_key", arrayOf(input.arc_ids_by_key, arc_lookup)},
          {"out_row_offsets", scalarArray(input.out_row_offsets)},
          {"out_arc_ids", ids(input.out_arc_ids)},
          {"in_row_offsets", scalarArray(input.in_row_offsets)},
          {"in_arc_ids", ids(input.in_arc_ids)},
          {"ordered_root_ids", ids(input.ordered_root_ids)},
          {"ordered_start_ids", ids(input.ordered_start_ids)},
          {"ordered_end_ids", ids(input.ordered_end_ids)},
          {"clock_tags", scalarArray(input.clock_tags)},
          {"clocks", arrayOf(input.clocks, clock)},
          {"clock_owner_by_node", arrayOf(input.clock_owner_by_node, optional_text)},
          {"port_constraints", arrayOf(input.port_constraints, portConstraint)},
          {"case_analysis_constraints", arrayOf(input.case_analysis_constraints, case_analysis)},
          {"timing_check_endpoints", arrayOf(input.timing_check_endpoints, timingCheck)},
          {"referenced_library_cells", arrayOf(input.referenced_library_cells, libraryCell)},
          {"parasitics", parasitics(input.parasitics)},
          {"start_seeds", arrayOf(input.start_seeds, startSeed)},
          {"endpoint_required", arrayOf(input.endpoint_required, endpoint)},
          {"cell_pin_capacitances", arrayOf(input.cell_pin_capacitances, pin_cap)},
          {"unsupported_reasons", arrayOf(input.unsupported_reasons,
                                            [](csr::CsrReferenceUnsupportedReason item) {
                                              return Json(csr::token(item));
                                            })}};
}

Json csrTimingValuesToJson(const csr::CsrTimingValues& result)
{
  return {{"level_offsets", scalarArray(result.level_offsets)},
          {"level_nodes", ids(result.level_nodes)},
          {"node_to_level", scalarArray(result.node_to_level)},
          {"nodes", nodeResult(result.nodes)},
          {"effective_clock", clockResult(result.effective_clock)},
          {"physical_clock", clockResult(result.physical_clock)},
          {"data_slews", optionalSoa(result.data_slews, number)},
          {"paths", pathResult(result.paths)},
          {"arcs", arcResult(result.arcs)}};
}

Json csrTimingResultToJson(const csr::CsrTimingResult& result)
{
  auto value=csrTimingValuesToJson(result);
  value["schema_version"]=result.schema_version;
  value["source_identity"]=identity(result.source_identity);
  return value;
}

}  // namespace ista::buffer_trace::test
