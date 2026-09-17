// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrExactReceiptJson.hpp"

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

#include "../timing_edit/TimingEditSemanticJson.hpp"
#include "DualCsrExactReceiptSemanticJson.hpp"
#include "DualCsrFunctionalReceiptJson.hpp"
#include "DualCsrReceiptJsonCommon.hpp"
#include "DualCsrResourceObservationJson.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using namespace json_detail;

using timing_edit::test::semantic_json::creationToken;
using timing_edit::test::semantic_json::stableArc;
using timing_edit::test::semantic_json::stableNet;
using timing_edit::test::semantic_json::stableNode;

template <typename... Visitors>
struct Overloaded : Visitors...
{
  using Visitors::operator()...;
};
template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

std::string_view phaseToken(DualCsrReceiptPhase value)
{
  switch (value) {
    case DualCsrReceiptPhase::kPreCandidatePrepare:
      return "pre_candidate_prepare";
    case DualCsrReceiptPhase::kCandidateApplyExecute:
      return "candidate_apply_execute";
    case DualCsrReceiptPhase::kPostCandidateCapture:
      return "post_candidate_capture";
    case DualCsrReceiptPhase::kDualCsrUpdate:
      return "dual_csr_update";
    case DualCsrReceiptPhase::kFullLevelization:
      return "full_levelization";
    case DualCsrReceiptPhase::kMixedLevelization:
      return "mixed_levelization";
    case DualCsrReceiptPhase::kFullPropagation:
      return "full_propagation";
    case DualCsrReceiptPhase::kIntrinsicVerify:
      return "intrinsic_verify";
    case DualCsrReceiptPhase::kPublish:
      return "publish";
    case DualCsrReceiptPhase::kNativeFinalRollback:
      return "native_final_rollback";
  }
  throw std::invalid_argument("unknown dual CSR receipt phase");
}

Json arcKeyBase(const csr::ArcKeyBase& value)
{
  Json result = arcKey(csr::ArcKey{.base = value});
  result.erase("parallel_ordinal");
  return result;
}

Json highWater(const timing_edit::AppendOnlyHighWater& value)
{
  return {{"node_slots", value.node_slots},
          {"net_slots", value.net_slots},
          {"arc_slots", value.arc_slots}};
}

Json extent(const csr::CsrBufferExtent& value)
{
  return {{"logical_size", value.logical_size},
          {"capacity", value.capacity}};
}

Json slots(const std::vector<std::optional<csr::ArcId>>& values)
{
  return arrayOf(values, [](const auto& value) {
    return optionalValue(value, [](csr::ArcId id) {
      return Json(id.value());
    });
  });
}

Json base(const BaseCsrState& value)
{
  return {{"row_offsets", scalarArray(value.row_offsets)},
          {"physical_slots", slots(value.physical_slots)},
          {"offsets_extent", extent(value.offsets_extent)},
          {"edge_extent", extent(value.edge_extent)}};
}

Json aux(const AuxOutCsrState& value)
{
  return {{"row_offsets", scalarArray(value.row_offsets)},
          {"row_sources", arrayOf(value.row_sources, [](csr::NodeId id) {
             return Json(id.value());
           })},
          {"arc_ids", arrayOf(value.arc_ids, [](csr::ArcId id) {
             return Json(id.value());
           })},
          {"source_to_aux_row", scalarArray(value.source_to_aux_row)},
          {"offsets_extent", extent(value.offsets_extent)},
          {"source_extent", extent(value.source_extent)},
          {"edge_extent", extent(value.edge_extent)},
          {"mapping_extent", extent(value.mapping_extent)}};
}

Json visits(const csr::CsrStageStorageVisits& value)
{
  return {{"base_out", value.base_out},
          {"aux_out", value.aux_out},
          {"base_in", value.base_in}};
}

Json traversal(const csr::CsrTraversalTelemetry& value)
{
  return {{"levelization", visits(value.levelization)},
          {"arc_initialization", visits(value.arc_initialization)},
          {"forward_propagation", visits(value.forward_propagation)},
          {"required_propagation", visits(value.required_propagation)}};
}

Json telemetry(const DualCsrTelemetry& value)
{
  const auto grow = [](const DualCsrCapacityGrowEvent& item) {
    return Json{{"buffer", dualCsrBufferToken(item.buffer)},
                {"old_capacity", item.old_capacity},
                {"required", item.required},
                {"new_capacity", item.new_capacity}};
  };
  return {{"generation_before", value.generation_before},
          {"generation_after", value.generation_after},
          {"logical_edges_before", value.logical_edges_before},
          {"logical_edges_after", value.logical_edges_after},
          {"base_physical_edges", value.base_physical_edges},
          {"base_active_edges", value.base_active_edges},
          {"base_tombstones", value.base_tombstones},
          {"aux_rows", value.aux_rows},
          {"aux_edges", value.aux_edges},
          {"tombstone_reuse_count", value.tombstone_reuse_count},
          {"base_change_count", value.base_change_count},
          {"reverse_change_count", value.reverse_change_count},
          {"nodes_appended", value.nodes_appended},
          {"arcs_appended", value.arcs_appended},
          {"payload_change_count", value.payload_change_count},
          {"aux_rebuild_count", value.aux_rebuild_count},
          {"full_rebuild_count", value.full_rebuild_count},
          {"fallback_count", value.fallback_count},
          {"capacity_grow_events",
           arrayOf(value.capacity_grow_events, grow)},
          {"traversal", traversal(value.traversal)}};
}

Json payload(const csr::ArcSemanticPayload& value)
{
  return {{"key", arcKey(value.key)},
          {"source", value.source.value()},
          {"sink", value.sink.value()},
          {"kind", csr::token(value.kind)},
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

Json arcGroupIndex(const DualCsrArcGroupIndex& value)
{
  Json slots_json = Json::array();
  for (std::size_t index = 0; index < value.slots.size(); ++index) {
    const auto& slot = value.slots[index];
    slots_json.push_back(
        {{"arc_id", index},
         {"ref", stableArc(slot.ref)},
         {"current_active_key",
          optionalValue(slot.current_active_key,
                        [](const csr::ArcKey& key) {
                          return arcKey(key);
                        })}});
  }
  const auto group = [](const DualCsrArcGroup& item) {
    return Json{{"base", arcKeyBase(item.base)},
                {"active_arc_ids",
                 arrayOf(item.active_arc_ids, [](csr::ArcId id) {
                   return Json(id.value());
                 })}};
  };
  const auto sink_net_group = [](const DualCsrArcSinkNetGroup& item) {
    return Json{{"sink_net_name", item.sink_net_name},
                {"active_arc_ids",
                 arrayOf(item.active_arc_ids, [](csr::ArcId id) {
                   return Json(id.value());
                 })}};
  };
  const auto net_sink_group = [](const DualCsrNetSinkGroup& item) {
    return Json{{"key",
                 {{"owner_name", item.key.owner_name},
                  {"sink", nodeKey(item.key.sink)}}},
                {"active_arc_ids",
                 arrayOf(item.active_arc_ids, [](csr::ArcId id) {
                   return Json(id.value());
                 })}};
  };
  return {{"session_token", value.session_token.value()},
          {"generation", value.generation.value()},
          {"arc_high_water", value.arc_high_water},
          {"slots", std::move(slots_json)},
          {"active_groups", arrayOf(value.active_groups, group)},
          {"active_sink_net_groups",
           arrayOf(value.active_sink_net_groups, sink_net_group)},
          {"active_net_sink_groups",
           arrayOf(value.active_net_sink_groups, net_sink_group)}};
}

Json audit(const NarrowCaptureAudit& value)
{
  return {{"requested_nodes", value.requested_nodes},
          {"node_reads", value.node_reads},
          {"accepted_nodes", value.accepted_nodes},
          {"requested_instances", value.requested_instances},
          {"instance_reads", value.instance_reads},
          {"accepted_instances", value.accepted_instances},
          {"requested_arcs", value.requested_arcs},
          {"native_arc_candidate_reads",
           value.native_arc_candidate_reads},
          {"accepted_arcs", value.accepted_arcs},
          {"requested_deactivations", value.requested_deactivations},
          {"deactivated_arc_before_reads",
           value.deactivated_arc_before_reads},
          {"accepted_deactivations", value.accepted_deactivations},
          {"captured_arc_records", value.captured_arc_records},
          {"requested_load_nets", value.requested_load_nets},
          {"net_reads", value.net_reads},
          {"accepted_load_nets", value.accepted_load_nets},
          {"requested_library_cells", value.requested_library_cells},
          {"library_cell_reads", value.library_cell_reads},
          {"accepted_library_cells", value.accepted_library_cells},
          {"requested_cell_pin_capacitances",
           value.requested_cell_pin_capacitances},
          {"cell_pin_capacitance_reads",
           value.cell_pin_capacitance_reads},
          {"accepted_cell_pin_capacitances",
           value.accepted_cell_pin_capacitances},
          {"requested_node_roles", value.requested_node_roles},
          {"node_role_reads", value.node_role_reads},
          {"accepted_node_roles", value.accepted_node_roles},
          {"index_group_lookups", value.index_group_lookups},
          {"native_arc_candidate_upper_bound",
           value.native_arc_candidate_upper_bound},
          {"arc_identity_tie_breaks", value.arc_identity_tie_breaks},
          {"rejected_reads", value.rejected_reads},
          {"full_export_guard_active", value.full_export_guard_active},
          {"full_export_observed", value.full_export_observed},
          {"native_full_graph_exports", value.native_full_graph_exports},
          {"native_full_reference_exports",
           value.native_full_reference_exports}};
}

Json counters(const DualCsrOperationCounters& value)
{
  return {{"native_full_graph_exports", value.native_full_graph_exports},
          {"native_full_reference_exports",
           value.native_full_reference_exports},
          {"host_snapshot_rebuilds", value.host_snapshot_rebuilds},
          {"baseline_factory_calls", value.baseline_factory_calls},
          {"fallback_calls", value.fallback_calls},
          {"candidate_creates", value.candidate_creates},
          {"candidate_clones", value.candidate_clones},
          {"candidate_applies", value.candidate_applies},
          {"candidate_executes", value.candidate_executes},
          {"candidate_installs", value.candidate_installs},
          {"candidate_rollbacks", value.candidate_rollbacks}};
}

std::string_view executionModeToken(LevelizationExecutionMode value)
{
  if (value == LevelizationExecutionMode::kFullStage5)
    return "full_stage5";
  if (value == LevelizationExecutionMode::kMixedStage6)
    return "mixed_stage6";
  throw std::invalid_argument("unknown levelization execution mode");
}

Json levelization(const csr::CsrFifoLevelization& value)
{
  return {{"complete", value.complete},
          {"level_offsets", scalarArray(value.level_offsets)},
          {"level_nodes", arrayOf(value.level_nodes, [](csr::NodeId node) {
             return Json(node.value());
           })},
          {"node_to_level", scalarArray(value.node_to_level)},
          {"view_error", value.view_error ? Json("present") : Json(nullptr)}};
}

Json mixedTrace(const MixedLevelizationDeterministicTrace& value)
{
  return {{"n_last_nodes", value.n_last_nodes},
          {"n_current_nodes", value.n_current_nodes},
          {"n_new_nodes", value.n_new_nodes},
          {"n_old_levels", value.n_old_levels},
          {"mixed_vertex_count", value.mixed_vertex_count},
          {"actual_eligible_arc_visits", value.actual_eligible_arc_visits},
          {"old_old", value.old_old},
          {"old_new", value.old_new},
          {"new_old", value.new_old},
          {"new_new", value.new_new},
          {"mapped_actual_arc_candidates",
           value.mapped_actual_arc_candidates},
          {"unique_actual_endpoint_pairs",
           value.unique_actual_endpoint_pairs},
          {"actual_parallel_duplicates_collapsed",
           value.actual_parallel_duplicates_collapsed},
          {"proxy_alias_duplicates_collapsed",
           value.proxy_alias_duplicates_collapsed},
          {"unique_precedence_edges", value.unique_precedence_edges},
          {"virtual_chain_edges", value.virtual_chain_edges},
          {"base_out", value.base_out},
          {"aux_out", value.aux_out},
          {"kahn_processed_count", value.kahn_processed_count},
          {"max_frontier", value.max_frontier},
          {"reconstructed_level_count",
           value.reconstructed_level_count},
          {"actual_cycle_classifier_runs",
           value.actual_cycle_classifier_runs},
          {"actual_cycle_classifier_arc_visits",
           value.actual_cycle_classifier_arc_visits},
          {"fallback_count", value.fallback_count}};
}

Json executionTrace(const LevelizationExecutionTrace& value)
{
  return {{"generation", value.generation().value()},
          {"mode", executionModeToken(value.mode())},
          {"levels", levelization(value.levels())},
          {"traversal", traversal(value.traversal())},
          {"full_levelizer_entry_delta",
           value.fullLevelizerEntryDelta()},
          {"fallback_count", value.fallbackCount()},
          {"supplied_levels_validated",
           value.suppliedLevelsValidated()},
          {"mixed", value.mixed() ? mixedTrace(*value.mixed())
                                  : Json(nullptr)}};
}

Json evidence(const DualCsrTransactionEvidence& value)
{
  return {{"phase_trace", arrayOf(value.phase_trace,
                                  [](DualCsrReceiptPhase item) {
                                    return Json(phaseToken(item));
                                  })},
          {"capture_audits", arrayOf(value.capture_audits, audit)},
          {"operation_counters", counters(value.operation_counters)},
          {"incremental_batch_executed",
           value.incremental_batch_executed},
          {"propagation_scope", "full_graph"},
          {"native_final_rollback_completed",
           value.native_final_rollback_completed}};
}

}  // namespace

nlohmann::json structuralReceiptToJson(
    const DualCsrStructuralReceipt& receipt)
{
  const auto inactive = [](const InactiveArcStructuralObservation& value) {
    return Json{{"id", value.id.value()},
                {"historical_payload", payload(value.historical_payload)}};
  };
  return {{"generation", receipt.generation.value()},
          {"high_water", highWater(receipt.high_water)},
          {"arc_group_index", arcGroupIndex(receipt.arc_group_index)},
          {"inactive_arcs", arrayOf(receipt.inactive_arcs, inactive)},
          {"base_out", base(receipt.base_out)},
          {"base_in", base(receipt.base_in)},
          {"aux_out", aux(receipt.aux_out)},
          {"update_telemetry", telemetry(receipt.update_telemetry)}};
}

nlohmann::json transactionReceiptToJson(const DualCsrTransactionReceipt& transaction)
{
  return {{"session_token", transaction.session_token.value()},
        {"generation", transaction.generation.value()},
        {"baseline_reference_input",
         semantic_json::referenceInput(
             transaction.baseline_reference_input)},
        {"resolver", semantic_json::resolver(transaction.resolver)},
        {"arc_group_index", arcGroupIndex(transaction.arc_group_index)},
        {"identity", semantic_json::identity(transaction.identity)},
        {"descriptor_trace",
         arrayOf(transaction.descriptor_trace, semantic_json::descriptor)},
        {"semantic_trace",
         arrayOf(transaction.semantic_trace, semantic_json::semanticPlan)},
        {"traversal_telemetry", traversal(transaction.traversal_telemetry)},
        {"execution_trace",
         arrayOf(transaction.execution_trace, executionTrace)},
        {"update_telemetry", telemetry(transaction.update_telemetry)},
        {"evidence", evidence(transaction.evidence)}};
}

}  // namespace ista::timing_dual_csr::test
