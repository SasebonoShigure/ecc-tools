// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferTraceReceiptJson.hpp"

#include <type_traits>
#include <variant>

#include "BufferTraceCsrJson.hpp"
#include "TimingEditSemanticJson.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::buffer_trace::test {
namespace {

using Json = nlohmann::json;

using timing_edit::test::semantic_json::arcKey;
using timing_edit::test::semantic_json::arrayOf;
using timing_edit::test::semantic_json::creationToken;
using timing_edit::test::semantic_json::librarySignature;
using timing_edit::test::semantic_json::nodeKey;
using timing_edit::test::semantic_json::stableArc;
using timing_edit::test::semantic_json::stableNet;
using timing_edit::test::semantic_json::stableNode;
using timing_edit::test::semantic_json::tableSignature;

template <typename... Visitors>
struct Overloaded : Visitors...
{
  using Visitors::operator()...;
};
template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

Json libraryCell(const timing_edit::LibraryCellRef& value)
{
  return {{"library_name", value.library_name},
          {"cell_name", value.cell_name},
          {"input_port", value.input_port},
          {"output_port", value.output_port},
          {"ordered_arc_signatures",
           arrayOf(value.ordered_arc_signatures, librarySignature)}};
}

Json mutation(const timing_edit::BufferInsertionMutation& value)
{
  const Json placement = std::visit(
      Overloaded{
          [](const timing_edit::UnplacedInputs&) {
            return Json{{"kind", "unplaced"}};
          },
          [](const timing_edit::PlacedInputs& item) {
            return Json{{"kind", "placed"},
                        {"x_microns", item.x_microns},
                        {"y_microns", item.y_microns}};
          }},
      value.placement_inputs);
  const Json rc = std::visit(
      Overloaded{
          [](const timing_edit::NoParasiticRcInputs&) {
            return Json{{"kind", "no_parasitic"}};
          },
          [](const timing_edit::ParasiticRcInputs& item) {
            return Json{{"kind", "parasitic"},
                        {"policy", timing_edit::token(item.parasitic_policy)}};
          }},
      value.rc_inputs);
  return {{"original_net", stableNet(value.original_net)},
          {"moved_sinks", arrayOf(value.moved_sinks, stableNode)},
          {"library_cell", libraryCell(value.library_cell)},
          {"placement_inputs", placement},
          {"rc_inputs", rc},
          {"creation_token", creationToken(value.creation_token)}};
}

Json graphOperation(const timing_edit::TimingGraphEdit& value)
{
  return std::visit(
      Overloaded{
          [](const timing_edit::AddInstance& item) {
            return Json{{"kind", "add_instance"},
                        {"creation_token", creationToken(item.creation_token)},
                        {"library_cell", libraryCell(item.library_cell)}};
          },
          [](const timing_edit::RemoveInstance& item) {
            return Json{{"kind", "remove_instance"},
                        {"creation_token", creationToken(item.creation_token)}};
          },
          [](const timing_edit::AddTimingPoint& item) {
            return Json{{"kind", "add_timing_point"},
                        {"node", stableNode(item.node)},
                        {"direction", csr::token(item.direction)}};
          },
          [](const timing_edit::RemoveTimingPoint& item) {
            return Json{{"kind", "remove_timing_point"},
                        {"node", stableNode(item.node)}};
          },
          [](const timing_edit::AddCellArc& item) {
            return Json{{"kind", "add_cell_arc"},
                        {"arc", stableArc(item.arc)}};
          },
          [](const timing_edit::RemoveCellArc& item) {
            return Json{{"kind", "remove_cell_arc"},
                        {"arc", stableArc(item.arc)}};
          },
          [](const timing_edit::AddNetArc& item) {
            return Json{{"kind", "add_net_arc"},
                        {"arc", stableArc(item.arc)}};
          },
          [](const timing_edit::RemoveNetArc& item) {
            return Json{{"kind", "remove_net_arc"},
                        {"arc", stableArc(item.arc)}};
          },
          [](const timing_edit::ReconnectSink& item) {
            return Json{{"kind", "reconnect_sink"},
                        {"sink", stableNode(item.sink)},
                        {"old_net", stableNet(item.old_net)},
                        {"new_net", stableNet(item.new_net)}};
          },
          [](const timing_edit::InvalidateLoad& item) {
            return Json{{"kind", "invalidate_load"},
                        {"net", stableNet(item.net)}};
          },
          [](const timing_edit::InvalidateParasitic& item) {
            return Json{{"kind", "invalidate_parasitic"},
                        {"net", stableNet(item.net)}};
          },
          [](const timing_edit::InvalidateArcDelay& item) {
            return Json{{"kind", "invalidate_arc_delay"},
                        {"arc", stableArc(item.arc)}};
          }},
      value);
}

Json delta(const timing_edit::TimingGraphDelta& value)
{
  return {{"schema_version", value.schema_version},
          {"operations", arrayOf(value.operations, graphOperation)}};
}

Json materialization(const timing_edit::TokenMaterialization& value)
{
  return {{"creation_token", creationToken(value.creation_token)},
          {"instance_name", value.instance_name},
          {"output_net_name", value.output_net_name},
          {"input_pin_name", value.input_pin_name},
          {"output_pin_name", value.output_pin_name}};
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

Json preState(const timing_edit::CanonicalPreStateEvidence& value)
{
  return {{"instances", value.instances},
          {"nets", value.nets},
          {"nodes", arrayOf(value.nodes, nodeKey)},
          {"arcs", arrayOf(value.arcs, arcKey)}};
}

Json baseline(const timing_edit::BaselineSemanticIdentity& value)
{
  return {{"design_name", value.design_name},
          {"fingerprints", fingerprints(value.fingerprints)},
          {"canonical_pre_state", preState(value.canonical_pre_state)}};
}

Json identity(const timing_edit::CommittedEditIdentity& value)
{
  return {{"generation", value.generation.value()},
          {"current_fingerprints", fingerprints(value.current_fingerprints)},
          {"canonical_pre_state", preState(value.canonical_pre_state)}};
}

Json plan(const timing_edit::DesignEditPlan& value)
{
  return {{"schema_version", value.schema_version},
          {"baseline", baseline(value.baseline)},
          {"expected_generation", value.expected_generation.value()},
          {"edits", arrayOf(value.edits, mutation)}};
}

Json resolver(const timing_edit::TimingEditResolverState& value)
{
  const auto binding = [](const timing_edit::NativeSlotBinding& item) {
    return Json{{"slot", item.slot.value()},
                {"native_name", item.native_name},
                {"tombstone", item.tombstone}};
  };
  Json nodes = Json::array();
  for (const auto& [ref, item] : value.nodeBindings()) {
    nodes.push_back({{"ref", stableNode(ref)}, {"binding", binding(item)}});
  }
  Json nets = Json::array();
  for (const auto& [ref, item] : value.netBindings()) {
    nets.push_back({{"ref", stableNet(ref)}, {"binding", binding(item)}});
  }
  Json arcs = Json::array();
  for (const auto& [ref, item] : value.arcBindings()) {
    arcs.push_back({{"ref", stableArc(ref)}, {"binding", binding(item)}});
  }
  Json materializations = Json::array();
  for (const auto& [token_value, item] : value.materializations()) {
    materializations.push_back({{"creation_token", creationToken(token_value)},
                                {"value", materialization(item)}});
  }
  return {{"high_water", {{"node_slots", value.highWater().node_slots},
                            {"net_slots", value.highWater().net_slots},
                            {"arc_slots", value.highWater().arc_slots}}},
          {"node_bindings", std::move(nodes)},
          {"net_bindings", std::move(nets)},
          {"arc_bindings", std::move(arcs)},
          {"materializations", std::move(materializations)}};
}

Json nativeDatabase(const NativeDatabaseFunctionalReceipt& value)
{
  const auto instance = [](const NativeInstanceFunctionalRecord& item) {
    return Json{{"name", item.name},
                {"cell", item.cell},
                {"ordered_pins", item.ordered_pins},
                {"sequential", item.sequential},
                {"clock_gating", item.clock_gating},
                {"io_cell", item.io_cell}};
  };
  const auto pin = [](const NativePinFunctionalRecord& item) {
    return Json{{"full_name", item.full_name},
                {"pin_name", item.pin_name},
                {"instance_name", item.instance_name},
                {"net_name", item.net_name},
                {"direction", item.direction},
                {"port", item.port}};
  };
  const auto net = [](const NativeNetFunctionalRecord& item) {
    return Json{{"name", item.name},
                {"ordered_drivers", item.ordered_drivers},
                {"ordered_loads", item.ordered_loads},
                {"ordered_pins", item.ordered_pins}};
  };
  return {{"design_name", value.design_name},
          {"instances", arrayOf(value.instances, instance)},
          {"pins", arrayOf(value.pins, pin)},
          {"nets", arrayOf(value.nets, net)}};
}

Json batch(const FunctionalBatchReceipt& value)
{
  return {{"batch_ordinal", value.batch_ordinal},
          {"expected_generation", value.expected_generation.value()},
          {"target_generation", value.target_generation.value()},
          {"normalized_edits", arrayOf(value.normalized_edits, mutation)},
          {"forward", delta(value.forward)},
          {"reverse", delta(value.reverse)},
          {"materializations", arrayOf(value.materializations, materialization)},
          {"post_identity", identity(value.post_identity)},
          {"csr_input", csrReferenceInputToJson(value.csr_input)},
          {"csr_timing", csrTimingResultToJson(value.csr_timing)},
          {"native_database", nativeDatabase(value.native_database)}};
}

Json header(const ReplayHeader& value)
{
  return {{"schema_version", value.schema_version},
          {"selection_mode", token(value.selection_mode)},
          {"semantic_workload_kind", token(value.workload_kind)},
          {"profile_id", token(value.profile_id)},
          {"comparability_status", token(value.comparability_status)},
          {"comparability_reason",
           value.comparability_reason
               ? Json(token(*value.comparability_reason))
               : Json(nullptr)}};
}

}  // namespace

Json replayReceiptToJson(const ReplayReceipt& receipt)
{
  return {{"header", header(receipt.header)},
          {"prefix_batches", receipt.prefix_batches},
          {"batches", arrayOf(receipt.batches, batch)},
          {"committed_identity", identity(receipt.committed_identity)},
          {"cumulative_trace", arrayOf(receipt.cumulative_trace.batches, plan)},
          {"resolver_state", resolver(receipt.resolver_state)},
          {"final_database", nativeDatabase(receipt.final_database)},
          {"final_csr_input", csrReferenceInputToJson(receipt.final_csr_input)},
          {"final_csr_timing", csrTimingResultToJson(receipt.final_csr_timing)}};
}

}  // namespace ista::buffer_trace::test
