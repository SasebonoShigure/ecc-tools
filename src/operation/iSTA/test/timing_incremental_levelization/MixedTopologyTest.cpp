#include <exception>
#include <iostream>

#include "IncrementalLevelizationTestSupport.hpp"
#include "MixedTopologyInternal.hpp"

using namespace ista;
using namespace ista::timing_incremental_levelization;
using namespace ista::timing_incremental_levelization::test;

int main()
{
  try {
    std::vector<detail::TopologyArc> arcs{{csr::NodeId{0}, csr::NodeId{2}, true, csr::EdgeStorageKind::kBaseOut},
                                          {csr::NodeId{2}, csr::NodeId{1}, true, csr::EdgeStorageKind::kAuxOut},
                                          {csr::NodeId{1}, csr::NodeId{0}, false, csr::EdgeStorageKind::kBaseOut}};
    detail::TopologyTraversal topology{3, [&](const detail::TopologyVisitor& visitor) {
                                         for (const auto& arc : arcs) {
                                           auto result = visitor(arc);
                                           if (result.control != csr::EdgeVisitControl::kContinue)
                                             return result;
                                         }
                                         return csr::EdgeVisitResult::continueVisit();
                                       }};
    TestEdgeView public_edges(3);
    for (const auto& arc : arcs)
      public_edges.addArc({arc.source.value(), arc.sink.value(), arc.levelization_eligible, arc.storage_kind});
    const auto previous = makeLevels(2, {{0}, {1}});
    auto built = detail::buildOwnedMixedGraph(topology, previous, {2, 0, 1});
    require(built == buildMixedLevelizationGraph(public_edges, previous, {2, 0, 1}), "narrow topology changed canonical graph or Base/Aux accounting");
    const auto& graph = std::get<MixedPrecedenceGraph>(built);
    require(detail::levelizeBuiltMixedGraph(graph) == runCorrectedMixedKahn(graph), "owned graph changed corrected Kahn results");
    require(detail::classifyActualDag(topology) == classifyActualDag(public_edges), "narrow topology changed actual DAG classification");

    arcs.back().levelization_eligible = true;
    require(detail::classifyActualDag(topology).code == ActualDagClassificationCode::kCycle, "eligibility update did not expose actual cycle");
    arcs.back().source = csr::NodeId{3};
    require(detail::classifyActualDag(topology).code == ActualDagClassificationCode::kViewError, "out-of-domain endpoint was accepted");
    auto malformed_previous = previous;
    malformed_previous.node_to_level.pop_back();
    require(std::holds_alternative<MixedGraphBuildFailure>(detail::buildOwnedMixedGraph(topology, malformed_previous, {2, 0, 1})),
            "owned previous dimensions were not checked");

    auto malformed_graph = graph;
    malformed_graph.successors.front() = MixedVertexId{graph.domain.mixed_vertex_count};
    require(std::get<MixedKahnFailure>(detail::levelizeBuiltMixedGraph(malformed_graph)).code == MixedKahnFailureCode::kInvariantFailure,
            "owned Kahn successor bounds were not checked");
    malformed_graph = graph;
    malformed_graph.adjacency_offsets[1] = graph.successors.size() + 1;
    require(std::holds_alternative<MixedKahnFailure>(detail::levelizeBuiltMixedGraph(malformed_graph)), "owned Kahn row bounds were not checked");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
