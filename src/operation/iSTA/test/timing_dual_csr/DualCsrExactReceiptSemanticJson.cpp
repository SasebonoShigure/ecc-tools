// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrExactReceiptSemanticJson.hpp"

#include <stdexcept>
#include <string_view>
#include <variant>

#include "DualCsrReceiptJsonCommon.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr::test::semantic_json {
namespace {

using namespace json_detail;

template <typename... Visitors>
struct Overloaded : Visitors...
{
  using Visitors::operator()...;
};
template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

Json creationToken(const timing_edit::CreationToken& value)
{
  return {{"schema_version", value.schema_version},
          {"case_id", value.case_id},
          {"profile_id", value.profile_id},
          {"seed", value.seed},
          {"batch_ordinal", value.batch_ordinal},
          {"accepted_ordinal", value.accepted_ordinal}};
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

Json fingerprints(const timing_edit::ComponentFingerprints& value)
{
  return {{"schema_version", value.schema_version},
          {"topology", value.topology},
          {"clock_constraint", value.clock_constraint},
          {"library", value.library},
          {"corner", value.corner},
          {"parasitic", value.parasitic},
          {"timing_input", value.timing_input}};
}

Json canonicalState(const timing_edit::CanonicalPreStateEvidence& value)
{
  return {{"instances", scalarArray(value.instances)},
          {"nets", scalarArray(value.nets)},
          {"nodes", arrayOf(value.nodes, nodeKey)},
          {"active_arcs", arrayOf(value.arcs, arcKey)}};
}

Json baseline(const timing_edit::BaselineSemanticIdentity& value)
{
  return {{"design_name", value.design_name},
          {"fingerprints", fingerprints(value.fingerprints)},
          {"canonical_pre_state", canonicalState(value.canonical_pre_state)}};
}

Json libraryCellRef(const timing_edit::LibraryCellRef& value)
{
  return {{"library_name", value.library_name},
          {"cell_name", value.cell_name},
          {"input_port", value.input_port},
          {"output_port", value.output_port},
          {"ordered_arc_signatures",
           arrayOf(value.ordered_arc_signatures, librarySignature)}};
}

Json placement(const timing_edit::PlacementInputs& value)
{
  return std::visit(
      Overloaded{
          [](const timing_edit::UnplacedInputs&) {
            return Json{{"kind", "unplaced"}};
          },
          [](const timing_edit::PlacedInputs& item) {
            return Json{{"kind", "placed"},
                        {"x_microns", item.x_microns},
                        {"y_microns", item.y_microns}};
          }},
      value);
}

Json rcInputs(const timing_edit::RcInputs& value)
{
  return std::visit(
      Overloaded{
          [](const timing_edit::NoParasiticRcInputs&) {
            return Json{{"kind", "none"}};
          },
          [](const timing_edit::ParasiticRcInputs& item) {
            return Json{{"kind", "parasitic"},
                        {"policy", timing_edit::token(item.parasitic_policy)}};
          }},
      value);
}

Json highWater(const timing_edit::AppendOnlyHighWater& value)
{
  return {{"node_slots", value.node_slots},
          {"net_slots", value.net_slots},
          {"arc_slots", value.arc_slots}};
}

Json binding(const timing_edit::NativeSlotBinding& value)
{
  return {{"slot", value.slot.value()},
          {"native_name", value.native_name},
          {"tombstone", value.tombstone}};
}

Json materialization(const timing_edit::TokenMaterialization& value)
{
  return {{"creation_token", creationToken(value.creation_token)},
          {"instance_name", value.instance_name},
          {"output_net_name", value.output_net_name},
          {"input_pin_name", value.input_pin_name},
          {"output_pin_name", value.output_pin_name}};
}

Json snapshotIdentity(const csr::CsrSnapshotIdentity& value)
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

Json analysisContext(const csr::StaAnalysisContext& value)
{
  return {{"liberty_corner", value.liberty_corner},
          {"temperature", value.temperature},
          {"rc_corner", value.rc_corner}};
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

Json arcPayload(const csr::ArcSemanticPayload& value)
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

Json instanceRecord(const csr::InstanceRecord& value)
{
  return {{"instance_name", value.instance_name},
          {"cell_name", value.cell_name},
          {"ordered_pins", arrayOf(value.ordered_pins, [](csr::NodeId id) {
             return Json(id.value());
           })},
          {"is_sequential", value.is_sequential},
          {"has_timing_checks", value.has_timing_checks},
          {"is_clock_gating", value.is_clock_gating},
          {"is_macro", value.is_macro}};
}

Json optionalNodeId(const std::optional<csr::NodeId>& value)
{
  return optionalValue(value,
                       [](csr::NodeId id) { return Json(id.value()); });
}

Json rawNode(const csr::RawNodeReference& value)
{
  return {{"full_name", value.full_name},
          {"resolved_node", optionalNodeId(value.resolved_node)}};
}

Json clock(const csr::ClockConstraintModel& value)
{
  return {{"map_key", value.map_key},
          {"embedded_clock_name", value.embedded_clock_name},
          {"ordered_sources", arrayOf(value.ordered_sources, rawNode)},
          {"period", number(value.period)},
          {"edges", arrayOf(value.edges, number)},
          {"propagated", value.propagated}};
}

Json optionalNumbers(
    const std::array<std::optional<double>, csr::kAnalysisCount>& value)
{
  return arrayOf(value, [](const std::optional<double>& item) {
    return optionalNumber(item);
  });
}

Json portConstraint(const csr::PortConstraintModel& value)
{
  return {{"map_key", value.map_key},
          {"embedded_port_name", value.embedded_port_name},
          {"resolved_node", optionalNodeId(value.resolved_node)},
          {"input_delay", optionalNumbers(value.input_delay)},
          {"input_transition", optionalNumber(value.input_transition)},
          {"output_delay", optionalNumbers(value.output_delay)},
          {"output_load", optionalNumber(value.output_load)},
          {"clock_name", value.clock_name}};
}

Json caseAnalysis(const csr::CaseAnalysisConstraintModel& value)
{
  return {{"raw_name", value.raw_name},
          {"resolved_node", optionalNodeId(value.resolved_node)},
          {"value", value.value}};
}

Json timingCheck(const csr::TimingCheckEndpointModel& value)
{
  return {{"instance_name", value.instance_name},
          {"referenced_check_ordinal", value.referenced_check_ordinal},
          {"check_kind", csr::token(value.check_kind)},
          {"clock_pin_raw_name", value.clock_pin_raw_name},
          {"data_pin_raw_name", value.data_pin_raw_name},
          {"clock_pin_full_name", value.clock_pin_full_name},
          {"data_pin_full_name", value.data_pin_full_name},
          {"resolved_clock_node", optionalNodeId(value.resolved_clock_node)},
          {"resolved_data_node", optionalNodeId(value.resolved_data_node)}};
}

Json startSeed(const csr::StartSeedModel& value)
{
  return {{"seed_node", value.seed_node.value()},
          {"path_start_node", value.path_start_node.value()},
          {"availability", csr::token(value.availability)},
          {"path_source", optionalValue(value.path_source, [](auto item) {
             return Json(csr::token(item));
           })},
          {"clock_tag", value.clock_tag},
          {"arrival", laneMatrix(value.arrival)},
          {"slew", laneMatrix(value.slew)},
          {"crpr_node", optionalNodeId(value.crpr_node)},
          {"crpr_transition",
           optionalValue(value.crpr_transition, [](auto item) {
             return Json(csr::token(item));
           })}};
}

Json endpoint(const csr::EndpointRequiredModel& value)
{
  return {{"node", value.node.value()},
          {"clock_name", value.clock_name},
          {"clock_period", number(value.clock_period)},
          {"output_delay", optionalNumbers(value.output_delay)}};
}

Json capacitance(const csr::CellPinCapacitanceModel& value)
{
  return {{"node", value.node.value()},
          {"default_capacitance", number(value.default_capacitance)},
          {"transition_capacitance", laneMatrix(value.transition_capacitance)}};
}

template <typename T, typename Convert>
Json replacement(const std::optional<ValueReplacement<T>>& value,
                 Convert convert)
{
  return optionalValue(value, [&](const ValueReplacement<T>& item) {
    return Json{{"expected", convert(item.expected)},
                {"replacement", convert(item.replacement)}};
  });
}

Json referenceDelta(const DualCsrReferencePayloadDelta& value)
{
  const auto text = [](const std::string& item) { return Json(item); };
  const auto ids = [](const std::vector<csr::NodeId>& items) {
    return arrayOf(items, [](csr::NodeId id) { return Json(id.value()); });
  };
  const auto enum_tokens = [](const auto& items) {
    return arrayOf(items, [](auto item) { return Json(csr::token(item)); });
  };
  const auto optional_texts = [](const auto& items) {
    return arrayOf(items, [](const auto& item) { return optionalText(item); });
  };
  return {
      {"schema_version", replacement(value.schema_version, text)},
      {"source_identity", replacement(value.source_identity, snapshotIdentity)},
      {"analysis_context", replacement(value.analysis_context, analysisContext)},
      {"analysis_order", replacement(value.analysis_order, enum_tokens)},
      {"path_source_order", replacement(value.path_source_order, enum_tokens)},
      {"transition_order", replacement(value.transition_order, enum_tokens)},
      {"instances", replacement(value.instances, [](const auto& items) {
         return arrayOf(items, instanceRecord);
       })},
      {"ordered_root_ids", replacement(value.ordered_root_ids, ids)},
      {"ordered_start_ids", replacement(value.ordered_start_ids, ids)},
      {"ordered_end_ids", replacement(value.ordered_end_ids, ids)},
      {"clock_tags", replacement(value.clock_tags,
                                   [](const auto& items) {
                                     return scalarArray(items);
                                   })},
      {"clocks", replacement(value.clocks, [](const auto& items) {
         return arrayOf(items, clock);
       })},
      {"clock_owner_by_node",
       replacement(value.clock_owner_by_node, optional_texts)},
      {"port_constraints",
       replacement(value.port_constraints, [](const auto& items) {
         return arrayOf(items, portConstraint);
       })},
      {"case_analysis_constraints",
       replacement(value.case_analysis_constraints, [](const auto& items) {
         return arrayOf(items, caseAnalysis);
       })},
      {"timing_check_endpoints",
       replacement(value.timing_check_endpoints, [](const auto& items) {
         return arrayOf(items, timingCheck);
       })},
      {"referenced_library_cells",
       replacement(value.referenced_library_cells, [](const auto& items) {
         return arrayOf(items, libraryCell);
       })},
      {"parasitics", replacement(value.parasitics, parasitics)},
      {"start_seeds", replacement(value.start_seeds, [](const auto& items) {
         return arrayOf(items, startSeed);
       })},
      {"endpoint_required",
       replacement(value.endpoint_required, [](const auto& items) {
         return arrayOf(items, endpoint);
       })},
      {"cell_pin_capacitances",
       replacement(value.cell_pin_capacitances, [](const auto& items) {
         return arrayOf(items, capacitance);
       })},
      {"unsupported_reasons",
       replacement(value.unsupported_reasons, enum_tokens)}};
}

std::string_view deactivationReason(LogicalArcDeactivateReason value)
{
  switch (value) {
    case LogicalArcDeactivateReason::kRemoved:
      return "removed";
    case LogicalArcDeactivateReason::kReplaced:
      return "replaced";
    case LogicalArcDeactivateReason::kAuxCleared:
      return "aux_cleared";
  }
  throw std::invalid_argument("unknown logical arc deactivation reason");
}

}  // namespace

Json stableNode(const timing_edit::StableNodeRef& value)
{
  return std::visit(
      Overloaded{
          [](const timing_edit::ExistingNodeRef& item) {
            return Json{{"kind", "existing"},
                        {"baseline_key", nodeKey(item.baseline_key)}};
          },
          [](const timing_edit::CreatedNodeRef& item) {
            return Json{{"kind", "created"},
                        {"creation_token", creationToken(item.creation_token)},
                        {"role", timing_edit::token(item.role)}};
          }},
      value);
}

Json stableNet(const timing_edit::StableNetRef& value)
{
  return std::visit(
      Overloaded{
          [](const timing_edit::ExistingNetRef& item) {
            return Json{{"kind", "existing"},
                        {"baseline_name", item.baseline_name}};
          },
          [](const timing_edit::CreatedNetRef& item) {
            return Json{{"kind", "created"},
                        {"creation_token", creationToken(item.creation_token)},
                        {"role", timing_edit::token(item.role)}};
          }},
      value);
}

Json stableArc(const timing_edit::StableArcRef& value)
{
  return std::visit(
      Overloaded{
          [](const timing_edit::ExistingArcRef& item) {
            return Json{{"kind", "existing"},
                        {"baseline_key", arcKey(item.baseline_key)}};
          },
          [](const timing_edit::CreatedCellArcRef& item) {
            return Json{{"kind", "created_cell"},
                        {"creation_token", creationToken(item.creation_token)},
                        {"library_source_port", item.library_source_port},
                        {"library_sink_port", item.library_sink_port},
                        {"ordered_library_arcs",
                         arrayOf(item.ordered_library_arcs, librarySignature)},
                        {"stable_equivalence_ordinal",
                         item.stable_equivalence_ordinal}};
          },
          [](const timing_edit::CreatedOldNetArcRef& item) {
            return Json{{"kind", "created_old_net"},
                        {"net", stableNet(item.net)},
                        {"driver", stableNode(item.driver)},
                        {"sink", stableNode(item.sink)},
                        {"stable_ordinal", item.stable_ordinal}};
          },
          [](const timing_edit::CreatedNewNetArcRef& item) {
            return Json{{"kind", "created_new_net"},
                        {"creation_token", creationToken(item.creation_token)},
                        {"driver", stableNode(item.driver)},
                        {"sink", stableNode(item.sink)},
                        {"stable_ordinal", item.stable_ordinal}};
          }},
      value);
}

Json resolver(const timing_edit::TimingEditResolverState& value)
{
  Json nodes = Json::array();
  for (const auto& [ref, item] : value.nodeBindings())
    nodes.push_back({{"ref", stableNode(ref)}, {"binding", binding(item)}});
  Json nets = Json::array();
  for (const auto& [ref, item] : value.netBindings())
    nets.push_back({{"ref", stableNet(ref)}, {"binding", binding(item)}});
  Json arcs = Json::array();
  for (const auto& [ref, item] : value.arcBindings())
    arcs.push_back({{"ref", stableArc(ref)}, {"binding", binding(item)}});
  Json materializations = Json::array();
  for (const auto& [token, item] : value.materializations())
    materializations.push_back(
        {{"key", creationToken(token)},
         {"value", materialization(item)}});
  return {{"high_water", highWater(value.highWater())},
          {"node_bindings", std::move(nodes)},
          {"net_bindings", std::move(nets)},
          {"arc_bindings", std::move(arcs)},
          {"materializations", std::move(materializations)}};
}

Json identity(const timing_edit::CommittedEditIdentity& value)
{
  return {{"generation", value.generation.value()},
          {"current_fingerprints", fingerprints(value.current_fingerprints)},
          {"canonical_pre_state", canonicalState(value.canonical_pre_state)}};
}

Json referenceInput(const csr::CsrReferenceInput& value)
{
  const auto node_ids = [](const auto& values) {
    return arrayOf(values, [](csr::NodeId id) { return Json(id.value()); });
  };
  const auto arc_ids = [](const auto& values) {
    return arrayOf(values, [](csr::ArcId id) { return Json(id.value()); });
  };
  const auto enum_tokens = [](const auto& values) {
    return arrayOf(values, [](auto item) { return Json(csr::token(item)); });
  };
  return {
      {"schema_version", value.schema_version},
      {"source_identity", snapshotIdentity(value.source_identity)},
      {"analysis_context", analysisContext(value.analysis_context)},
      {"analysis_order", enum_tokens(value.analysis_order)},
      {"path_source_order", enum_tokens(value.path_source_order)},
      {"transition_order", enum_tokens(value.transition_order)},
      {"nodes", arrayOf(value.nodes, nodeRecord)},
      {"node_ids_by_key", arrayOf(value.node_ids_by_key, [](const auto& item) {
         return Json{{"key", nodeKey(item.key)}, {"id", item.id.value()}};
       })},
      {"instances", arrayOf(value.instances, instanceRecord)},
      {"arcs", arrayOf(value.arcs, arcRecord)},
      {"arc_ids_by_key", arrayOf(value.arc_ids_by_key, [](const auto& item) {
         return Json{{"key", arcKey(item.key)}, {"id", item.id.value()}};
       })},
      {"out_row_offsets", scalarArray(value.out_row_offsets)},
      {"out_arc_ids", arc_ids(value.out_arc_ids)},
      {"in_row_offsets", scalarArray(value.in_row_offsets)},
      {"in_arc_ids", arc_ids(value.in_arc_ids)},
      {"ordered_root_ids", node_ids(value.ordered_root_ids)},
      {"ordered_start_ids", node_ids(value.ordered_start_ids)},
      {"ordered_end_ids", node_ids(value.ordered_end_ids)},
      {"clock_tags", scalarArray(value.clock_tags)},
      {"clocks", arrayOf(value.clocks, clock)},
      {"clock_owner_by_node", arrayOf(value.clock_owner_by_node, optionalText)},
      {"port_constraints", arrayOf(value.port_constraints, portConstraint)},
      {"case_analysis_constraints", arrayOf(value.case_analysis_constraints, caseAnalysis)},
      {"timing_check_endpoints", arrayOf(value.timing_check_endpoints, timingCheck)},
      {"referenced_library_cells", arrayOf(value.referenced_library_cells, libraryCell)},
      {"parasitics", parasitics(value.parasitics)},
      {"start_seeds", arrayOf(value.start_seeds, startSeed)},
      {"endpoint_required", arrayOf(value.endpoint_required, endpoint)},
      {"cell_pin_capacitances", arrayOf(value.cell_pin_capacitances, capacitance)},
      {"unsupported_reasons", enum_tokens(value.unsupported_reasons)}};
}

Json descriptor(const DualCsrBatchDescriptor& value)
{
  const auto node_append = [](const NodeAppendRecord& item) {
    return Json{{"id", item.id.value()},
                {"key", nodeKey(item.key)},
                {"payload", nodeRecord(item.payload)}};
  };
  const auto arc_append = [](const ArcAppendRecord& item) {
    return Json{{"id", item.id.value()},
                {"key", arcKey(item.key)},
                {"source", item.source.value()},
                {"sink", item.sink.value()},
                {"payload", arcPayload(item.payload)}};
  };
  const auto node_change = [](const NodePayloadChange& item) {
    return Json{{"id", item.id.value()},
                {"expected_key", nodeKey(item.expected_key)},
                {"expected_net_name", item.expected_net_name},
                {"replacement_net_name", item.replacement_net_name}};
  };
  const auto remove = [](const BaseOutRemoveDescriptor& item) {
    return Json{{"source", item.source.value()},
                {"physical_slot", item.physical_slot},
                {"expected_arc", item.expected_arc.value()}};
  };
  const auto change = [](const BaseOutChangeDescriptor& item) {
    return Json{{"source", item.source.value()},
                {"physical_slot", item.physical_slot},
                {"expected_arc", optionalValue(item.expected_arc, [](auto id) {
                   return Json(id.value());
                 })},
                {"replacement_arc", item.replacement_arc.value()}};
  };
  const auto reverse = [](const BaseInChangeDescriptor& item) {
    return Json{{"sink", item.sink.value()},
                {"physical_slot", item.physical_slot},
                {"expected_arc", item.expected_arc.value()},
                {"replacement_arc", item.replacement_arc.value()}};
  };
  const auto payload_change = [](const ArcPayloadChangeDescriptor& item) {
    return Json{{"id", item.id.value()},
                {"expected", arcPayload(item.expected)},
                {"replacement", arcPayload(item.replacement)}};
  };
  const auto deactivate = [](const LogicalArcDeactivate& item) {
    return Json{{"id", item.id.value()},
                {"key", arcKey(item.key)},
                {"source", item.source.value()},
                {"sink", item.sink.value()},
                {"reason", deactivationReason(item.reason)},
                {"expected_active", item.expected_active}};
  };
  const auto ids = [](const auto& values) {
    return arrayOf(values, [](csr::ArcId id) { return Json(id.value()); });
  };
  const auto aux_row = [&](const AuxOutRowDescriptor& item) {
    return Json{{"source", item.source.value()},
                {"expected_arc_ids", ids(item.expected_arc_ids)},
                {"replacement_arc_ids", ids(item.replacement_arc_ids)}};
  };
  const auto appended_out = [&](const AppendedBaseOutRowDescriptor& item) {
    return Json{{"source", item.source.value()},
                {"arc_ids", ids(item.arc_ids)}};
  };
  const auto affected_out = [&](const AffectedBaseOutRowDescriptor& item) {
    return Json{{"source", item.source.value()},
                {"expected_arc_ids", ids(item.expected_arc_ids)},
                {"replacement_arc_ids", ids(item.replacement_arc_ids)}};
  };
  const auto affected_in = [&](const AffectedBaseInRowDescriptor& item) {
    return Json{{"sink", item.sink.value()},
                {"expected_arc_ids", ids(item.expected_arc_ids)},
                {"replacement_arc_ids", ids(item.replacement_arc_ids)}};
  };
  const auto index_delta = [](const ArcGroupIndexDelta& item) {
    return Json{{"id", item.id.value()},
                {"ref", stableArc(item.ref)},
                {"expected_active_key",
                 optionalValue(item.expected_active_key, arcKey)},
                {"replacement_active_key",
                 optionalValue(item.replacement_active_key, arcKey)}};
  };
  return {{"schema", value.schema},
          {"session_token", value.session_token.value()},
          {"expected_generation", value.expected_generation.value()},
          {"target_generation", value.target_generation.value()},
          {"before", highWater(value.before)},
          {"after", highWater(value.after)},
          {"node_appends", arrayOf(value.node_appends, node_append)},
          {"arc_appends", arrayOf(value.arc_appends, arc_append)},
          {"node_payload_changes",
           arrayOf(value.node_payload_changes, node_change)},
          {"removes", arrayOf(value.removes, remove)},
          {"changes", arrayOf(value.changes, change)},
          {"reverse_changes", arrayOf(value.reverse_changes, reverse)},
          {"payload_changes",
           arrayOf(value.payload_changes, payload_change)},
          {"logical_deactivations",
           arrayOf(value.logical_deactivations, deactivate)},
          {"aux_rows", arrayOf(value.aux_rows, aux_row)},
          {"appended_base_out_rows",
           arrayOf(value.appended_base_out_rows, appended_out)},
          {"affected_base_out_rows",
           arrayOf(value.affected_base_out_rows, affected_out)},
          {"affected_base_in_rows",
           arrayOf(value.affected_base_in_rows, affected_in)},
          {"reference_payload", referenceDelta(value.reference_payload)},
          {"arc_group_index_delta",
           arrayOf(value.arc_group_index_delta, index_delta)}};
}

Json semanticPlan(const timing_edit::DesignEditPlan& value)
{
  const auto edit = [](const timing_edit::BufferInsertionMutation& item) {
    return Json{{"original_net", stableNet(item.original_net)},
                {"moved_sinks", arrayOf(item.moved_sinks, stableNode)},
                {"library_cell", libraryCellRef(item.library_cell)},
                {"placement_inputs", placement(item.placement_inputs)},
                {"rc_inputs", rcInputs(item.rc_inputs)},
                {"creation_token", creationToken(item.creation_token)}};
  };
  return {{"schema_version", value.schema_version},
          {"baseline", baseline(value.baseline)},
          {"expected_generation", value.expected_generation.value()},
          {"edits", arrayOf(value.edits, edit)}};
}

}  // namespace ista::timing_dual_csr::test::semantic_json
