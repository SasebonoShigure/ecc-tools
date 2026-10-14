// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrFunctionalReceiptJson.hpp"

#include "DualCsrReceiptJsonCommon.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr::test::detail {
namespace {

using namespace json_detail;

Json node(const DualCsrProjectedNode& value)
{
  return {{"key", nodeKey(value.key)},
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

Json arc(const DualCsrProjectedArc& value)
{
  return {{"key", arcKey(value.key)},
          {"source", nodeKey(value.source)},
          {"sink", nodeKey(value.sink)},
          {"kind", csr::token(value.kind)},
          {"is_clock_arc", value.is_clock_arc},
          {"is_disabled", value.is_disabled},
          {"is_loop_disabled", value.is_loop_disabled},
          {"eligibility", {{"levelization", value.eligibility.levelization},
                            {"data_arrival", value.eligibility.data_arrival},
                            {"data_slew", value.eligibility.data_slew},
                            {"clock", value.eligibility.clock}}},
          {"transfer", transfer(value.transfer)},
          {"native_cell_arc_ordinal", optionalValue(value.native_cell_arc_ordinal, [](auto ordinal) { return Json(ordinal); })},
          {"output_load", optionalValue(
                              value.output_load,
                              [](const auto& present) {
                                return loadMatrix(present);
                              })}};
}

Json instance(const DualCsrProjectedInstance& value)
{
  return {{"instance_name", value.instance_name},
          {"cell_name", value.cell_name},
          {"ordered_pins", arrayOf(value.ordered_pins, nodeKey)},
          {"is_sequential", value.is_sequential},
          {"has_timing_checks", value.has_timing_checks},
          {"is_clock_gating", value.is_clock_gating},
          {"is_macro", value.is_macro}};
}

Json rawNode(const DualCsrProjectedRawNodeReference& value)
{
  return {{"full_name", value.full_name},
          {"resolved_node", optionalNodeKey(value.resolved_node)}};
}

Json clock(const DualCsrProjectedClockConstraint& value)
{
  return {{"map_key", value.map_key},
          {"embedded_clock_name", value.embedded_clock_name},
          {"ordered_sources", arrayOf(value.ordered_sources, rawNode)},
          {"period", number(value.period)},
          {"edges", arrayOf(value.edges,
                            [](double item) { return number(item); })},
          {"propagated", value.propagated}};
}

Json clockOwner(const DualCsrProjectedClockOwner& value)
{
  return {{"node", nodeKey(value.node)},
          {"clock_name", optionalText(value.clock_name)}};
}

Json portConstraint(const DualCsrProjectedPortConstraint& value)
{
  return {{"map_key", value.map_key},
          {"embedded_port_name", value.embedded_port_name},
          {"resolved_node", optionalNodeKey(value.resolved_node)},
          {"input_delay", arrayOf(value.input_delay, optionalNumber)},
          {"input_transition", optionalNumber(value.input_transition)},
          {"output_delay", arrayOf(value.output_delay, optionalNumber)},
          {"output_load", optionalNumber(value.output_load)},
          {"clock_name", value.clock_name}};
}

Json caseAnalysis(const DualCsrProjectedCaseAnalysis& value)
{
  return {{"raw_name", value.raw_name},
          {"resolved_node", optionalNodeKey(value.resolved_node)},
          {"value", value.value}};
}

Json timingCheck(const DualCsrProjectedTimingCheckEndpoint& value)
{
  return {{"instance_name", value.instance_name},
          {"referenced_check_ordinal", value.referenced_check_ordinal},
          {"check_kind", csr::token(value.check_kind)},
          {"clock_pin_raw_name", value.clock_pin_raw_name},
          {"data_pin_raw_name", value.data_pin_raw_name},
          {"clock_pin_full_name", value.clock_pin_full_name},
          {"data_pin_full_name", value.data_pin_full_name},
          {"resolved_clock_node", optionalNodeKey(value.resolved_clock_node)},
          {"resolved_data_node", optionalNodeKey(value.resolved_data_node)}};
}

Json startSeed(const DualCsrProjectedStartSeed& value)
{
  return {{"seed_node", nodeKey(value.seed_node)},
          {"path_start_node", nodeKey(value.path_start_node)},
          {"availability", csr::token(value.availability)},
          {"path_source", optionalValue(
                              value.path_source,
                              [](csr::PathSource present) {
                                return Json(csr::token(present));
                              })},
          {"clock_tag", value.clock_tag},
          {"arrival", laneMatrix(value.arrival)},
          {"slew", laneMatrix(value.slew)},
          {"crpr_node", optionalNodeKey(value.crpr_node)},
          {"crpr_transition", optionalValue(
                                  value.crpr_transition,
                                  [](csr::Transition present) {
                                    return Json(csr::token(present));
                                  })},
          {"output_load", optionalValue(value.output_load, loadMatrix)}};
}

Json endpoint(const DualCsrProjectedEndpointRequired& value)
{
  return {{"node", nodeKey(value.node)},
          {"clock_name", value.clock_name},
          {"clock_period", number(value.clock_period)},
          {"output_delay", arrayOf(value.output_delay, optionalNumber)}};
}

Json capacitance(const DualCsrProjectedCellPinCapacitance& value)
{
  return {{"node", nodeKey(value.node)},
          {"default_capacitance", number(value.default_capacitance)},
          {"transition_capacitance",
           laneMatrix(value.transition_capacitance)}};
}

Json reference(const DualCsrProjectedReferencePayload& value)
{
  return {
      {"schema_version", value.schema_version},
      {"analysis_context",
       {{"liberty_corner", value.analysis_context.liberty_corner},
        {"temperature", value.analysis_context.temperature},
        {"rc_corner", value.analysis_context.rc_corner}}},
      {"analysis_order", arrayOf(value.analysis_order,
                                 [](csr::AnalysisMode item) {
                                   return Json(csr::token(item));
                                 })},
      {"path_source_order", arrayOf(value.path_source_order,
                                    [](csr::PathSource item) {
                                      return Json(csr::token(item));
                                    })},
      {"transition_order", arrayOf(value.transition_order,
                                   [](csr::Transition item) {
                                     return Json(csr::token(item));
                                   })},
      {"instances", arrayOf(value.instances, instance)},
      {"ordered_roots", arrayOf(value.ordered_roots, nodeKey)},
      {"ordered_starts", arrayOf(value.ordered_starts, nodeKey)},
      {"ordered_ends", arrayOf(value.ordered_ends, nodeKey)},
      {"clock_tags", scalarArray(value.clock_tags)},
      {"clocks", arrayOf(value.clocks, clock)},
      {"clock_owners", arrayOf(value.clock_owners, clockOwner)},
      {"port_constraints", arrayOf(value.port_constraints, portConstraint)},
      {"case_analysis_constraints",
       arrayOf(value.case_analysis_constraints, caseAnalysis)},
      {"timing_check_endpoints",
       arrayOf(value.timing_check_endpoints, timingCheck)},
      {"referenced_library_cells",
       arrayOf(value.referenced_library_cells, libraryCell)},
      {"parasitics", parasitics(value.parasitics)},
      {"start_seeds", arrayOf(value.start_seeds, startSeed)},
      {"endpoint_required", arrayOf(value.endpoint_required, endpoint)},
      {"cell_pin_capacitances",
       arrayOf(value.cell_pin_capacitances, capacitance)},
      {"unsupported_reasons", arrayOf(
                                  value.unsupported_reasons,
                                  [](csr::CsrReferenceUnsupportedReason item) {
                                    return Json(csr::token(item));
                                  })}};
}

}  // namespace

nlohmann::json functionalGraphToJson(
    const DualCsrFunctionalProjection& graph)
{
  return {{"nodes", json_detail::arrayOf(graph.nodes, node)},
          {"active_arcs", json_detail::arrayOf(graph.active_arcs, arc)},
          {"reference_payload", reference(graph.reference_payload)}};
}

}  // namespace ista::timing_dual_csr::test::detail
