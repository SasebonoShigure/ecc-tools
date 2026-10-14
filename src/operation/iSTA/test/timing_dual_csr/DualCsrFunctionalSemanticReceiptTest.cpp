// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <variant>

#include "CsrCpuReferenceBackend.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"

namespace ista::timing_dual_csr::test {
namespace {

DualCsrState renumberedStableState()
{
  DualCsrState result = baselineState();
  const auto old_nodes = result.nodes;
  result.nodes = {old_nodes[1], old_nodes[0]};
  result.nodes[0].id = csr::NodeId{0};
  result.nodes[1].id = csr::NodeId{1};
  result.arc_slots[0].payload.source = csr::NodeId{1};
  result.arc_slots[0].payload.sink = csr::NodeId{0};
  result.base_out.row_offsets = {0, 0, 1};
  result.base_in.row_offsets = {0, 1, 1};
  result.reference_payload.ordered_root_ids = {csr::NodeId{1}};
  result.reference_payload.ordered_start_ids = {csr::NodeId{1}};
  result.reference_payload.ordered_end_ids = {csr::NodeId{0}};
  result.reference_payload.clock_owner_by_node = {
      result.reference_payload.clock_owner_by_node[1],
      result.reference_payload.clock_owner_by_node[0]};
  for (auto& constraint : result.reference_payload.port_constraints) {
    if (constraint.resolved_node == csr::NodeId{0})
      constraint.resolved_node = csr::NodeId{1};
    else if (constraint.resolved_node == csr::NodeId{1})
      constraint.resolved_node = csr::NodeId{0};
  }
  for (auto& seed : result.reference_payload.start_seeds) {
    seed.seed_node = csr::NodeId{1};
    seed.path_start_node = csr::NodeId{1};
  }
  for (auto& endpoint : result.reference_payload.endpoint_required)
    endpoint.node = csr::NodeId{0};
  result.session_token = timing_edit::TimingEditSessionToken{91};
  result.generation = timing_edit::TransactionGeneration{7};
  result.reference_payload.source_identity.session_token = 91;
  result.reference_payload.source_identity.graph_generation = 7;
  result.telemetry.generation_before = 6;
  result.telemetry.generation_after = 7;

  DualCsrArcSlot active = result.arc_slots.front();
  DualCsrArcSlot historical = active;
  historical.active = false;
  historical.payload.key.parallel_ordinal = 1;
  result.arc_slots = {std::move(historical), std::move(active)};
  result.base_out.physical_slots[0] = csr::ArcId{1};
  result.base_in.physical_slots[0] = csr::ArcId{1};
  result.high_water.arc_slots = result.arc_slots.size();
  require(validateDualCsrState(result).empty(),
          "renumbered stable/tombstone state was rejected");
  return result;
}

DualCsrTimingResult timingFor(const DualCsrState& state)
{
  csr::CsrCpuReferenceBackend backend;
  DualCsrEdgeViewImpl edges(state);
  DualCsrTimingPayloadViewImpl payloads(state);
  auto output = backend.computeWithTelemetry(edges, payloads);
  const auto* values
      = std::get_if<csr::CsrTimingResult>(&output.computation.outcome);
  require(values != nullptr, "semantic receipt fixture did not compute");
  DualCsrTimingResult result{*values};
  const auto violations = validateDualCsrTimingResult(state, result);
  require(violations.empty(),
          "semantic receipt timing failed intrinsic validation at "
              + (violations.empty() ? std::string{}
                                    : violations.front().field));
  return result;
}

FunctionalSemanticReceipt receiptFor(const DualCsrState& state)
{
  const auto attempt = projectDualCsrFunctionalSemanticReceipt(
      state, timingFor(state));
  require(attempt.success(),
          "functional semantic receipt projection was rejected");
  return *attempt.receipt;
}

void rankAndStorageHistoryAreNotFunctional()
{
  const DualCsrState compact = baselineState();
  const DualCsrState stable = renumberedStableState();
  require(compact.session_token != stable.session_token
              && compact.generation != stable.generation
              && compact.high_water != stable.high_water
              && compact.reference_payload.source_identity
                     != stable.reference_payload.source_identity,
          "equivalence canary did not vary physical/session identity");
  require(compact.nodes[0].key == stable.nodes[1].key
              && compact.arc_slots[0].active
              && !stable.arc_slots[0].active
              && stable.arc_slots[1].active,
          "equivalence canary did not vary stable NodeId/ArcId layout");

  const FunctionalSemanticReceipt compact_receipt = receiptFor(compact);
  const FunctionalSemanticReceipt stable_receipt = receiptFor(stable);
  require(compact_receipt == stable_receipt,
          "functional receipt retained rank/session/tombstone history");
  require(stable_receipt.graph.active_arcs.size() == 1
              && stable_receipt.timing.arcs.delays.values.size() == 1
              && stable_receipt.timing.arcs.timing_arc_row_offsets.size()
                     == 2,
          "inactive stable ArcId was not normalized out of the receipt");
  require(stable_receipt.graph.nodes[0].key.full_name == "in"
              && stable_receipt.graph.active_arcs[0].source.full_name
                     == "in"
              && stable_receipt.graph.active_arcs[0].sink.full_name
                     == "out",
          "canonical NodeKey/ArcKey ordering was not preserved");
}

void timingSchemaIsIndependentOfInput()
{
  const auto state = baselineState();
  auto result = timingFor(state);
  require(result.values.schema_version == csr::kCsrSchemaVersion
              && result.values.schema_version != state.reference_payload.schema_version,
          "timing result inherited the input schema");
  result.values.schema_version = state.reference_payload.schema_version;
  const auto errors = validateDualCsrTimingResult(state, result);
  require(std::ranges::any_of(errors, [](const auto& error) {
    return error.field == "result.schema_version";
  }), "validator accepted an input schema as a timing result schema");
}

void requiredMinimumCannotBeWeakened()
{
  const auto state = baselineState();
  auto result = timingFor(state);
  result.values.nodes.requireds.values[0] -= 0.125;
  result.values.nodes.slacks.values[0] -= 0.125;
  const auto errors = validateDualCsrTimingResult(state, result);
  require(std::ranges::any_of(errors, [](const auto& error) {
    return error.field == "result.nodes.requireds";
  }), "validator accepted lower non-endpoint required with matching slack");
}

void clockOwnershipAndPhysicalCausality()
{
  auto input = baselineInput();
  input.clocks = {{"clk", "clk", {{"in", csr::NodeId{0}}}, 10.0, {0.0, 5.0}, false}};
  input.clock_owner_by_node.assign(2, std::string{"clk"});
  input.arcs.front().eligibility.clock = true;
  input.endpoint_required.front().clock_period = 10.0;
  for (std::size_t analysis = 0; analysis < csr::kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < csr::kTransitionCount; ++transition)
      input.start_seeds.front().arrival[analysis][transition] = analysis == 0 && transition == 1 ? 5.0 : 0.0;
  csr::test::refreshIdentity(input, kSessionValue);
  auto baseline = DualCsrBaselineFactory::create(input, timing_edit::TimingEditSessionToken{kSessionValue},
                                                timing_edit::AppendOnlyHighWater{2, 0, 1});
  require(baseline.success(), "clock fanout baseline rejected");
  const auto& state = *baseline.state;
  auto result = timingFor(state);
  require(result.values.nodes.is_clock_point.values == std::vector<std::uint8_t>{1, 1}
          && result.values.physical_clock.predecessor_nodes.presence[4] == 1,
          "clock propagation did not produce an owned sink/predecessor");
  input.clocks.front().propagated = true;
  csr::test::refreshIdentity(input, kSessionValue);
  auto propagated_baseline = DualCsrBaselineFactory::create(input, timing_edit::TimingEditSessionToken{kSessionValue},
                                                           timing_edit::AppendOnlyHighWater{2, 0, 1});
  require(propagated_baseline.success(), "Dual propagated baseline rejected");
  const auto& propagated_state = *propagated_baseline.state;
  const auto propagated = timingFor(propagated_state);
  require(propagated.values.effective_clock == propagated.values.physical_clock
              && validateDualCsrTimingResult(propagated_state, propagated).empty(),
          "Dual propagated clock was not computed or validated");
  auto missing = propagated;
  missing.values.effective_clock.predecessor_nodes.presence[4] = 0;
  missing.values.effective_clock.predecessor_arc_delays.presence[4] = 0;
  missing.values.effective_clock.predecessor_transitions.presence[4] = 0;
  require(!validateDualCsrTimingResult(propagated_state, missing).empty(),
          "Dual propagated clock lost its effective predecessor");
  result.values.physical_clock.predecessor_arc_delays.values[4] += 0.25;
  require(!validateDualCsrTimingResult(state, result).empty(),
          "physical clock validator accepted a forged predecessor delay");
}

void semanticDifferenceIsVisible()
{
  const FunctionalSemanticReceipt baseline = receiptFor(baselineState());
  FunctionalSemanticReceipt changed = baseline;
  require(!changed.timing.nodes.arrivals.values.empty(),
          "negative canary has no projected arrival");
  changed.timing.nodes.arrivals.values[0] += 0.125;
  require(changed != baseline,
          "functional receipt ignored a real timing semantic change");
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_dual_csr::test::rankAndStorageHistoryAreNotFunctional();
    ista::timing_dual_csr::test::timingSchemaIsIndependentOfInput();
    ista::timing_dual_csr::test::requiredMinimumCannotBeWeakened();
    ista::timing_dual_csr::test::clockOwnershipAndPhysicalCausality();
    ista::timing_dual_csr::test::semanticDifferenceIsVisible();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
