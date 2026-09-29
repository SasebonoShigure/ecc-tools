// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <algorithm>
#include <bit>
#include <numeric>
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"
#include "CsrValidatorTestFixture.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::test {
namespace {
void requireBits(double actual, double expected, const std::string& name)
{
  requireCpuReference(std::bit_cast<std::uint64_t>(actual) == std::bit_cast<std::uint64_t>(expected), name + " bit pattern differs");
}
const CsrTimingResult& computeTiming(const CsrReferenceInput& input, CsrComputationResult& computation, const std::string& name)
{
  computation = computeContractTiming(input);
  const auto* result = std::get_if<CsrTimingResult>(&computation.outcome);
  requireCpuReference(result != nullptr, name + " did not return timing");
  requireCpuReference(validateTimingResult(input, *result).empty(), name + " returned an invalid timing result");
  return *result;
}

}  // namespace

void runParallelFrontierContracts()
{
  CsrComputationResult computation;
  const auto across_rounds = computeTiming(
      makeCpuReferenceDagFixture(6, {{0, 2}, {1, 3}, {1, 4}, {2, 4}, {2, 5}}), computation, "cross-round FIFO wake order");
  requireCpuReference(across_rounds.level_offsets == std::vector<std::uint64_t>({0, 2, 4, 6})
                          && across_rounds.level_nodes == std::vector<NodeId>({NodeId{0}, NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}}),
                      "wake order reused a previous frontier's edge ordinal");

  // More than four 256-thread blocks. The FIFO order deliberately opposes
  // NodeId/ArcId order; the first incoming candidate wins the exact tie.
  constexpr std::size_t width = 1025;
  std::vector<DagArc> arcs;
  for (std::size_t node = width; node > 0; --node)
    arcs.push_back({0, node});
  for (std::size_t node = 1; node <= width; ++node)
    arcs.push_back({node, width + 1});
  const auto input = makeCpuReferenceDagFixture(width + 2, arcs);
  const auto wide = computeTiming(input, computation, "cross-block reconvergence");
  requireCpuReference(wide.level_offsets == std::vector<std::uint64_t>({0, 1, width + 1, width + 2}),
                      "wide frontier shape differs");
  for (std::size_t index = 1; index <= width; ++index)
    requireCpuReference(wide.level_nodes[index] == NodeId{width + 1 - index}, "wide FIFO order follows physical slots");
  requireCpuReference(wide.nodes.predecessor_nodes.values.back() == NodeId{1}, "cross-block winner differs from local incoming identity");
  requireBits(wide.nodes.arrivals.values.back(), 2.0, "cross-block arrival");
  for (int repeat = 0; repeat < 3; ++repeat)
    requireExactTimingResult(computeTiming(input, computation, "repeated cross-block reconvergence"), wide,
                             "cross-block result changed between executions");

  // Select within the fixed epsilon window of the numeric extremum. The
  // stable local incoming identity, not the source schedule, chooses the tie.
  auto epsilon = makeCpuReferenceDagFixture(5, {{0, 3}, {0, 2}, {0, 1}, {1, 4}, {2, 4}, {3, 4}});
  for (auto& arc : epsilon.arcs) {
    if (arc.sink == NodeId{4}) {
      const double delay = 1.0 + static_cast<double>(3 - arc.source.value()) * 0.75e-9;
      arc.transfer.delay = arc.transfer.delay_min = arc.transfer.delay_max = delay;
      epsilon.referenced_library_cells.front().cell_arcs[arc.id.value()].transfer = arc.transfer;
    }
  }
  refreshCpuReferenceIdentity(epsilon);
  const auto folded = computeTiming(epsilon, computation, "epsilon-chain stable candidate");
  requireCpuReference(folded.nodes.predecessor_nodes.values.back() == NodeId{1},
                      "epsilon winner differs from the local candidate identity");

  // A different legal source schedule must not change a near-equal slew.
  // The stable incoming candidate identity selects node 1 in the max window;
  // a pairwise epsilon/ID fold fails at least one permutation of this chain.
  auto slew_chain = makeCpuReferenceDagFixture(5, {{0, 3}, {1, 3}, {2, 3}, {3, 4}});
  const auto model = makeCpuReferenceCellFixture().arcs[1].transfer;
  for (auto& arc : slew_chain.arcs) {
    if (arc.sink != NodeId{3})
      continue;
    auto children = model.children();
    for (auto& child : children)
      for (auto& table : child.slew_tables)
        std::ranges::fill(table.values, 0.1 + static_cast<double>(arc.source.value()) * 0.75e-6);
    arc.transfer = model;
    arc.transfer.setChildren(children);
    arc.key.base.library_arcs.clear();
    for (const auto& child : children)
      arc.key.base.library_arcs.push_back(child.signature);
    slew_chain.arc_ids_by_key[arc.id.value()].key = arc.key;
    slew_chain.referenced_library_cells.front().cell_arcs[arc.id.value()].transfer = arc.transfer;
  }
  refreshCpuReferenceIdentity(slew_chain);
  requireCpuReference(validateReferenceInput(slew_chain).empty(), "slew chain input is invalid");
  const auto stable = computeTiming(slew_chain, computation, "stable epsilon slew winner");
  requireBits(stable.data_slews.values[3 * 4], 0.1 + 0.75e-6, "schedule-independent max slew");
  requireBits(stable.data_slews.values[3 * 4 + 2], 0.1, "schedule-independent min slew");
  FlatCsrEdgeView edges(slew_chain);
  FlatTimingPayloadView payloads(slew_chain);
  auto levels = replayFifoLevelization(slew_chain);
  do {
    const auto permuted = CsrCpuReferenceBackend{}.compute(edges, payloads, levels);
    const auto* timing = std::get_if<CsrTimingResult>(&permuted.outcome);
    requireCpuReference(timing != nullptr, "permuted legal levels did not return timing");
    auto normalized = *timing;
    normalized.level_offsets = stable.level_offsets;
    normalized.level_nodes = stable.level_nodes;
    normalized.node_to_level = stable.node_to_level;
    requireExactTimingResult(normalized, stable, "functional result depends on the legal level schedule");
  } while (std::next_permutation(levels.level_nodes.begin(), levels.level_nodes.begin() + 3,
                                [](NodeId lhs, NodeId rhs) { return lhs.value() < rhs.value(); }));

  arcs.clear();
  for (std::size_t node = 0; node < width; ++node)
    arcs.push_back({node, node + 1});
  const auto chain = computeTiming(makeCpuReferenceDagFixture(width + 1, arcs), computation, "deep ordered chain");
  requireCpuReference(chain.level_offsets.size() == width + 2 && chain.node_to_level.back() == width + 1,
                      "deep chain level dependency differs");
  requireBits(chain.nodes.arrivals.values.back(), static_cast<double>(width), "deep chain arrival");
}

}  // namespace ista::csr::test
