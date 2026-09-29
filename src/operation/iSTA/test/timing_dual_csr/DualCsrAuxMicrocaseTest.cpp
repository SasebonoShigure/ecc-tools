// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceValidator.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrUpdaterInternal.hpp"
#include "DualCsrTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrTimingResultValidator.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::timing_dual_csr::test {
namespace {

DualCsrBatchDescriptor auxOverflowBatch(const DualCsrState& state)
{
  auto descriptor = descriptorFor(state, 1, 1);
  const auto appended_node = appendNode(
      2, "aux_sink", AppendedPortRole::kSink);
  descriptor.node_appends = {appended_node};
  descriptor.arc_appends = {appendArc(
      1, csr::NodeId{0}, csr::NodeId{2}, state.nodes[0].key,
      appended_node.key, 0)};
  descriptor.aux_rows = {{csr::NodeId{0}, {}, {csr::ArcId{1}}}};
  completeReferencePayload(descriptor, state);

  csr::PortConstraintModel aux_constraint;
  aux_constraint.map_key = "aux_sink";
  aux_constraint.embedded_port_name = "aux_sink";
  aux_constraint.resolved_node = csr::NodeId{2};
  aux_constraint.output_delay[0] = 2.0;
  auto port_constraints = state.reference_payload.port_constraints;
  port_constraints.insert(port_constraints.begin(), aux_constraint);
  replaceReference(descriptor.reference_payload.port_constraints,
                   state.reference_payload.port_constraints,
                   std::move(port_constraints));

  require(descriptor.reference_payload.endpoint_required.has_value(),
          "Aux endpoint append did not declare endpoint-required payload");
  auto& endpoint_required
      = descriptor.reference_payload.endpoint_required->replacement;
  const auto endpoint = std::ranges::find(
      endpoint_required, csr::NodeId{2},
      &csr::EndpointRequiredModel::node);
  require(endpoint != endpoint_required.end(),
          "Aux endpoint has no endpoint-required row");
  endpoint->output_delay[0] = 2.0;
  endpoint->clock_period = 0.0;
  endpoint->clock_name = "clk";
  return descriptor;
}

DualCsrBatchDescriptor auxClearBatch(const DualCsrState& state)
{
  auto descriptor = descriptorFor(state, 1, 1);
  auto appended_node = appendNode(
      3, "new_source", AppendedPortRole::kSource);
  appended_node.payload.net_name = "n2";
  descriptor.node_appends = {appended_node};
  auto appended_arc = appendArc(
      2, csr::NodeId{3}, csr::NodeId{2}, appended_node.key,
      state.nodes[2].key, 0);
  appended_arc.payload.key.base.owner_name = "n2";
  appended_arc.payload.output_load = csr::test::outputLoad(0.0);
  appended_arc.key = appended_arc.payload.key;
  descriptor.arc_appends = {std::move(appended_arc)};
  descriptor.node_payload_changes = {{
      csr::NodeId{2}, state.nodes[2].key, state.nodes[2].net_name, "n2"}};
  const auto incoming_slot = state.base_in.row_offsets[2];
  descriptor.reverse_changes = {{csr::NodeId{2}, incoming_slot,
                                 csr::ArcId{1}, csr::ArcId{2}}};
  descriptor.logical_deactivations = {
      deactivate(state, csr::ArcId{1},
                 LogicalArcDeactivateReason::kAuxCleared)};
  descriptor.aux_rows = {{csr::NodeId{0}, {csr::ArcId{1}}, {}}};
  replaceRoots(descriptor, state,
               {csr::NodeId{0}, csr::NodeId{3}});
  completeReferencePayload(descriptor, state);
  return descriptor;
}

csr::CsrComputationWithTelemetry runCpu(const DualCsrState& state)
{
  csr::CsrCpuReferenceBackend backend;
  DualCsrEdgeViewImpl edges(state);
  DualCsrTimingPayloadViewImpl payloads(state);
  return backend.computeWithTelemetry(edges, payloads);
}

void copyReferencePayload(const DualCsrReferencePayload& source,
                          csr::CsrReferenceInput& target)
{
  target.schema_version = source.schema_version;
  target.source_identity = source.source_identity;
  target.analysis_context = source.analysis_context;
  target.analysis_order = source.analysis_order;
  target.path_source_order = source.path_source_order;
  target.transition_order = source.transition_order;
  target.instances = source.instances;
  target.ordered_root_ids = source.ordered_root_ids;
  target.ordered_start_ids = source.ordered_start_ids;
  target.ordered_end_ids = source.ordered_end_ids;
  target.clock_tags = source.clock_tags;
  target.clocks = source.clocks;
  target.clock_owner_by_node = source.clock_owner_by_node;
  target.port_constraints = source.port_constraints;
  target.case_analysis_constraints = source.case_analysis_constraints;
  target.timing_check_endpoints = source.timing_check_endpoints;
  target.referenced_library_cells = source.referenced_library_cells;
  target.parasitics = source.parasitics;
  target.start_seeds = source.start_seeds;
  target.endpoint_required = source.endpoint_required;
  target.cell_pin_capacitances = source.cell_pin_capacitances;
  target.unsupported_reasons = source.unsupported_reasons;
}

csr::CsrReferenceInput logicalActiveInput(
    const DualCsrState& state,
    std::optional<csr::ArcId> omitted = std::nullopt)
{
  csr::CsrReferenceInput input;
  copyReferencePayload(state.reference_payload, input);
  input.nodes = state.nodes;
  for (const auto& node : input.nodes)
    input.node_ids_by_key.push_back({node.key, node.id});

  std::vector<std::vector<csr::ArcId>> outgoing(input.nodes.size());
  std::vector<std::vector<csr::ArcId>> incoming(input.nodes.size());
  for (std::size_t stable_id = 0; stable_id < state.arc_slots.size();
       ++stable_id) {
    const csr::ArcId source_id{stable_id};
    const auto& slot = state.arc_slots[stable_id];
    if (!slot.active || omitted == source_id)
      continue;
    const auto& payload = slot.payload;
    csr::ArcRecord arc;
    arc.id = csr::ArcId{input.arcs.size()};
    arc.key = payload.key;
    arc.source = payload.source;
    arc.sink = payload.sink;
    arc.kind = payload.kind;
    arc.native_traversal_rank = input.arcs.size();
    arc.is_clock_arc = payload.is_clock_arc;
    arc.is_disabled = payload.is_disabled;
    arc.is_loop_disabled = payload.is_loop_disabled;
    arc.eligibility = payload.eligibility;
    arc.transfer = payload.transfer;
    arc.output_load = payload.output_load;
    input.arc_ids_by_key.push_back({arc.key, arc.id});
    outgoing.at(arc.source.value()).push_back(arc.id);
    incoming.at(arc.sink.value()).push_back(arc.id);
    input.arcs.push_back(std::move(arc));
  }

  input.out_row_offsets.push_back(0);
  input.in_row_offsets.push_back(0);
  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    input.out_arc_ids.insert(input.out_arc_ids.end(),
                             outgoing[node].begin(),
                             outgoing[node].end());
    input.out_row_offsets.push_back(input.out_arc_ids.size());
    input.in_arc_ids.insert(input.in_arc_ids.end(),
                            incoming[node].begin(), incoming[node].end());
    input.in_row_offsets.push_back(input.in_arc_ids.size());
  }
  return input;
}

csr::CsrComputationWithTelemetry runFlat(
    const csr::CsrReferenceInput& input)
{
  csr::CsrCpuReferenceBackend backend;
  csr::FlatCsrEdgeView edges(input);
  csr::FlatTimingPayloadView payloads(input);
  return backend.computeWithTelemetry(edges, payloads);
}

const csr::CsrTimingResult& requireFlatTiming(
    const csr::CsrComputationWithTelemetry& output,
    const std::string& name)
{
  const auto* timing
      = std::get_if<csr::CsrTimingResult>(&output.computation.outcome);
  require(timing != nullptr, name + " flat oracle did not compute");
  return *timing;
}

FunctionalSemanticReceipt requireFunctionalReceipt(
    const DualCsrState& state, const DualCsrTimingResult& timing,
    const std::string& name)
{
  const auto projected
      = projectDualCsrFunctionalSemanticReceipt(state, timing);
  require(projected.success(), name + " receipt projection failed");
  return *projected.receipt;
}

void requireExactFlatOracle(const DualCsrState& state,
                            const DualCsrTimingResult& timing,
                            const std::string& name)
{
  const auto input = logicalActiveInput(state);
  require(csr::validateReferenceInputIntrinsic(input).empty(),
          name + " logical flat input is invalid");
  require(requireFlatTiming(runFlat(input), name) == timing.values,
          name + " differs from the independent flat CPU oracle");
}

void requireSemanticFlatOracle(const DualCsrState& state,
                               const DualCsrTimingResult& timing,
                               const std::string& name)
{
  auto input = logicalActiveInput(state);
  input.source_identity = csr::makeReferenceInputIdentity(
      input, state.session_token.value(), 0);
  require(csr::validateReferenceInputIntrinsic(input).empty(),
          name + " compact flat input is invalid");
  auto compact = DualCsrBaselineFactory::create(
      input, state.session_token,
      timing_edit::AppendOnlyHighWater{input.nodes.size(),
                                       state.high_water.net_slots,
                                       input.arcs.size()});
  require(compact.success(), name + " compact oracle state failed");
  const auto flat_output = runFlat(input);
  const auto& values = requireFlatTiming(flat_output, name);
  DualCsrTimingResult compact_timing{
      values};
  require(validateDualCsrTimingResult(
              *compact.state, compact_timing).empty(),
          name + " compact oracle timing is invalid");
  require(requireFunctionalReceipt(state, timing, name)
              == requireFunctionalReceipt(*compact.state,
                                          compact_timing, name),
          name + " differs semantically from the compact flat oracle");
}

void requireAuxOmissionChangesResult(const DualCsrState& state,
                                     const DualCsrTimingResult& timing)
{
  const auto omitted = runFlat(
      logicalActiveInput(state, csr::ArcId{1}));
  const auto* values
      = std::get_if<csr::CsrTimingResult>(&omitted.computation.outcome);
  require(values == nullptr || *values != timing.values,
          "omitting the only Aux edge preserved the timing receipt");
}

DualCsrTimingResult requireValidTiming(
    const DualCsrState& state, const csr::CsrComputationWithTelemetry& output,
                        const std::string& name)
{
  const auto* timing
      = std::get_if<csr::CsrTimingResult>(&output.computation.outcome);
  require(timing != nullptr, name + " did not produce a timing result");
  const DualCsrTimingResult dual{*timing};
  const auto violations = validateDualCsrTimingResult(state, dual);
  require(violations.empty(),
          name + " failed Dual timing validation at "
              + (violations.empty() ? std::string{}
                                    : violations.front().field));
  return dual;
}

std::size_t nodeLane(std::size_t node, std::size_t analysis,
                     std::size_t transition)
{
  return ((node * csr::kAnalysisCount) + analysis)
         * csr::kTransitionCount
         + transition;
}

std::size_t pathRow(std::size_t node, std::size_t analysis,
                    std::size_t source, std::size_t transition)
{
  return (((node * csr::kAnalysisCount) + analysis)
          * csr::kPathSourceCount
          + source)
         * csr::kTransitionCount
         + transition;
}

std::size_t requireAuxNumericResult(const DualCsrTimingResult& timing)
{
  const auto& values = timing.values;
  constexpr std::size_t kAuxNode = 2;
  require(values.nodes.arrivals.presence[kAuxNode] == 1
              && values.nodes.arrivals.values[kAuxNode] == 1.0,
          "Aux arc did not produce the endpoint scalar arrival");
  require(values.nodes.predecessor_nodes.presence[kAuxNode] == 1
              && values.nodes.predecessor_arcs.presence[kAuxNode] == 1
              && values.nodes.predecessor_nodes.values[kAuxNode]
                     == csr::NodeId{0}
              && values.nodes.predecessor_arcs.values[kAuxNode]
                     == csr::ArcId{1},
          "Aux endpoint scalar predecessor is not the Aux arc");
  require(values.nodes.requireds.values[kAuxNode] == -2.0
              && values.nodes.requireds.values[0] == -2.0
              && values.nodes.slacks.values[kAuxNode] == -3.0,
          "Aux endpoint required/slack did not propagate through AuxOut");

  const std::size_t slew_lane = nodeLane(kAuxNode, 0, 0);
  require(values.data_slews.presence[slew_lane] == 1
              && values.data_slews.values[slew_lane] == 0.2,
          "Aux arc did not produce the endpoint data slew");

  const std::size_t row = pathRow(kAuxNode, 0, 0, 0);
  require(values.paths.row_offsets[row + 1]
              == values.paths.row_offsets[row] + 1,
          "Aux endpoint max/input/rise path row is not unique");
  const std::size_t entry
      = static_cast<std::size_t>(values.paths.row_offsets[row]);
  require(values.paths.arrivals.presence[entry] == 1
              && values.paths.arrivals.values[entry] == 1.0
              && values.paths.slews.values[entry] == 0.2
              && values.paths.launches.values[entry] == 0.0
              && values.paths.start_nodes.values[entry] == csr::NodeId{0},
          "Aux endpoint path values do not preserve the input seed");
  require(values.paths.predecessor_nodes.presence[entry] == 1
              && values.paths.predecessor_arcs.presence[entry] == 1
              && values.paths.predecessor_arc_delays.presence[entry] == 1
              && values.paths.predecessor_transitions.presence[entry] == 1
              && values.paths.predecessor_nodes.values[entry]
                     == csr::NodeId{0}
              && values.paths.predecessor_arcs.values[entry]
                     == csr::ArcId{1}
              && values.paths.predecessor_arc_delays.values[entry] == 0.0
              && values.paths.predecessor_transitions.values[entry]
                     == csr::Transition::kRise,
          "Aux endpoint path predecessor is not the Aux arc lane");
  return entry;
}

void requireRejected(const DualCsrState& state,
                     const DualCsrTimingResult& timing,
                     const std::string& message)
{
  require(!validateDualCsrTimingResult(state, timing).empty(), message);
}

void paperAuxOverflowTrace()
{
  const DualCsrState baseline = baselineState();
  const auto baseline_out_offsets = baseline.base_out.row_offsets;
  const auto baseline_out_slots = baseline.base_out.physical_slots;
  DualCsrState first = apply(baseline,
                                   auxOverflowBatch(baseline));

  requireOffsetPrefix(baseline_out_offsets, first.base_out.row_offsets,
                      "paper_aux_overflow batch1 BaseOut");
  require(first.base_out.physical_slots == baseline_out_slots,
          "old-source add-only batch expanded an old BaseOut row");
  require(first.aux_out.row_sources
              == std::vector<csr::NodeId>{csr::NodeId{0}}
              && first.aux_out.row_arcs.front()
                     == std::vector<csr::ArcId>{csr::ArcId{1}}
              && first.aux_out.source_to_aux_row[0] == 0,
          "paper_aux_overflow batch1 did not build packed AuxOut");

  DualCsrEdgeViewImpl first_edges(first);
  std::uint64_t base_visits = 0;
  std::uint64_t aux_visits = 0;
  const auto out_status = first_edges.forEachOutgoing(
      csr::NodeId{0}, [&](const csr::EdgeEntry& entry) {
        if (entry.storage_kind == csr::EdgeStorageKind::kBaseOut)
          ++base_visits;
        if (entry.storage_kind == csr::EdgeStorageKind::kAuxOut)
          ++aux_visits;
        return csr::EdgeVisitResult::continueVisit();
      });
  require(out_status.control == csr::EdgeVisitControl::kContinue
              && base_visits == 1 && aux_visits == 1,
          "Dual outgoing traversal did not merge BaseOut and AuxOut");
  std::vector<csr::EdgeEntry> incoming;
  const auto in_status = first_edges.forEachIncoming(
      csr::NodeId{2}, [&](const csr::EdgeEntry& entry) {
        incoming.push_back(entry);
        return csr::EdgeVisitResult::continueVisit();
      });
  require(in_status.control == csr::EdgeVisitControl::kContinue
              && incoming
                     == std::vector<csr::EdgeEntry>{{
                         csr::ArcId{1}, csr::EdgeStorageKind::kBaseIn}},
          "Aux outgoing arc was not transposed into appended BaseIn row");

  const auto first_cpu = runCpu(first);
  const DualCsrTimingResult first_timing
      = requireValidTiming(first, first_cpu, "paper_aux_overflow batch1");
  const std::size_t aux_path_entry = requireAuxNumericResult(first_timing);
  requireExactFlatOracle(first, first_timing, "paper_aux_overflow batch1");
  requireAuxOmissionChangesResult(first, first_timing);

  auto nan_arc_canary = first_timing;
  nan_arc_canary.values.arcs.delays.values[1]
      = std::numeric_limits<double>::quiet_NaN();
  requireRejected(first, nan_arc_canary,
                  "validator accepted a present NaN Arc delay");
  auto inf_slew_canary = first_timing;
  inf_slew_canary.values.data_slews.values[nodeLane(2, 0, 0)]
      = std::numeric_limits<double>::infinity();
  requireRejected(first, inf_slew_canary,
                  "validator accepted a present infinite data slew");
  auto inf_path_canary = first_timing;
  inf_path_canary.values.paths.arrivals.values[aux_path_entry]
      = std::numeric_limits<double>::infinity();
  requireRejected(first, inf_path_canary,
                  "validator accepted a present infinite path arrival");
  auto mismatched_presence_canary = first_timing;
  mismatched_presence_canary.values.nodes.predecessor_arcs.presence[2] = 0;
  mismatched_presence_canary.values.nodes.predecessor_arcs.values[2]
      = csr::ArcId{};
  requireRejected(first, mismatched_presence_canary,
                  "validator accepted mismatched node/Arc predecessor presence");
  auto wrong_endpoint_canary = first_timing;
  wrong_endpoint_canary.values.nodes.predecessor_arcs.values[2]
      = csr::ArcId{0};
  requireRejected(first, wrong_endpoint_canary,
                  "validator accepted an Arc with the wrong sink endpoint");
  auto wrong_path_endpoint_canary = first_timing;
  wrong_path_endpoint_canary.values.paths.predecessor_arcs.values[
      aux_path_entry] = csr::ArcId{0};
  requireRejected(first, wrong_path_endpoint_canary,
                  "validator accepted a path Arc with the wrong sink endpoint");
  require(first_cpu.telemetry.levelization.aux_out > 0
              && first_cpu.telemetry.arc_initialization.aux_out > 0
              && first_cpu.telemetry.forward_propagation.base_in > 0
              && first_cpu.telemetry.required_propagation.aux_out > 0,
          "generic CPU path did not consume the expected incoming/Aux storage");
  const auto first_projection = projectDualCsrState(first);
  require(first_projection.success()
              && first_projection.projection->active_arcs.size() == 2,
          "batch1 functional projection lost an active arc");

  const auto first_out_offsets = first.base_out.row_offsets;
  const auto first_in_offsets = first.base_in.row_offsets;
  first.aux_out.row_arcs.front().reserve(32);
  const auto* retained_row = first.aux_out.row_arcs.front().data();
  const auto retained_capacity = first.aux_out.row_arcs.front().capacity();
  {
    detail::PreparedDualCsrGraphPatch patch(first, auxClearBatch(first));
    patch.apply();
    require(first.aux_out.row_arcs.front().empty()
                && first.aux_out.row_arcs.front().data() == retained_row,
            "clearing an Aux row replaced its storage");
    patch.rollback();
    require(first.aux_out.row_arcs.front() == std::vector<csr::ArcId>{csr::ArcId{1}}
                && first.aux_out.row_arcs.front().data() == retained_row
                && first.aux_out.row_arcs.front().capacity() == retained_capacity,
            "restoring an Aux row lost its allocation or logical values");
  }
  const DualCsrState second = apply(first, auxClearBatch(first));
  require(second.nodes[2].net_name == "n2"
              && second.arc_slots[1].payload.key.base.owner_name == "n1"
              && !second.arc_slots[1].active
              && validateDualCsrState(second).empty(),
          "reconnect did not preserve the inactive historical Arc owner");
  requireOffsetPrefix(first_out_offsets, second.base_out.row_offsets,
                      "paper_aux_overflow batch2 BaseOut");
  requireOffsetPrefix(first_in_offsets, second.base_in.row_offsets,
                      "paper_aux_overflow batch2 BaseIn");
  require(second.aux_out.row_sources == first.aux_out.row_sources
              && second.aux_out.row_arcs.front().empty()
              && second.aux_out.source_to_aux_row[0] == 0,
          "batch2 did not clear the affected AuxOut row/mapping");
  require(!second.arc_slots[1].active && second.arc_slots[2].active
              && second.base_in.physical_slots[
                     second.base_in.row_offsets[2]]
                     == std::optional<csr::ArcId>{csr::ArcId{2}},
          "batch2 did not pair Aux deactivation with BaseIn reverse change");
  const auto second_cpu = runCpu(second);
  const DualCsrTimingResult second_timing
      = requireValidTiming(second, second_cpu, "paper_aux_overflow batch2");
  requireSemanticFlatOracle(second, second_timing,
                            "paper_aux_overflow batch2");
  auto inactive_predecessor_canary = second_timing;
  inactive_predecessor_canary.values.nodes.predecessor_arcs.values[2]
      = csr::ArcId{1};
  requireRejected(second, inactive_predecessor_canary,
                  "validator accepted an inactive stable predecessor ArcId");
  auto presence_canary = second_timing;
  presence_canary.values.arcs.delays.presence[1] = 1;
  require(!validateDualCsrTimingResult(second, presence_canary).empty(),
          "validator accepted a present delay for an inactive ArcId");
  auto negative_zero_canary = second_timing;
  negative_zero_canary.values.arcs.delays.values[1] = -0.0;
  require(
      !validateDualCsrTimingResult(second, negative_zero_canary).empty(),
      "validator accepted noncanonical negative zero in an absent lane");
  const auto second_projection = projectDualCsrState(second);
  require(second_projection.success()
              && second_projection.projection->active_arcs.size() == 2,
          "batch2 functional projection has the wrong active set");
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_dual_csr::test::paperAuxOverflowTrace();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
