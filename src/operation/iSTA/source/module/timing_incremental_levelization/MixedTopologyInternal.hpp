#pragma once

#include <functional>

#include "ActualDagCycleClassifier.hpp"
#include "CorrectedMixedKahnLevelizer.hpp"
#include "MixedLevelizationInternal.hpp"
#include "MixedLevelizationValidation.hpp"
#include "MixedLevelReconstructor.hpp"

namespace ista::timing_incremental_levelization::detail {

// Logical endpoints and actual forward storage membership only. No timing
// payload or physical CSR slot ownership crosses this boundary.
struct TopologyArc
{
  csr::NodeId source, sink;
  bool levelization_eligible;
  csr::EdgeStorageKind storage_kind;
};
using TopologyVisitor = std::function<csr::EdgeVisitResult(const TopologyArc&)>;
struct TopologyTraversal
{
  std::size_t node_count;
  std::function<csr::EdgeVisitResult(const TopologyVisitor&)> visit;
};

inline TopologyTraversal topologyTraversal(const csr::ActiveCsrEdgeView& edges)
{
  return {edges.nodeCount(), [&edges](const TopologyVisitor& visitor) {
            return edges.forEachActiveArc([&](const csr::EdgeEntry& entry) {
              const auto payload = edges.arc(entry.arc_id);
              if (!payload)
                return csr::EdgeVisitResult::fail(payload.error());
              const auto& arc = payload.value();
              return visitor({arc.source, arc.sink, arc.eligibility.levelization, entry.storage_kind});
            });
          }};
}

// Internal owner path: previous levels belong to a successful computation.
// Arbitrary callers use the public ActiveCsrEdgeView/graph entry points.
[[nodiscard]] MixedGraphBuildAttempt buildOwnedMixedGraph(const TopologyTraversal& edges, const PreviousLevels& previous,
                                                          const MixedLevelizationRequest& request);
[[nodiscard]] MixedValidationResult validateBuiltMixedResult(const MixedPrecedenceGraph& graph, const MixedKahnLevelization& levels);
[[nodiscard]] MixedKahnAttempt levelizeBuiltMixedGraph(const MixedPrecedenceGraph& graph);
[[nodiscard]] ActualDagClassification classifyActualDag(const TopologyTraversal& edges);

}  // namespace ista::timing_incremental_levelization::detail
