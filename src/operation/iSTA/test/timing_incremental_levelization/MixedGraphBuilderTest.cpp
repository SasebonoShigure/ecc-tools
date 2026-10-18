// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <exception>
#include <iostream>
#include <limits>
#include <vector>

#include "IncrementalLevelizationTestSupport.hpp"
#include "MixedLevelizationValidation.hpp"
#include "MixedTopologyInternal.hpp"

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

void previousNewNodeBecomesOldInNextBatch()
{
  const auto previous = makeLevels(3, {{0}, {2}, {1}});
  TestEdgeView edges(4);
  edges.addArc({0, 2});
  edges.addArc({2, 1});
  edges.addArc({2, 3, true, csr::EdgeStorageKind::kAuxOut});
  edges.addArc({3, 1});
  const auto built = buildMixedLevelizationGraph(edges, previous, {3, 1, 2});
  const auto& graph = std::get<MixedPrecedenceGraph>(built);
  const std::vector<std::uint64_t> groups{2, 1, 3, 999}, remap{0, 3, 1, 2};
  const detail::PreviousLevels owned(previous.level_offsets, previous.level_nodes,
      std::span<const std::uint64_t>(groups).first(3), remap, true);
  const auto remapped = detail::buildOwnedMixedGraph(detail::topologyTraversal(edges), owned, {3, 1, 2});
  require(std::get<MixedPrecedenceGraph>(remapped) == graph,
          "stable group mapping changed mixed topology or consumed the uncommitted suffix");
  require(graph.domain == MixedIdDomain{3, 4, 1, 3, 4}
              && graph.adjacency_offsets == std::vector<std::uint64_t>{0, 1, 3, 3, 4}
              && graph.successors == std::vector<MixedVertexId>{
                  MixedVertexId{1}, MixedVertexId{2}, MixedVertexId{3}, MixedVertexId{2}},
          "prior-batch new node did not move into the old/virtual domain");
}

void malformedPreviousVariants()
{
  const auto valid = makeLevels(2, {{0}, {1}});
  auto offsets = valid;
  offsets.level_offsets = {0, 2, 1};
  auto duplicate = valid;
  duplicate.level_nodes = {csr::NodeId{0}, csr::NodeId{0}};
  auto inverse = valid;
  inverse.node_to_level[1] = 1;
  auto view_error = valid;
  view_error.view_error = csr::CsrViewError::kMalformedAdjacency;
  TestEdgeView edges(2);
  for (const auto* malformed : {&offsets, &duplicate, &inverse, &view_error}) {
    const auto attempt = buildMixedLevelizationGraph(edges, *malformed, {2, 0, 1});
    require(std::get<MixedGraphBuildFailure>(attempt).code
                == MixedGraphBuildFailureCode::kMalformedPreviousLevels,
            "malformed previous levels were accepted at the public builder boundary");
  }
  const auto high_water = buildMixedLevelizationGraph(
      edges, valid, {std::numeric_limits<std::uint64_t>::max(), 0, 1});
  require(std::get<MixedGraphBuildFailure>(high_water).code
              == MixedGraphBuildFailureCode::kMappingOverflow,
          "old-node high-water exceeding current nodes was accepted");
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
    ista::timing_incremental_levelization::test::previousNewNodeBecomesOldInNextBatch();
    ista::timing_incremental_levelization::test::malformedPreviousVariants();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
