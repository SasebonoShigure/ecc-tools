// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <exception>
#include <iostream>
#include <vector>

#include "CorrectedMixedKahnLevelizer.hpp"
#include "IncrementalLevelizationTestSupport.hpp"
#include "MixedLevelizationValidation.hpp"

namespace ista::timing_incremental_levelization::test {
namespace {

MixedPrecedenceGraph cycleGraph()
{
  MixedPrecedenceGraph graph;
  graph.domain = std::get<MixedIdDomain>(makeMixedIdDomain(0, 2, 0));
  graph.generation_after = 1;
  graph.adjacency_offsets = {0, 1, 2};
  graph.successors = {MixedVertexId{1}, MixedVertexId{0}};
  graph.indegree = {1, 1};
  graph.telemetry.n_current_nodes = 2;
  graph.telemetry.n_new_nodes = 2;
  graph.telemetry.mixed_vertex_count = 2;
  graph.telemetry.actual_eligible_arc_visits = 2;
  graph.telemetry.new_new = 2;
  graph.telemetry.mapped_actual_arc_candidates = 2;
  graph.telemetry.unique_actual_endpoint_pairs = 2;
  graph.telemetry.unique_precedence_edges = 2;
  graph.telemetry.base_out = 2;
  return graph;
}

void indegreeKahnAndDeterministicFrontiers()
{
  TestEdgeView edges(6);
  edges.addArc({0, 2});
  edges.addArc({1, 3});
  edges.addArc({0, 3});
  edges.addArc({3, 2});
  edges.addArc({3, 4});
  edges.addArc({4, 5});
  const auto previous = makeLevels(3, {{0, 1}, {2}});
  const auto built = buildMixedLevelizationGraph(edges, previous, {3, 0, 1});
  const auto& graph = std::get<MixedPrecedenceGraph>(built);
  const auto attempt = runCorrectedMixedKahn(graph);
  const auto* levels = std::get_if<MixedKahnLevelization>(&attempt);
  require(levels != nullptr, "acyclic mixed graph failed Kahn");
  require(levels->level_offsets
              == std::vector<std::uint64_t>{0, 1, 2, 4, 5}
              && levels->level_vertices
                     == std::vector<MixedVertexId>{
                         MixedVertexId{0}, MixedVertexId{2},
                         MixedVertexId{1}, MixedVertexId{3},
                         MixedVertexId{4}}
              && levels->processed_count == 5
              && levels->max_frontier == 2,
          "corrected Kahn did not use sorted indegree frontiers");
  require(validateMixedKahnLevelization(graph, *levels).valid(),
          "Kahn success violates its output contract");

  MixedPrecedenceGraph independent;
  independent.domain = std::get<MixedIdDomain>(makeMixedIdDomain(0, 3, 0));
  independent.generation_after = 1;
  independent.adjacency_offsets = {0, 0, 0, 0};
  independent.indegree = {0, 0, 0};
  independent.telemetry.n_current_nodes = 3;
  independent.telemetry.n_new_nodes = 3;
  independent.telemetry.mixed_vertex_count = 3;
  const auto independent_result =
      std::get<MixedKahnLevelization>(runCorrectedMixedKahn(independent));
  require(independent_result.level_offsets
              == std::vector<std::uint64_t>{0, 3}
              && independent_result.level_vertices
                     == std::vector<MixedVertexId>{
                         MixedVertexId{0}, MixedVertexId{1},
                         MixedVertexId{2}},
          "same-frontier vertices were not sorted by MixedVertexId");
}

void cycleAndInvariantFailures()
{
  const auto cycle = runCorrectedMixedKahn(cycleGraph());
  const auto& cycle_failure = std::get<MixedKahnFailure>(cycle);
  require(cycle_failure.code == MixedKahnFailureCode::kMixedCycle
              && cycle_failure.processed_count == 0
              && cycle_failure.remaining_vertex_count == 2,
          "processed-count cycle classification is incorrect");

  auto malformed = cycleGraph();
  malformed.indegree[0] = 0;
  const auto invalid = runCorrectedMixedKahn(malformed);
  require(std::get<MixedKahnFailure>(invalid).code
              == MixedKahnFailureCode::kInvariantFailure,
          "indegree mismatch/underflow canary was not rejected");
}

}  // namespace
}  // namespace ista::timing_incremental_levelization::test

int main()
{
  try {
    ista::timing_incremental_levelization::test::indegreeKahnAndDeterministicFrontiers();
    ista::timing_incremental_levelization::test::cycleAndInvariantFailures();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
