#include <utility>
#include <variant>

#include "CsrCpuReferenceTestFixture.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "MixedLevelReconstructor.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"
#include "timing_incremental_levelization/IncrementalLevelizationTestSupport.hpp"

namespace ista::csr::test {
using namespace timing_dual_csr;
using namespace timing_dual_csr::test;
namespace mixed = timing_incremental_levelization;
namespace {
CudaDualCsrSession checkAgainstFullBuilder(CudaDualCsrSession& owner, const CudaDualCsrPatch& patch,
                                          const std::vector<mixed::test::ArcSpec>& arcs)
{
  const auto& before = owner.timingValues();
  CsrFifoLevelization previous;
  previous.complete = true;
  previous.level_nodes = before.level_nodes;
  previous.level_offsets = before.level_offsets;
  previous.node_to_level = before.node_to_level;
  mixed::test::TestEdgeView edges(patch.after.node_slots);
  for (const auto& arc : arcs) edges.addArc(arc);
  const auto built = mixed::buildMixedLevelizationGraph(edges, previous,
      {patch.before.node_slots, patch.expected_generation.value(), patch.target_generation.value()});
  require(std::holds_alternative<mixed::MixedPrecedenceGraph>(built), "independent full builder rejected topology fixture");
  const auto& graph = std::get<mixed::MixedPrecedenceGraph>(built);
  const auto kahn = mixed::runCorrectedMixedKahn(graph);
  require(std::holds_alternative<mixed::MixedKahnLevelization>(kahn), "independent full mixed Kahn failed");
  const auto reconstructed = mixed::reconstructMixedLevelization(graph.domain, previous, std::get<mixed::MixedKahnLevelization>(kahn));
  require(std::holds_alternative<CsrFifoLevelization>(reconstructed), "independent full mixed reconstruction failed");
  const auto& expected = std::get<CsrFifoLevelization>(reconstructed);
  auto candidate = owner.stage(patch, CudaLevelizationMode::kMixed);
  require(!candidate.computationCode(), "local topology candidate failed");
  const auto& actual = candidate.timingValues();
  require(actual.level_offsets == expected.level_offsets && actual.level_nodes == expected.level_nodes
              && actual.node_to_level == expected.node_to_level,
          "delta topology levels differ from independent full builder");
  return candidate;
}

void appendReplaceAndRoots()
{
  const auto input = makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  CudaDualCsrSession owner(input, water);
  const auto baseline = owner.timingValues();
  auto patch = patchFor(input, 0, water, {3, 3, 3});
  const auto middle = appendNode(2, "middle", AppendedPortRole::kSink);
  patch.node_appends = {middle};
  patch.arc_appends = {appendArc(1, NodeId{0}, middle.id, input.nodes[0].key, middle.key, 0),
                       appendArc(2, middle.id, NodeId{1}, middle.key, input.nodes[1].key, 0)};
  patch.changes = {{NodeId{0}, 0, ArcId{0}, ArcId{1}}};
  patch.logical_deactivations = {{ArcId{0}, input.arcs[0].key, NodeId{0}, NodeId{1}, LogicalArcDeactivateReason::kRemoved, true}};
  patch.reverse_changes = {{NodeId{1}, 0, ArcId{0}, ArcId{2}}};
  patch.appended_base_out_rows = {{middle.id, {ArcId{2}}}};
  patch.appended_base_in_rows = {{middle.id, {ArcId{1}}}};
  std::vector<mixed::test::ArcSpec> edges{{0, 1, true, EdgeStorageKind::kBaseOut, false}, {0, 2}, {2, 1}};
  {
    auto abandoned = checkAgainstFullBuilder(owner, patch, edges);
  }
  require(owner.timingValues() == baseline, "abandoned topology edit changed committed result");
  auto candidate = checkAgainstFullBuilder(owner, patch, edges);
  require(owner.publish(std::move(candidate)), "mixed insertion failed to publish after abandonment");

  auto replacement = patchFor(input, 1, patch.after, {3, 3, 4});
  replacement.arc_appends = {appendArc(3, middle.id, NodeId{1}, middle.key, input.nodes[1].key, 1)};
  replacement.changes = {{middle.id, 1, ArcId{2}, ArcId{3}}};
  replacement.logical_deactivations = {{ArcId{2}, patch.arc_appends[1].key, middle.id, NodeId{1}, LogicalArcDeactivateReason::kRemoved, true}};
  replacement.reverse_changes = {{NodeId{1}, 0, ArcId{2}, ArcId{3}}};
  edges[2].active = false;
  edges.push_back({2, 1});
  auto replaced = checkAgainstFullBuilder(owner, replacement, edges);
  require(owner.publish(std::move(replaced)), "same-sink removal plus append changed root membership");

  auto isolated = patchFor(input, 2, replacement.after, {4, 4, 4});
  const auto root = appendNode(3, "isolated", AppendedPortRole::kIsolated);
  isolated.node_appends = {root};
  isolated.appended_base_out_rows = {{root.id, {}}};
  isolated.appended_base_in_rows = {{root.id, {}}};
  auto missing = owner.stage(isolated, CudaLevelizationMode::kMixed);
  require(missing.computationCode() == CsrComputationCode::kInvariantViolation && !owner.poisoned(), "undeclared new root accepted");
  isolated.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, {NodeId{0}, NodeId{2}}};
  auto wrong = owner.stage(isolated, CudaLevelizationMode::kMixed);
  require(wrong.computationCode() == CsrComputationCode::kInvariantViolation, "nonzero-degree root accepted");
  isolated.roots->replacement = {NodeId{0}, NodeId{0}, root.id};
  bool rejected = false;
  try { static_cast<void>(owner.stage(isolated, CudaLevelizationMode::kMixed)); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "duplicate root escaped external input validation");
  isolated.roots->replacement = {NodeId{0}, root.id};
  auto added = checkAgainstFullBuilder(owner, isolated, edges);
  require(owner.publish(std::move(added)), "declared isolated root failed after rejected roots");
  auto empty = checkAgainstFullBuilder(owner, patchFor(input, 3, isolated.after, isolated.after), edges);
  require(owner.publish(std::move(empty)), "empty mixed batch failed after root addition");
}

void parallelEligibility()
{
  const auto input = makeCpuReferenceDagFixture(3, {{0, 1}, {0, 1}, {1, 2}});
  const timing_edit::AppendOnlyHighWater water{3, 3, 3};
  CudaDualCsrSession owner(input, water);
  std::vector<ArcSemanticPayload> payloads;
  std::vector<mixed::test::ArcSpec> edges;
  for (const auto& arc : input.arcs) {
    payloads.push_back({arc.key, arc.source, arc.sink, arc.kind, arc.is_clock_arc, arc.is_disabled,
                        arc.is_loop_disabled, arc.eligibility, arc.transfer, arc.output_load, arc.native_cell_arc_ordinal});
    edges.push_back({arc.source.value(), arc.sink.value()});
  }
  const auto disable = [&](std::uint64_t generation, std::size_t id) {
    auto patch = patchFor(input, generation, water, water);
    auto after = payloads[id];
    after.eligibility = {};
    patch.payload_changes = {{ArcId{id}, payloads[id], after}};
    return patch;
  };
  auto first = disable(0, 0);
  edges[0].levelization = false;
  auto one = checkAgainstFullBuilder(owner, first, edges);
  require(owner.publish(std::move(one)), "disabling one parallel edge changed a nonzero indegree to zero");
  payloads[0] = first.payload_changes[0].replacement;
  auto last = disable(1, 1);
  auto missing = owner.stage(last, CudaLevelizationMode::kMixed);
  require(missing.computationCode() == CsrComputationCode::kInvariantViolation, "last eligible edge removal did not require new root");
  last.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, {NodeId{0}, NodeId{1}}};
  edges[1].levelization = false;
  auto zero = checkAgainstFullBuilder(owner, last, edges);
  require(owner.publish(std::move(zero)), "last eligible edge removal failed with correct roots");
  payloads[1] = last.payload_changes[0].replacement;
  auto enabled = patchFor(input, 2, water, water);
  auto after = payloads[0];
  after.eligibility = input.arcs[0].eligibility;
  enabled.payload_changes = {{ArcId{0}, payloads[0], after}};
  enabled.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}, NodeId{1}}, {NodeId{0}}};
  auto full = owner.stage(enabled, CudaLevelizationMode::kFull);
  require(!full.computationCode() && owner.publish(std::move(full)), "full levelization failed while restoring eligibility");
  edges[0].levelization = true;
  auto continued = checkAgainstFullBuilder(owner, patchFor(input, 3, water, water), edges);
  require(owner.publish(std::move(continued)), "mixed did not retain full-mode degree updates");
}
}  // namespace

void runCudaMixedTopologyContracts()
{
  appendReplaceAndRoots();
  parallelEligibility();
}
}  // namespace ista::csr::test
