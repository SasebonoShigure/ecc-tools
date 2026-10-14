// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CsrModelTestMutation.hpp"
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "CsrFifoLevelization.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr::test {
namespace {

void baselineFactoryContracts()
{
  const auto input = baselineInput();
  const auto attempt = DualCsrBaselineFactory::create(
      input, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{input.nodes.size(), 0,
                                       input.arcs.size()});
  require(attempt.success(), "strict-valid baseline was rejected");
  const auto& state = *attempt.state;
  require(validateDualCsrState(state).empty(),
          "factory emitted an invalid Dual state");
  require(state.base_out.physical_slots.size() == 1
              && state.base_in.physical_slots.size() == 1,
          "baseline did not materialize both CSR directions");
  require(state.telemetry.base_physical_edges
              == state.base_out.physical_slots.size()
              && state.telemetry.base_active_edges == 1,
          "baseline telemetry is not outgoing-Base scoped");

  auto fifo_input = csr::test::cellInput();
  auto native_first = fifo_input.arcs.front();
  native_first.id = csr::ArcId{1};
  auto canonical_first = native_first;
  canonical_first.id = csr::ArcId{0};
  canonical_first.native_traversal_rank = 1;
  canonical_first.native_cell_arc_ordinal = fifo_input.referenced_library_cells[0].cell_arcs.size();
  canonical_first.key.base.library_arcs[0].arc_index = 0;
  ista::csr::test::mutateChildren(canonical_first.transfer, [&](auto& children) { children[0].signature.arc_index = 0; });
  fifo_input.arcs = {canonical_first, native_first};
  fifo_input.arc_ids_by_key = {{canonical_first.key, canonical_first.id},
                               {native_first.key, native_first.id}};
  fifo_input.referenced_library_cells[0].cell_arcs.push_back(
      {"A", "Y", canonical_first.transfer});
  fifo_input.out_row_offsets = {0, 2, 2};
  fifo_input.out_arc_ids = {csr::ArcId{1}, csr::ArcId{0}};
  fifo_input.in_row_offsets = {0, 0, 2};
  fifo_input.in_arc_ids = {csr::ArcId{1}, csr::ArcId{0}};
  csr::test::refreshIdentity(fifo_input, kSessionValue);
  const auto fifo_attempt = DualCsrBaselineFactory::create(
      fifo_input, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{fifo_input.nodes.size(), 0,
                                       fifo_input.arcs.size()});
  require(fifo_attempt.success(),
          "reverse-order generation-zero baseline was rejected");
  const auto& fifo_state = *fifo_attempt.state;
  require(fifo_state.base_out.physical_slots.size() == 2
              && fifo_state.base_out.physical_slots[0]
                     == csr::ArcId{1}
              && fifo_state.base_out.physical_slots[1]
                     == csr::ArcId{0}
              && fifo_state.base_in.physical_slots
                     == fifo_state.base_out.physical_slots
              && csr::arcKeyLess(fifo_state.arc_slots[0].payload.key,
                                 fifo_state.arc_slots[1].payload.key),
          "baseline factory reordered input CSR rows by ArcKey");

  auto invalid = input;
  invalid.out_row_offsets = {0, 0, 0};
  const auto rejected = DualCsrBaselineFactory::create(
      invalid, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{invalid.nodes.size(), 0,
                                       invalid.arcs.size()});
  require(!rejected.success() && !rejected.violations.empty(),
          "factory accepted a full-reference semantic violation");

  auto wrong_identity = input;
  auto& digit = wrong_identity.source_identity.topology_fingerprint.front();
  digit = digit == '0' ? '1' : '0';
  const auto identity_rejected = DualCsrBaselineFactory::create(
      wrong_identity, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{input.nodes.size(), 0, input.arcs.size()});
  require(!identity_rejected.success() && !identity_rejected.violations.empty(),
          "public factory accepted a valid-shaped input with a false identity");
}


void loadAccumulationOrderContract()
{
  auto input = csr::test::cellInput();
  const auto ports = baselineInput();
  auto cell_nodes = std::move(input.nodes);
  input.nodes.clear();
  // Native full-name order is ((1 + 2^-53) + 2^-53), not key-length order.
  for (const char* name : {"b", "c", "a_long"}) {
    auto sink = ports.nodes.back();
    sink.id = csr::NodeId{input.nodes.size()};
    sink.key = nodeKey(name);
    input.nodes.push_back(std::move(sink));
  }
  for (auto& node : cell_nodes) {
    node.id = csr::NodeId{input.nodes.size()};
    input.nodes.push_back(std::move(node));
  }
  input.nodes[4].net_name = input.nodes.front().net_name;
  input.nodes[4].is_endpoint = false;
  input.instances.front().ordered_pins = {csr::NodeId{3}, csr::NodeId{4}};
  input.start_seeds.front().seed_node = input.start_seeds.front().path_start_node = csr::NodeId{3};
  for (auto& capacitance : input.cell_pin_capacitances)
    capacitance.node = csr::NodeId{capacitance.node.value() + 3};
  input.node_ids_by_key.clear();
  for (const auto& node : input.nodes) input.node_ids_by_key.push_back({node.key, node.id});
  input.arcs.front().source = csr::NodeId{3};
  input.arcs.front().sink = csr::NodeId{4};
  input.arcs.front().output_load = csr::test::outputLoad(1.0);
  for (std::size_t i = 0; i < 3; ++i) {
    auto arc = arcRecord(i + 1, input.nodes[4], input.nodes[i], 0);
    arc.native_traversal_rank = (i + 1) % 3 + 1;
    input.arcs.push_back(std::move(arc));
  }
  input.arc_ids_by_key.clear();
  for (const auto& arc : input.arcs) input.arc_ids_by_key.push_back({arc.key, arc.id});
  input.out_row_offsets = {0, 0, 0, 0, 1, 4};
  input.out_arc_ids = {csr::ArcId{0}, csr::ArcId{3}, csr::ArcId{1}, csr::ArcId{2}};
  input.in_row_offsets = {0, 1, 2, 3, 3, 4};
  input.in_arc_ids = {csr::ArcId{1}, csr::ArcId{2}, csr::ArcId{3}, csr::ArcId{0}};
  input.ordered_root_ids = input.ordered_start_ids = {csr::NodeId{3}};
  input.ordered_end_ids = {csr::NodeId{2}, csr::NodeId{0}, csr::NodeId{1}};
  input.clock_owner_by_node.resize(5);
  for (const auto id : input.ordered_end_ids) {
    auto constraint = ports.port_constraints.back();
    constraint.map_key = constraint.embedded_port_name = input.nodes[id.value()].key.full_name;
    constraint.resolved_node = id;
    constraint.output_load = id.value() == 2 ? 1.0 : 0x1p-53;
    input.port_constraints.push_back(std::move(constraint));
    auto endpoint = ports.endpoint_required.front();
    endpoint.node = id;
    input.endpoint_required.push_back(std::move(endpoint));
  }
  csr::test::refreshIdentity(input, kSessionValue);
  csr::test::requireValid(csr::validateReferenceInput(input), "native full-name load order");
  auto attempt = DualCsrBaselineFactory::create(
      input, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{input.nodes.size(), 0, input.arcs.size()});
  require(attempt.success(), "Dual diagnostic projection reordered native load accumulation");
  (*attempt.state->arc_slots.front().payload.output_load)[0][0] = 2.0;
  require(!validateDualCsrState(*attempt.state).empty(), "load-order fix accepted a forged cell output load");
}

void directViewContracts()
{
  const DualCsrState state = baselineState();
  DualCsrEdgeViewImpl edges(state);
  require(edges.nodeCount() == 2 && edges.arcSlotCount() == 1
              && edges.activeArcCount() == 1,
          "Dual edge-view counts are incorrect");

  std::vector<csr::EdgeEntry> outgoing;
  const auto out_status = edges.forEachOutgoing(
      csr::NodeId{0}, [&](const csr::EdgeEntry& entry) {
        outgoing.push_back(entry);
        return csr::EdgeVisitResult::continueVisit();
      });
  require(out_status.control == csr::EdgeVisitControl::kContinue
              && outgoing
                     == std::vector<csr::EdgeEntry>{{
                         csr::ArcId{0}, csr::EdgeStorageKind::kBaseOut}},
          "Dual outgoing view did not expose BaseOut");

  std::vector<csr::EdgeEntry> incoming;
  const auto in_status = edges.forEachIncoming(
      csr::NodeId{1}, [&](const csr::EdgeEntry& entry) {
        incoming.push_back(entry);
        return csr::EdgeVisitResult::continueVisit();
      });
  require(in_status.control == csr::EdgeVisitControl::kContinue
              && incoming
              == std::vector<csr::EdgeEntry>{{
                  csr::ArcId{0}, csr::EdgeStorageKind::kBaseIn}},
          "Dual incoming view did not expose BaseIn");
  require(edges.forEachOutgoing(csr::NodeId{2}, [](const csr::EdgeEntry&) {
            return csr::EdgeVisitResult::continueVisit();
          }).error
              == csr::CsrViewError::kNodeOutOfRange,
          "out-of-range node did not produce a typed view failure");
  require(edges.arc(csr::ArcId{1}).error()
              == csr::CsrViewError::kArcOutOfRange,
          "out-of-range ArcId did not produce a typed view failure");

  DualCsrTimingPayloadViewImpl payloads(state);
  require(payloads.nodeCount() == state.nodes.size()
              && payloads.sourceIdentity()
                     == state.reference_payload.source_identity
              && payloads.orderedRootIds()
                     == state.reference_payload.ordered_root_ids,
          "Dual timing-payload view does not directly project state");
  const auto projection = projectDualCsrState(state);
  require(projection.success()
              && projection.projection->active_arcs.size() == 1
              && projection.projection->active_arcs[0].key
                     == state.arc_slots[0].payload.key,
          "functional projection lost the baseline active arc");
}

void physicalFifoContracts()
{
  DualCsrState state = baselineState();
  auto appended_node = state.nodes[1];
  appended_node.id = csr::NodeId{2};
  appended_node.key = nodeKey("very_long_sink");
  state.nodes.push_back(appended_node);

  auto appended_arc = state.arc_slots[0].payload;
  appended_arc.key.base.sink = appended_node.key;
  appended_arc.sink = appended_node.id;
  state.arc_slots.push_back({appended_arc, true});
  require(csr::arcKeyLess(state.arc_slots[0].payload.key,
                          state.arc_slots[1].payload.key),
          "FIFO canary ArcKeys are not intentionally reverse-ordered");

  state.base_out.row_offsets = {0, 2, 2, 2};
  state.base_out.physical_slots = {csr::ArcId{1}, csr::ArcId{0}};
  state.base_in.row_offsets = {0, 0, 1, 2};
  state.base_in.physical_slots = {csr::ArcId{0}, csr::ArcId{1}};
  state.aux_out.source_to_aux_row.push_back(-1);
  state.high_water.node_slots = 3;
  state.high_water.arc_slots = 2;
  state.telemetry.logical_edges_before = 2;
  state.telemetry.logical_edges_after = 2;
  state.telemetry.base_physical_edges = 2;
  state.telemetry.base_active_edges = 2;
  state.telemetry.base_tombstones = 0;

  DualCsrEdgeViewImpl edges(state);
  std::vector<csr::EdgeEntry> outgoing;
  const auto status = edges.forEachOutgoing(
      csr::NodeId{0}, [&](const csr::EdgeEntry& entry) {
        outgoing.push_back(entry);
        return csr::EdgeVisitResult::continueVisit();
      });
  require(status.control == csr::EdgeVisitControl::kContinue
              && outgoing
                     == std::vector<csr::EdgeEntry>{
                         {csr::ArcId{1},
                          csr::EdgeStorageKind::kBaseOut},
                         {csr::ArcId{0},
                          csr::EdgeStorageKind::kBaseOut}},
          "Dual outgoing view reordered the physical Base row by ArcKey");

  DualCsrTimingPayloadViewImpl payloads(state);
  const auto fifo = csr::replayFifoLevelization(edges, payloads, nullptr);
  require(fifo.complete
              && fifo.level_nodes
                     == std::vector<csr::NodeId>{csr::NodeId{0},
                                                 csr::NodeId{2},
                                                 csr::NodeId{1}},
          "Dual FIFO levelization did not retain physical row order");
}

DualCsrState baseAndAuxState()
{
  DualCsrState state = baselineState();
  auto appended_node = state.nodes[1];
  appended_node.id = csr::NodeId{2};
  appended_node.key = nodeKey("aux_sink");
  state.nodes.push_back(appended_node);

  auto appended_arc = state.arc_slots[0].payload;
  appended_arc.key.base.sink = appended_node.key;
  appended_arc.key.parallel_ordinal = 0;
  appended_arc.sink = appended_node.id;
  state.arc_slots.push_back({appended_arc, true});

  state.base_out.row_offsets = {0, 1, 1, 1};
  state.base_in.row_offsets = {0, 0, 1, 2};
  state.base_in.physical_slots = {csr::ArcId{0}, csr::ArcId{1}};
  state.aux_out.row_sources = {csr::NodeId{0}};
  state.aux_out.row_arcs = {{csr::ArcId{1}}};
  state.aux_out.source_to_aux_row = {0, -1, -1};
  state.high_water.node_slots = 3;
  state.high_water.arc_slots = 2;
  state.telemetry.logical_edges_before = 2;
  state.telemetry.logical_edges_after = 2;
  state.telemetry.base_physical_edges = 1;
  state.telemetry.base_active_edges = 1;
  state.telemetry.base_tombstones = 0;
  state.telemetry.aux_rows = 1;
  state.telemetry.aux_edges = 1;
  return state;
}

void directViewControlAndMalformedContracts()
{
  const DualCsrState state = baseAndAuxState();
  DualCsrEdgeViewImpl edges(state);
  std::size_t visits = 0;
  const auto stopped = edges.forEachOutgoing(
      csr::NodeId{0}, [&](const csr::EdgeEntry&) {
        ++visits;
        return csr::EdgeVisitResult::stopVisit();
      });
  require(stopped.control == csr::EdgeVisitControl::kStop
              && !stopped.error && visits == 1,
          "Base Stop did not short-circuit before the Aux row");

  visits = 0;
  const auto missing_error = edges.forEachOutgoing(
      csr::NodeId{0}, [&](const csr::EdgeEntry&) {
        ++visits;
        return csr::EdgeVisitResult{
            csr::EdgeVisitControl::kError, std::nullopt};
      });
  require(missing_error.control == csr::EdgeVisitControl::kError
              && missing_error.error
                     == csr::CsrViewError::kCallbackError
              && visits == 1,
          "callback Error without a code was not normalized");

  const auto stop_with_error = edges.forEachOutgoing(
      csr::NodeId{0}, [](const csr::EdgeEntry&) {
        return csr::EdgeVisitResult{
            csr::EdgeVisitControl::kStop,
            csr::CsrViewError::kMalformedAdjacency};
      });
  require(stop_with_error.control == csr::EdgeVisitControl::kError
              && stop_with_error.error
                     == csr::CsrViewError::kCallbackError,
          "callback Stop carrying an error was not normalized");

  auto aliased = state;
  aliased.aux_out.source_to_aux_row[1] = 0;
  DualCsrEdgeViewImpl aliased_edges(aliased);
  const auto alias_status = aliased_edges.forEachOutgoing(
      csr::NodeId{1}, [](const csr::EdgeEntry&) {
        return csr::EdgeVisitResult::continueVisit();
      });
  require(alias_status.control == csr::EdgeVisitControl::kError
              && alias_status.error
                     == csr::CsrViewError::kMalformedAdjacency,
          "aliased source-to-Aux-row mapping escaped local validation");

  auto missing_inverse = state;
  missing_inverse.aux_out.source_to_aux_row[0] = -1;
  DualCsrEdgeViewImpl missing_inverse_edges(missing_inverse);
  const auto missing_inverse_status
      = missing_inverse_edges.forEachActiveArc(
          [](const csr::EdgeEntry&) {
            return csr::EdgeVisitResult::continueVisit();
          });
  require(missing_inverse_status.control
                  == csr::EdgeVisitControl::kError
              && missing_inverse_status.error
                     == csr::CsrViewError::kMalformedAdjacency,
          "missing Aux-row inverse mapping silently dropped an Aux arc");
}

void stableKeyContracts()
{
  DualCsrState historical = baselineState();
  historical.arc_slots.push_back(
      {historical.arc_slots[0].payload, false});
  ++historical.high_water.arc_slots;
  require(validateDualCsrState(historical).empty(),
          "inactive historical ArcKey reuse was rejected");
  DualCsrEdgeViewImpl edges(historical);
  require(edges.activeArcCount() == 1
              && !edges.arc(csr::ArcId{1})
              && edges.arc(csr::ArcId{1}).error()
                     == csr::CsrViewError::kInactiveArc,
          "active/history ArcId APIs are not isolated");

  historical.arc_slots[1].active = true;
  require(hasCode(validateDualCsrState(historical),
                  DualCsrViolationCode::kKey),
          "active ArcKey collision was accepted");
}

void telemetryContracts()
{
  auto structure = baselineState();
  ++structure.telemetry.logical_edges_after;
  require(hasCode(validateDualCsrState(structure),
                  DualCsrViolationCode::kExpectedValue),
          "state validator accepted forged active-edge telemetry");
  if constexpr (!incregpusta_debug_checks_enabled)
    return;
  auto generation = baselineState();
  ++generation.telemetry.generation_after;
  require(hasCode(validateDualCsrState(generation),
                  DualCsrViolationCode::kGeneration),
          "state validator accepted forged generation telemetry");
  auto fallback = baselineState();
  ++fallback.telemetry.fallback_count;
  require(hasCode(validateDualCsrState(fallback),
                  DualCsrViolationCode::kConflict),
          "state validator accepted committed fallback telemetry");
}

DualCsrState cellState()
{
  auto input = csr::test::cellInput();
  csr::test::refreshIdentity(input, kSessionValue);
  auto attempt = DualCsrBaselineFactory::create(
      input, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{input.nodes.size(), 0,
                                       input.arcs.size()});
  require(attempt.success(), "Dual cell baseline fixture was rejected");
  return std::move(*attempt.state);
}

void payloadSemanticContracts()
{
  auto disabled = baselineState();
  disabled.arc_slots[0].payload.is_disabled = true;
  require(hasCode(validateDualCsrState(disabled),
                  DualCsrViolationCode::kExpectedValue),
          "DisabledArcEligibilityRejected canary was accepted");

  auto net_transfer = baselineState();
  net_transfer.arc_slots[0].payload.transfer.delay = 1.0;
  require(hasCode(validateDualCsrState(net_transfer),
                  DualCsrViolationCode::kExpectedValue),
          "NetTransferAndOwnerMustBeCanonical transfer canary was accepted");
  auto net_owner = baselineState();
  net_owner.arc_slots[0].payload.key.base.owner_name = "wrong_net";
  require(hasCode(validateDualCsrState(net_owner),
                  DualCsrViolationCode::kKey),
          "NetTransferAndOwnerMustBeCanonical owner canary was accepted");

  auto cell_flags = cellState();
  cell_flags.arc_slots[0].payload.is_clock_arc = true;
  require(hasCode(validateDualCsrState(cell_flags),
                  DualCsrViolationCode::kExpectedValue),
          "CellTransferFlagsAndChildSignatureMirrorKey flag canary was accepted");
  auto cell_signature = cellState();
  csr::test::mutateChildren(cell_signature.arc_slots[0].payload.transfer, [](auto& children) { ++children[0].signature.arc_index; });
  require(hasCode(validateDualCsrState(cell_signature),
                  DualCsrViolationCode::kExpectedValue),
          "CellTransferFlagsAndChildSignatureMirrorKey signature canary was accepted");
  auto cell_table = cellState();
  ista::csr::test::mutateChildren(cell_table.arc_slots[0].payload.transfer, [](auto& children) { children[0].delay_tables[0].present = true; });
  require(!validateDualCsrState(cell_table).empty(),
          "cell timing-table signature/shape canary was accepted");

  auto nan_load = cellState();
  (*nan_load.arc_slots[0].payload.output_load)[0][0]
      = std::numeric_limits<double>::quiet_NaN();
  require(hasCode(validateDualCsrState(nan_load),
                  DualCsrViolationCode::kExpectedValue),
          "OutputLoadRejectsNaNOrNegative NaN canary was accepted");
  auto negative_load = cellState();
  (*negative_load.arc_slots[0].payload.output_load)[0][0] = -1.0;
  require(hasCode(validateDualCsrState(negative_load),
                  DualCsrViolationCode::kExpectedValue),
          "OutputLoadRejectsNaNOrNegative negative canary was accepted");

  auto missing_load = cellState();
  missing_load.arc_slots[0].payload.output_load.reset();
  require(hasCode(validateDualCsrState(missing_load),
                  DualCsrViolationCode::kExpectedValue),
          "OutputLoadPresenceFollowsParasiticDomain empty-domain canary was accepted");
  auto mixed_input = csr::test::mixedParasiticInput();
  csr::test::refreshIdentity(mixed_input, kSessionValue);
  auto mixed = DualCsrBaselineFactory::create(mixed_input,
      timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{mixed_input.nodes.size(), 0, mixed_input.arcs.size()});
  require(mixed.success() && validateDualCsrState(*mixed.state).empty(),
          "valid mixed RC/no-RC state was rejected");
  mixed.state->arc_slots[1].payload.output_load = csr::test::outputLoad(0.5);
  require(hasCode(validateDualCsrState(*mixed.state), DualCsrViolationCode::kExpectedValue),
          "RC arc accepted a no-RC output load");

  auto inactive = cellState();
  auto inactive_payload = inactive.arc_slots[0].payload;
  inactive_payload.key.parallel_ordinal = 1;
  inactive.arc_slots.push_back({std::move(inactive_payload), false});
  ++inactive.high_water.arc_slots;
  require(validateDualCsrState(inactive).empty(),
          "valid inactive stable ArcId fixture was rejected");
  (*inactive.arc_slots[1].payload.output_load)[0][0] = -1.0;
  require(hasCode(validateDualCsrState(inactive),
                  DualCsrViolationCode::kExpectedValue),
          "inactive ArcSemanticPayload escaped semantic validation");
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_dual_csr::test::baselineFactoryContracts();
    ista::timing_dual_csr::test::loadAccumulationOrderContract();
    ista::timing_dual_csr::test::directViewContracts();
    ista::timing_dual_csr::test::physicalFifoContracts();
    ista::timing_dual_csr::test::directViewControlAndMalformedContracts();
    ista::timing_dual_csr::test::stableKeyContracts();
    ista::timing_dual_csr::test::telemetryContracts();
    ista::timing_dual_csr::test::payloadSemanticContracts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
