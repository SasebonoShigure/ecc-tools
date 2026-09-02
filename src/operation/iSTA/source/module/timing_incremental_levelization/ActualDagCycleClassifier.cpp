// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "ActualDagCycleClassifier.hpp"

#include <queue>
#include <set>
#include <utility>
#include <vector>

#include "MixedLevelizationInternal.hpp"

namespace ista::timing_incremental_levelization {
namespace {

ActualDagClassification classification(
    ActualDagClassificationCode code, std::uint64_t visits,
    std::uint64_t unique_pairs, std::uint64_t processed,
    std::string detail,
    std::optional<csr::CsrViewError> view_error = std::nullopt)
{
  return {code, view_error, visits, unique_pairs, processed,
          std::move(detail)};
}

}  // namespace

ActualDagClassification classifyActualDag(
    const csr::ActiveCsrEdgeView& edges)
{
  using EndpointPair = std::pair<std::uint64_t, std::uint64_t>;
  std::set<EndpointPair> pairs;
  std::uint64_t visits = 0;
  bool counter_overflow = false;
  const csr::EdgeVisitResult visit =
      edges.forEachActiveArc([&](const csr::EdgeEntry& entry) {
        const auto payload = edges.arc(entry.arc_id);
        if (!payload)
          return csr::EdgeVisitResult::fail(payload.error());
        const csr::ArcSemanticPayload& arc = payload.value();
        if (arc.source.value() >= edges.nodeCount()
            || arc.sink.value() >= edges.nodeCount())
          return csr::EdgeVisitResult::fail(csr::CsrViewError::kNodeOutOfRange);
        if (!arc.eligibility.levelization)
          return csr::EdgeVisitResult::continueVisit();
        if (entry.storage_kind != csr::EdgeStorageKind::kBaseOut
            && entry.storage_kind != csr::EdgeStorageKind::kAuxOut)
          return csr::EdgeVisitResult::fail(
              csr::CsrViewError::kMalformedAdjacency);
        counter_overflow |= !detail::checkedIncrement(visits);
        pairs.emplace(arc.source.value(), arc.sink.value());
        return csr::EdgeVisitResult::continueVisit();
      });
  if (counter_overflow)
    return classification(ActualDagClassificationCode::kInvariantFailure,
                          visits, 0, 0,
                          "actual classifier arc counter overflow");
  if (visit.control != csr::EdgeVisitControl::kContinue)
    return classification(
        ActualDagClassificationCode::kViewError, visits, 0, 0,
        "actual graph traversal failed or stopped unexpectedly",
        visit.error.value_or(csr::CsrViewError::kCallbackError));

  const std::size_t node_count = edges.nodeCount();
  std::vector<std::vector<std::size_t>> adjacency(node_count);
  std::vector<std::uint64_t> indegree(node_count, 0);
  for (const EndpointPair& pair : pairs) {
    const std::size_t source = static_cast<std::size_t>(pair.first);
    const std::size_t sink = static_cast<std::size_t>(pair.second);
    adjacency[source].push_back(sink);
    if (!detail::checkedIncrement(indegree[sink]))
      return classification(ActualDagClassificationCode::kInvariantFailure,
                            visits, pairs.size(), 0,
                            "actual indegree overflow");
  }

  std::queue<std::size_t> ready;
  for (std::size_t node = 0; node < node_count; ++node)
    if (indegree[node] == 0)
      ready.push(node);
  std::uint64_t processed = 0;
  while (!ready.empty()) {
    const std::size_t source = ready.front();
    ready.pop();
    if (!detail::checkedIncrement(processed))
      return classification(ActualDagClassificationCode::kInvariantFailure,
                            visits, pairs.size(), processed,
                            "actual processed-count overflow");
    for (std::size_t sink : adjacency[source]) {
      if (indegree[sink] == 0)
        return classification(ActualDagClassificationCode::kInvariantFailure,
                              visits, pairs.size(), processed,
                              "actual indegree underflow");
      if (--indegree[sink] == 0)
        ready.push(sink);
    }
  }
  const std::uint64_t node_count_u64 =
      static_cast<std::uint64_t>(node_count);
  return classification(
      processed == node_count_u64 ? ActualDagClassificationCode::kDag
                                  : ActualDagClassificationCode::kCycle,
      visits, static_cast<std::uint64_t>(pairs.size()), processed,
      processed == node_count_u64 ? "actual graph is a DAG"
                                  : "actual graph contains a cycle");
}

}  // namespace ista::timing_incremental_levelization
