// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <exception>
#include <iostream>
#include <vector>

#include "IncrementalLevelizationTestSupport.hpp"
#include "MixedLevelizationValidation.hpp"

namespace ista::timing_incremental_levelization::test {
namespace {

TestEdgeView fourClassView()
{
  TestEdgeView edges(6);
  edges.addArc({0, 2, true, csr::EdgeStorageKind::kBaseOut});
  edges.addArc({1, 3, true, csr::EdgeStorageKind::kBaseOut});
  edges.addArc({1, 3, true, csr::EdgeStorageKind::kAuxOut});
  edges.addArc({0, 3, true, csr::EdgeStorageKind::kBaseOut});
  edges.addArc({3, 2, true, csr::EdgeStorageKind::kBaseOut});
  edges.addArc({3, 4, true, csr::EdgeStorageKind::kBaseOut});
  edges.addArc({4, 5, true, csr::EdgeStorageKind::kAuxOut});
  edges.addArc({5, 0, false, csr::EdgeStorageKind::kAuxOut});
  edges.addArc({5, 5, false, csr::EdgeStorageKind::kBaseOut});
  edges.addArc({1, 3, true, csr::EdgeStorageKind::kBaseOut, false});
  return edges;
}

void fourClassesDedupAndAux()
{
  const auto previous = makeLevels(3, {{0, 1}, {2}});
  const auto attempt = buildMixedLevelizationGraph(
      fourClassView(), previous, {3, 8, 9});
  const auto* graph = std::get_if<MixedPrecedenceGraph>(&attempt);
  require(graph != nullptr, "valid four-class graph was rejected");
  require(validateMixedPrecedenceGraph(*graph).valid(),
          "builder emitted an invalid mixed graph");
  require(graph->domain == MixedIdDomain{3, 6, 3, 2, 5},
          "mixed domain has the wrong old/new boundary");
  require(graph->adjacency_offsets
              == std::vector<std::uint64_t>{0, 2, 2, 4, 5, 5}
              && graph->successors
                     == std::vector<MixedVertexId>{
                         MixedVertexId{1}, MixedVertexId{2},
                         MixedVertexId{1}, MixedVertexId{3},
                         MixedVertexId{4}}
              && graph->indegree
                     == std::vector<std::uint64_t>{0, 2, 1, 1, 1},
          "mixed precedence adjacency is not deterministic");
  const auto& telemetry = graph->telemetry;
  require(telemetry.actual_eligible_arc_visits == 7
              && telemetry.old_old == 1 && telemetry.old_new == 3
              && telemetry.new_old == 1 && telemetry.new_new == 2,
          "four-class visit accounting is incorrect");
  require(telemetry.mapped_actual_arc_candidates == 6
              && telemetry.unique_actual_endpoint_pairs == 5
              && telemetry.actual_parallel_duplicates_collapsed == 1
              && telemetry.proxy_alias_duplicates_collapsed == 1
              && telemetry.virtual_chain_edges == 1
              && telemetry.unique_precedence_edges == 5,
          "two-stage dedup accounting is incorrect");
  require(telemetry.base_out == 5 && telemetry.aux_out == 2
              && validateMixedTelemetryIdentities(telemetry),
          "Base/Aux traversal accounting is inconsistent");

  auto missing_chain = *graph;
  missing_chain.successors.erase(missing_chain.successors.begin());
  missing_chain.adjacency_offsets = {0, 1, 1, 3, 4, 4};
  missing_chain.indegree = {0, 1, 1, 1, 1};
  missing_chain.telemetry.virtual_chain_edges = 0;
  missing_chain.telemetry.unique_precedence_edges = 4;
  require(validateMixedPrecedenceGraph(missing_chain).code
              == MixedValidationCode::kMalformedAdjacency,
          "validator accepted a graph with its virtual chain removed");
}

void builderTypedFailures()
{
  const auto previous = makeLevels(3, {{0, 1}, {2}});
  TestEdgeView conflict(3);
  conflict.addArc({2, 0});
  const auto conflict_attempt = buildMixedLevelizationGraph(
      conflict, previous, {3, 0, 1});
  require(std::get<MixedGraphBuildFailure>(conflict_attempt).code
              == MixedGraphBuildFailureCode::kOldOldOrderConflict,
          "backward old-to-old edge lost its typed builder failure");

  auto malformed = previous;
  malformed.node_to_level[0] = 0;
  const auto malformed_attempt = buildMixedLevelizationGraph(
      fourClassView(), malformed, {3, 0, 1});
  require(std::get<MixedGraphBuildFailure>(malformed_attempt).code
              == MixedGraphBuildFailureCode::kMalformedPreviousLevels,
          "malformed previous inverse was accepted");

  TestEdgeView view_error(3);
  view_error.failActiveTraversal(csr::CsrViewError::kMalformedAdjacency);
  const auto view_attempt = buildMixedLevelizationGraph(
      view_error, previous, {3, 0, 1});
  const auto& view_failure =
      std::get<MixedGraphBuildFailure>(view_attempt);
  require(view_failure.code == MixedGraphBuildFailureCode::kViewError
              && view_failure.view_error
                     == csr::CsrViewError::kMalformedAdjacency,
          "generic view failure was not preserved");

  const auto generation_attempt = buildMixedLevelizationGraph(
      fourClassView(), previous, {3, 4, 6});
  require(std::get<MixedGraphBuildFailure>(generation_attempt).code
              == MixedGraphBuildFailureCode::kInvariantFailure,
          "nonconsecutive generation binding was accepted");
}

}  // namespace
}  // namespace ista::timing_incremental_levelization::test

int main()
{
  try {
    ista::timing_incremental_levelization::test::fourClassesDedupAndAux();
    ista::timing_incremental_levelization::test::builderTypedFailures();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
