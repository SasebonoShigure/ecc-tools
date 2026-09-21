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

Json nodeTiming(const DualCsrProjectedNodeTiming& value)
{
  return {{"is_clock_point", optionalSoa(
                                 value.is_clock_point,
                                 [](std::uint8_t item) { return Json(item); })},
          {"arrivals", optionalSoa(value.arrivals, number)},
          {"requireds", optionalSoa(value.requireds, number)},
          {"slacks", optionalSoa(value.slacks, number)},
          {"launches", optionalSoa(value.launches, number)},
          {"clock_names", optionalSoa(
                              value.clock_names,
                              [](const std::string& item) {
                                return Json(item);
                              })},
          {"predecessor_nodes", optionalSoa(value.predecessor_nodes, nodeKey)},
          {"predecessor_arcs", optionalSoa(value.predecessor_arcs, arcKey)}};
}

Json clockTiming(const DualCsrProjectedClockTiming& value)
{
  return {{"arrivals", optionalSoa(value.arrivals, number)},
          {"slews", optionalSoa(value.slews, number)},
          {"predecessor_nodes", optionalSoa(value.predecessor_nodes, nodeKey)},
          {"predecessor_arc_delays",
           optionalSoa(value.predecessor_arc_delays, number)},
          {"predecessor_transitions",
           optionalSoa(value.predecessor_transitions,
                       [](csr::Transition item) {
                         return Json(csr::token(item));
                       })}};
}

Json pathTiming(const DualCsrProjectedPathTiming& value)
{
  return {{"row_offsets", scalarArray(value.row_offsets)},
          {"clock_tags", optionalSoa(
                             value.clock_tags,
                             [](const std::string& item) {
                               return Json(item);
                             })},
          {"arrivals", optionalSoa(value.arrivals, number)},
          {"slews", optionalSoa(value.slews, number)},
          {"launches", optionalSoa(value.launches, number)},
          {"start_nodes", optionalSoa(value.start_nodes, nodeKey)},
          {"predecessor_nodes", optionalSoa(value.predecessor_nodes, nodeKey)},
          {"predecessor_arcs", optionalSoa(value.predecessor_arcs, arcKey)},
          {"predecessor_arc_delays",
           optionalSoa(value.predecessor_arc_delays, number)},
          {"predecessor_transitions",
           optionalSoa(value.predecessor_transitions,
                       [](csr::Transition item) {
                         return Json(csr::token(item));
                       })},
          {"crpr_nodes", optionalSoa(value.crpr_nodes, nodeKey)},
          {"crpr_transitions", optionalSoa(
                                   value.crpr_transitions,
                                   [](csr::Transition item) {
                                     return Json(csr::token(item));
                                   })}};
}

Json arcTiming(const DualCsrProjectedArcTiming& value)
{
  return {{"delays", optionalSoa(value.delays, number)},
          {"min_delays", optionalSoa(value.min_delays, number)},
          {"max_delays", optionalSoa(value.max_delays, number)},
          {"transition_delays", optionalSoa(value.transition_delays, number)},
          {"input_output_delays",
           optionalSoa(value.input_output_delays, number)},
          {"graph_delays", optionalSoa(value.graph_delays, number)},
          {"transition_types", optionalSoa(
                                   value.transition_types,
                                   [](csr::Transition item) {
                                     return Json(csr::token(item));
                                   })},
          {"timing_arc_row_offsets",
           scalarArray(value.timing_arc_row_offsets)},
          {"timing_arc_indices", scalarArray(value.timing_arc_indices)},
          {"timing_arc_delays", optionalSoa(value.timing_arc_delays, number)},
          {"timing_arc_fallbacks", optionalSoa(
                                      value.timing_arc_fallbacks,
                                      [](std::uint8_t item) {
                                        return Json(item);
                                      })}};
}

}  // namespace

nlohmann::json functionalTimingToJson(
    const DualCsrTimingSemanticProjection& timing)
{
  using namespace json_detail;
  return {{"schema_version", timing.schema_version},
          {"level_offsets", scalarArray(timing.level_offsets)},
          {"level_nodes", arrayOf(timing.level_nodes, nodeKey)},
          {"node_to_level", scalarArray(timing.node_to_level)},
          {"nodes", nodeTiming(timing.nodes)},
          {"effective_clock", clockTiming(timing.effective_clock)},
          {"physical_clock", clockTiming(timing.physical_clock)},
          {"data_slews", optionalSoa(timing.data_slews, number)},
          {"paths", pathTiming(timing.paths)},
          {"arcs", arcTiming(timing.arcs)}};
}

}  // namespace ista::timing_dual_csr::test::detail
