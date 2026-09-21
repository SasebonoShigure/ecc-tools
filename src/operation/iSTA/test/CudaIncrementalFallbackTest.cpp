#include <iostream>
#include <stdexcept>
#include <string>

#include "CsrCpuReferenceTestFixture.hpp"
#include "CudaTargetedExecutionInternal.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"

namespace {
using namespace ista;
using namespace csr;
using namespace timing_dual_csr;
using namespace timing_dual_csr::test;
struct Fixture
{
  CsrReferenceInput input = csr::test::makeCpuReferenceNetFixture();
  timing_edit::AppendOnlyHighWater initial{2, 2, 1}, water{3, 3, 2};
  CudaDualCsrPatch first, conflict, cycle;

  CudaDualCsrPatch patch(std::uint64_t generation, timing_edit::AppendOnlyHighWater before, timing_edit::AppendOnlyHighWater after) const
  {
    CudaDualCsrPatch result;
    result.session_token = timing_edit::TimingEditSessionToken{input.source_identity.session_token};
    result.expected_generation = timing_edit::TransactionGeneration{generation};
    result.target_generation = timing_edit::TransactionGeneration{generation + 1};
    result.before = before;
    result.after = after;
    return result;
  }
  Fixture()
  {
    first = patch(0, initial, water);
    const auto sink = appendNode(2, "aux_sink", AppendedPortRole::kSink);
    first.node_appends = {sink};
    first.arc_appends = {appendArc(1, NodeId{0}, NodeId{2}, input.nodes[0].key, sink.key, 0)};
    first.aux_rows = {{NodeId{0}, {}, {ArcId{1}}}};
    first.appended_base_out_rows = {{NodeId{2}, {}}};
    first.appended_base_in_rows = {{NodeId{2}, {ArcId{1}}}};
    first.ends = ValueReplacement<std::vector<NodeId>>{{NodeId{1}}, {NodeId{1}, NodeId{2}}};
    first.appended_endpoints = {{NodeId{2}, "clk", 0.0, {2.0, std::nullopt}}};
    conflict = patch(1, water, {3, 3, 3});
    conflict.arc_appends = {appendArc(2, NodeId{2}, NodeId{1}, sink.key, input.nodes[1].key, 0)};
    conflict.removes = {{NodeId{0}, 0, ArcId{0}}};
    conflict.logical_deactivations = {{ArcId{0}, input.arcs[0].key, NodeId{0}, NodeId{1}, LogicalArcDeactivateReason::kRemoved, true}};
    conflict.reverse_changes = {{NodeId{1}, 0, ArcId{0}, ArcId{2}}};
    conflict.aux_rows = {{NodeId{2}, {}, {ArcId{2}}}};
    cycle = conflict;
    cycle.after.arc_slots = 4;
    cycle.arc_appends.push_back(appendArc(3, NodeId{1}, NodeId{2}, input.nodes[1].key, sink.key, 0));
    cycle.logical_deactivations.push_back({ArcId{1}, first.arc_appends[0].key, NodeId{0}, NodeId{2}, LogicalArcDeactivateReason::kAuxCleared, true});
    cycle.reverse_changes.push_back({NodeId{2}, 1, ArcId{1}, ArcId{3}});
    cycle.aux_rows.push_back({NodeId{0}, {ArcId{1}}, {}});
    cycle.aux_rows.push_back({NodeId{1}, {}, {ArcId{3}}});
  }
  CudaDualCsrSession owner() const
  {
    CudaDualCsrSession result(input, initial);
    auto first_candidate = result.stage(first);
    require(result.publish(std::move(first_candidate)), "fixture initial Aux batch failed");
    return result;
  }
};

void rejectAndContinue(const Fixture& fixture, bool actual_cycle)
{
  auto owner = fixture.owner();
  const auto before = owner.timingValues();
  CudaTargetedTraceBatch batch;
  bool rejected = false;
  try {
    auto unexpected = csr::detail::stageCudaTargetedBatch(owner, actual_cycle ? fixture.cycle : fixture.conflict, StaUpdateStrategy::kIncremental,
                                                          actual_cycle ? StaFallbackPolicy::kFullWithRecord : StaFallbackPolicy::kError, batch);
    static_cast<void>(unexpected);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected && !batch.rejected_mixed_attempt, "production policy accepted conflict or attempted cycle fallback");
  require(owner.generation().value() == 1 && !owner.poisoned() && owner.timingValues() == before, "rejected policy changed origin");
  auto healthy = csr::detail::stageCudaTargetedBatch(owner, fixture.patch(1, fixture.water, fixture.water), StaUpdateStrategy::kIncremental,
                                                     StaFallbackPolicy::kError, batch);
  require(healthy.timingValues() == before && owner.publish(std::move(healthy)), "healthy batch after policy rejection failed");
}
void fallbackAndContinue(const Fixture& fixture)
{
  auto owner = fixture.owner();
  auto independent = fixture.owner();
  auto full = independent.stage(fixture.conflict, CudaLevelizationMode::kFull);
  require(!full.computationCode(), "independent full DAG computation failed");
  CudaTargetedTraceBatch batch;
  auto candidate = csr::detail::stageCudaTargetedBatch(owner, fixture.conflict, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kFullWithRecord, batch);
  const StaExecutionRecord expected{StaBackend::kCuda,
                                    StaUpdateStrategy::kIncremental,
                                    StaFallbackPolicy::kFullWithRecord,
                                    StaBackend::kCuda,
                                    StaUpdateStrategy::kFull,
                                    StaExecutionKind::kFallbackFull,
                                    StaPropagationScope::kFullGraph,
                                    false,
                                    true,
                                    StaFallbackReason::kOldLevelOrderConflict};
  require(batch.execution == expected && batch.rejected_mixed_attempt
              && batch.rejected_mixed_attempt->mixed_failure == timing_incremental_levelization::MixedStageFailureCode::kOldLevelOrderConflict,
          "production fallback record or rejected attempt is incorrect");
  require(candidate.timingValues() == full.timingValues(), "production full fallback differs from independent full execution");
  require(owner.publish(std::move(candidate)) && owner.generation().value() == 2, "production full fallback could not publish");
  CudaTargetedTraceBatch next;
  const auto committed = owner.timingValues();
  auto continuation = csr::detail::stageCudaTargetedBatch(owner, fixture.patch(2, fixture.conflict.after, fixture.conflict.after),
                                                          StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError, next);
  require(continuation.timingValues() == committed && next.execution.execution_kind == StaExecutionKind::kIncrementalUpdate
              && !next.rejected_mixed_attempt && owner.publish(std::move(continuation)),
          "mixed execution did not continue after actual full fallback");
}
}  // namespace
int main(int argc, char** argv)
{
  try {
    require(argc <= 2, "usage: fallback-test [error|fallback|cycle]");
    const std::string mode = argc == 2 ? argv[1] : "all";
    require(mode == "all" || mode == "error" || mode == "fallback" || mode == "cycle", "unknown test case");
    const Fixture fixture;
    if (mode == "all" || mode == "error")
      rejectAndContinue(fixture, false);
    if (mode == "all" || mode == "fallback")
      fallbackAndContinue(fixture);
    if (mode == "all" || mode == "cycle")
      rejectAndContinue(fixture, true);
    std::cout << "production CUDA incremental fallback policy passed: " << mode << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
