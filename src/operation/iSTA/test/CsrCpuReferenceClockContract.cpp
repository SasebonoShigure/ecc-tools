#include <algorithm>
#include <limits>

#include "CsrCpuReferenceBackendInternal.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr::test {

void runCpuReferenceClockContracts()
{
  // Exercise the clock stage directly while public six-feature enablement is
  // being integrated. The fixture keeps a distinct data input slew of 1.
  auto input = makeCpuReferenceCellFixture();
  input.clocks = {{"clk", "clk", {{"in", NodeId{0}}}, 10.0, {0.0, 5.0}, false}};
  input.clock_owner_by_node.assign(input.nodes.size(), std::string{"clk"});
  for (auto& arc : input.arcs)
    arc.eligibility.clock = true;
  auto children = input.arcs[1].transfer.children();
  auto& child = children.front();
  child.time_unit_scale = 1.0;
  child.capacitance_unit_scale = 1.0;
  for (auto& table : child.delay_tables)
    std::ranges::fill(table.values, 2.0);
  for (auto& table : child.slew_tables)
    std::ranges::fill(table.values, 3.0);
  input.arcs[1].transfer.setChildren(children);
  FlatCsrEdgeView edges(input);
  FlatTimingPayloadView payloads(input);
  const auto levels = replayFifoLevelization(edges, payloads);
  requireCpuReference(levels.complete && !levels.view_error, "clock fixture must levelize");
  CsrCpuReferenceWorkState work;
  requireCpuReference(initializeCsrCpuReferenceResult(payloads, levels, work) == CsrCpuReferenceStageStatus::kSuccess, "clock seed failed");
  requireCpuReference(initializeCsrCpuReferenceArcs(edges, work) == CsrCpuReferenceStageStatus::kSuccess, "clock arc initialization failed");
  const auto effective = work.result.effective_clock;
  const auto data_slews = work.result.data_slews;
  const auto paths = work.path_rows;
  auto physical = effective;
  for (std::size_t node = 1; node < input.nodes.size(); ++node) {
    for (AnalysisMode analysis : {AnalysisMode::kMax, AnalysisMode::kMin}) {
      for (Transition transition : {Transition::kRise, Transition::kFall}) {
        const auto lane = csrNodeLane(node, analysis, transition);
        physical.arrivals.values[lane] = node == 2 ? 2.0 : 0.0;
        physical.slews.values[lane] = node == 2 ? 3.0 : 0.0;
        physical.predecessor_nodes.presence[lane] = 1;
        physical.predecessor_nodes.values[lane] = NodeId{node - 1};
        physical.predecessor_arc_delays.presence[lane] = 1;
        physical.predecessor_arc_delays.values[lane] = node == 2 ? 2.0 : 0.0;
        physical.predecessor_transitions.presence[lane] = 1;
        physical.predecessor_transitions.values[lane] = transition;
      }
    }
  }
  requireCpuReference(propagateCsrCpuReferenceClocks(edges, payloads, work) == CsrCpuReferenceStageStatus::kSuccess, "clock propagation failed");
  requireCpuReference(work.result.physical_clock == physical, "physical clock values and predecessors differ");
  requireCpuReference(work.result.effective_clock == effective, "ideal effective clock changed during physical propagation");
  requireCpuReference(work.result.data_slews == data_slews && work.path_rows == paths, "clock pass modified data timing");
  requireCpuReference(propagateCsrCpuReferenceForward(edges, work) == CsrCpuReferenceStageStatus::kSuccess, "clock fixture data propagation failed");
  requireCpuReference(propagateCsrCpuReferenceRequired(edges, payloads, work) == CsrCpuReferenceStageStatus::kSuccess, "physical clock result publication failed");
  requireCpuReference(work.result.physical_clock == physical, "final result lost physical clock values or predecessors");

  for (auto& table : child.delay_tables)
    std::ranges::fill(table.values, std::numeric_limits<double>::max());
  input.arcs[1].transfer.setChildren(children);
  FlatCsrEdgeView overflow_edges(input);
  CsrCpuReferenceWorkState overflow_work;
  requireCpuReference(initializeCsrCpuReferenceResult(payloads, levels, overflow_work) == CsrCpuReferenceStageStatus::kSuccess, "numeric case seed failed");
  requireCpuReference(initializeCsrCpuReferenceArcs(overflow_edges, overflow_work) == CsrCpuReferenceStageStatus::kSuccess, "numeric case arc initialization failed");
  requireCpuReference(propagateCsrCpuReferenceClocks(overflow_edges, payloads, overflow_work) == CsrCpuReferenceStageStatus::kNumericFailure,
                      "physical clock accumulation overflow must fail");
  auto chain = makeCpuReferenceDagFixture(4, {{0, 3}, {1, 3}, {2, 3}});
  chain.clocks = {{"clk", "clk", {{"u1:p0", NodeId{0}}, {"u1:p1", NodeId{1}}, {"u1:p2", NodeId{2}}},
                   10.0, {0.0, 5.0}, true}};
  chain.clock_owner_by_node.assign(4, std::string{"clk"});
  for (auto& arc : chain.arcs) {
    arc.eligibility.clock = true;
    arc.transfer.delay = arc.transfer.delay_min = arc.transfer.delay_max = 0.1 + arc.source.value() * 0.75e-6;
  }
  FlatCsrEdgeView chain_edges(chain);
  FlatTimingPayloadView chain_payloads(chain);
  auto chain_levels = replayFifoLevelization(chain_edges, chain_payloads);
  std::optional<decltype(work.result.physical_clock)> expected_clock;
  do {
    CsrCpuReferenceWorkState state;
    requireCpuReference(initializeCsrCpuReferenceResult(chain_payloads, chain_levels, state) == CsrCpuReferenceStageStatus::kSuccess
                            && initializeCsrCpuReferenceArcs(chain_edges, state) == CsrCpuReferenceStageStatus::kSuccess
                            && propagateCsrCpuReferenceClocks(chain_edges, chain_payloads, state) == CsrCpuReferenceStageStatus::kSuccess,
                        "clock epsilon-chain calculation failed");
    const auto& actual = state.result.physical_clock;
    for (std::size_t lane = 0; lane < 4; ++lane)
      requireCpuReference(actual.predecessor_nodes.values[12 + lane] == NodeId{lane < 2 ? 1U : 0U},
                          "clock epsilon winner differs from stable incoming identity");
    if (expected_clock)
      requireCpuReference(actual == *expected_clock, "clock result depends on the legal level schedule");
    else
      expected_clock = actual;
  } while (std::next_permutation(chain_levels.level_nodes.begin(), chain_levels.level_nodes.begin() + 3,
                                [](NodeId lhs, NodeId rhs) { return lhs.value() < rhs.value(); }));

}

}  // namespace ista::csr::test
