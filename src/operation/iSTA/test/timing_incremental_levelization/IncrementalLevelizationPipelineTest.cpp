// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <exception>
#include <iostream>

#include "IncrementalLevelizationTestSupport.hpp"

namespace ista::timing_incremental_levelization::test {
namespace {

void successDomains()
{
  const auto previous = makeLevels(3, {{0, 1}, {2}});
  TestEdgeView mixed(6);
  mixed.addArc({0, 2});
  mixed.addArc({1, 3});
  mixed.addArc({0, 3});
  mixed.addArc({3, 2});
  mixed.addArc({3, 4});
  mixed.addArc({4, 5, true, csr::EdgeStorageKind::kAuxOut});
  const MixedLevelizationSuccess success =
      requirePipelineSuccess(mixed, previous, 3, 12);
  require(success.telemetry.n_last_nodes == 3
              && success.telemetry.n_current_nodes == 6
              && success.telemetry.n_new_nodes == 3
              && success.telemetry.n_old_levels == 2
              && success.telemetry.kahn_processed_count
                     == success.telemetry.mixed_vertex_count
              && success.telemetry.reconstructed_level_count
                     == success.levels.level_offsets.size() - 1
              && success.telemetry.fallback_count == 0,
          "pipeline success telemetry is incomplete");

  TestEdgeView all_new(3);
  all_new.addArc({0, 1});
  all_new.addArc({0, 2, true, csr::EdgeStorageKind::kAuxOut});
  const auto empty_previous = makeLevels(0, {});
  const MixedLevelizationSuccess new_success =
      requirePipelineSuccess(all_new, empty_previous, 0);
  require(new_success.telemetry.n_old_levels == 0
              && new_success.telemetry.n_new_nodes == 3,
          "old=0/new>0 domain was not handled");

  TestEdgeView no_new(3);
  const auto conservative_previous = makeLevels(3, {{0}, {1}, {2}});
  const MixedLevelizationSuccess noncompact =
      requirePipelineSuccess(no_new, conservative_previous, 3);
  require(noncompact.levels.level_offsets
              == conservative_previous.level_offsets
              && noncompact.levels.node_to_level
                     == conservative_previous.node_to_level,
          "removed old edges did not preserve legal conservative levels");

  TestEdgeView single_old_level(3);
  single_old_level.addArc({0, 2});
  const auto one_level_previous = makeLevels(2, {{0, 1}});
  const MixedLevelizationSuccess one_level =
      requirePipelineSuccess(single_old_level, one_level_previous, 2);
  require(one_level.telemetry.n_old_levels == 1
              && one_level.telemetry.mixed_vertex_count == 2,
          "single-old-level domain was not handled");
}

void previousNewNodeBecomesOldInNextBatch()
{
  const auto generation_zero = makeLevels(2, {{0}, {1}});
  TestEdgeView first(3);
  first.addArc({0, 2});
  first.addArc({2, 1});
  const MixedLevelizationSuccess generation_one =
      requirePipelineSuccess(first, generation_zero, 2, 1);

  TestEdgeView second(4);
  second.addArc({0, 2});
  second.addArc({2, 1});
  second.addArc({2, 3, true, csr::EdgeStorageKind::kAuxOut});
  second.addArc({3, 1});
  const MixedLevelizationSuccess generation_two =
      requirePipelineSuccess(second, generation_one.levels, 3, 2);
  require(generation_two.telemetry.n_last_nodes == 3
              && generation_two.telemetry.n_new_nodes == 1
              && generation_two.telemetry.n_old_levels == 3,
          "prior-batch new node did not move into the old/virtual domain");
}

MixedLevelizationFailure runFailure(
    const TestEdgeView& edges, const csr::CsrFifoLevelization& previous,
    std::uint64_t n_last_nodes)
{
  const auto attempt = runCpuIncrementalLevelization(
      edges, previous, {n_last_nodes, 0, 1});
  const auto* result = std::get_if<MixedLevelizationFailure>(&attempt);
  require(result != nullptr, "negative case unexpectedly succeeded");
  return *result;
}

void distinguishesTypedFailureDomains()
{
  const auto previous = makeLevels(2, {{0}, {1}});

  TestEdgeView early_conflict(2);
  early_conflict.addArc({1, 0});
  const MixedLevelizationFailure early =
      runFailure(early_conflict, previous, 2);
  require(early.code == MixedStageFailureCode::kOldLevelOrderConflict
              && early.origin == MixedFailureOrigin::kBuilderOldOld,
          "early old-order conflict was misclassified");

  TestEdgeView quotient_conflict(3);
  quotient_conflict.addArc({1, 2});
  quotient_conflict.addArc({2, 0});
  const MixedLevelizationFailure quotient =
      runFailure(quotient_conflict, previous, 2);
  require(quotient.code == MixedStageFailureCode::kOldLevelOrderConflict
              && quotient.origin
                     == MixedFailureOrigin::kMixedKahnQuotient,
          "acyclic quotient conflict was misclassified as an actual cycle");

  TestEdgeView actual_cycle(3);
  actual_cycle.addArc({0, 2});
  actual_cycle.addArc({2, 0});
  const MixedLevelizationFailure cycle =
      runFailure(actual_cycle, previous, 2);
  require(cycle.code == MixedStageFailureCode::kActualCycle
              && cycle.origin == MixedFailureOrigin::kActualClassifier,
          "actual cycle was not distinguished from old-order conflict");

  TestEdgeView mixed_self_loop(3);
  mixed_self_loop.addArc({2, 2, true,
                          csr::EdgeStorageKind::kAuxOut});
  const MixedLevelizationFailure self_loop =
      runFailure(mixed_self_loop, previous, 2);
  require(self_loop.code == MixedStageFailureCode::kActualCycle
              && self_loop.origin == MixedFailureOrigin::kActualClassifier,
          "mixed self-loop was not classified as an actual cycle");

  TestEdgeView view_error(2);
  view_error.failActiveTraversal(csr::CsrViewError::kCallbackError);
  const MixedLevelizationFailure view =
      runFailure(view_error, previous, 2);
  require(view.code == MixedStageFailureCode::kViewError
              && view.origin == MixedFailureOrigin::kViewTraversal
              && view.view_error == csr::CsrViewError::kCallbackError,
          "generic traversal failure lost its typed view error");

  auto malformed = previous;
  malformed.view_error = csr::CsrViewError::kMalformedAdjacency;
  TestEdgeView unchanged(2);
  const MixedLevelizationFailure malformed_failure =
      runFailure(unchanged, malformed, 2);
  require(malformed_failure.code
              == MixedStageFailureCode::kMalformedPreviousLevels,
          "complete-looking previous levels with view_error were accepted");
}

}  // namespace
}  // namespace ista::timing_incremental_levelization::test

int main()
{
  try {
    ista::timing_incremental_levelization::test::successDomains();
    ista::timing_incremental_levelization::test::previousNewNodeBecomesOldInNextBatch();
    ista::timing_incremental_levelization::test::distinguishesTypedFailureDomains();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
