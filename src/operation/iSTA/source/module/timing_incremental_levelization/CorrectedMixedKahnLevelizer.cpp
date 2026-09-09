// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CorrectedMixedKahnLevelizer.hpp"

#include <algorithm>
#include <vector>

#include "IncreGpuStaBuildConfig.hpp"
#include "MixedLevelizationInternal.hpp"
#include "MixedLevelizationValidation.hpp"
#include "MixedTopologyInternal.hpp"

namespace ista::timing_incremental_levelization {
namespace {

MixedKahnFailure failure(MixedKahnFailureCode code,
                         std::uint64_t processed,
                         std::uint64_t remaining,
                         std::uint64_t max_frontier,
                         std::string detail)
{
  return {code, processed, remaining, max_frontier, std::move(detail)};
}

}  // namespace

MixedKahnAttempt detail::levelizeBuiltMixedGraph(const MixedPrecedenceGraph& graph)
{
  if (graph.indegree.size() != graph.domain.mixed_vertex_count || graph.adjacency_offsets.size() != graph.indegree.size() + 1
      || graph.adjacency_offsets.front() != 0 || graph.adjacency_offsets.back() != graph.successors.size())
    return failure(MixedKahnFailureCode::kInvariantFailure, 0, graph.domain.mixed_vertex_count, 0, "mixed graph dimensions are invalid");
  const std::size_t vertex_count = graph.indegree.size();
  std::vector<std::uint64_t> indegree = graph.indegree;
  // 0 = unseen, 1 = queued, 2 = processed.
  std::vector<std::uint8_t> state(vertex_count, 0);
  std::vector<MixedVertexId> frontier;
  for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
    if (indegree[vertex] == 0) {
      state[vertex] = 1;
      frontier.emplace_back(static_cast<std::uint64_t>(vertex));
    }

  MixedKahnLevelization result;
  result.vertex_to_level.assign(vertex_count, 0);
  result.level_vertices.reserve(vertex_count);
  while (!frontier.empty()) {
    std::sort(frontier.begin(), frontier.end());
    if (std::adjacent_find(frontier.begin(), frontier.end())
        != frontier.end())
      return failure(MixedKahnFailureCode::kInvariantFailure,
                     result.processed_count,
                     graph.domain.mixed_vertex_count
                         - result.processed_count,
                     result.max_frontier,
                     "a mixed vertex was queued twice");
    result.max_frontier = std::max(
        result.max_frontier, static_cast<std::uint64_t>(frontier.size()));
    const std::uint64_t one_based_level =
        static_cast<std::uint64_t>(result.level_offsets.size());
    std::vector<MixedVertexId> next;

    for (MixedVertexId source : frontier) {
      const std::size_t source_index =
          static_cast<std::size_t>(source.value());
      if (source_index >= vertex_count || state[source_index] != 1)
        return failure(MixedKahnFailureCode::kInvariantFailure,
                       result.processed_count,
                       graph.domain.mixed_vertex_count
                           - result.processed_count,
                       result.max_frontier,
                       "queued mixed vertex has invalid state");
      state[source_index] = 2;
      result.level_vertices.push_back(source);
      result.vertex_to_level[source_index] = one_based_level;
      if (!detail::checkedIncrement(result.processed_count))
        return failure(MixedKahnFailureCode::kInvariantFailure,
                       result.processed_count, 0, result.max_frontier,
                       "processed-count overflow");

      const std::uint64_t begin = graph.adjacency_offsets[source_index];
      const std::uint64_t end = graph.adjacency_offsets[source_index + 1];
      if (begin > end || end > graph.successors.size())
        return failure(MixedKahnFailureCode::kInvariantFailure, result.processed_count, graph.domain.mixed_vertex_count - result.processed_count,
                       result.max_frontier, "mixed row is out of range");
      for (std::uint64_t index = begin; index < end; ++index) {
        const MixedVertexId sink =
            graph.successors[static_cast<std::size_t>(index)];
        const std::size_t sink_index =
            static_cast<std::size_t>(sink.value());
        if (sink_index >= vertex_count || indegree[sink_index] == 0)
          return failure(MixedKahnFailureCode::kInvariantFailure,
                         result.processed_count,
                         graph.domain.mixed_vertex_count
                             - result.processed_count,
                         result.max_frontier,
                         "mixed indegree underflow");
        --indegree[sink_index];
        if (sink_index >= vertex_count || indegree[sink_index] == 0) {
          if (state[sink_index] != 0)
            return failure(MixedKahnFailureCode::kInvariantFailure,
                           result.processed_count,
                           graph.domain.mixed_vertex_count
                               - result.processed_count,
                           result.max_frontier,
                           "mixed vertex was re-enqueued");
          state[sink_index] = 1;
          next.push_back(sink);
        }
      }
    }
    result.level_offsets.push_back(
        static_cast<std::uint64_t>(result.level_vertices.size()));
    frontier = std::move(next);
  }

  if (result.processed_count != graph.domain.mixed_vertex_count)
    return failure(MixedKahnFailureCode::kMixedCycle,
                   result.processed_count,
                   graph.domain.mixed_vertex_count
                       - result.processed_count,
                   result.max_frontier,
                   "mixed precedence graph contains a cycle");
  if constexpr (incregpusta_debug_checks_enabled) {
    const MixedValidationResult validation = detail::validateBuiltMixedResult(graph, result);
    if (!validation.valid())
      return failure(MixedKahnFailureCode::kInvariantFailure, result.processed_count, 0, result.max_frontier, validation.detail);
  }
  return result;
}

MixedKahnAttempt runCorrectedMixedKahn(const MixedPrecedenceGraph& graph)
{
  const MixedValidationResult validation = validateMixedPrecedenceGraph(graph);
  if (!validation.valid())
    return failure(MixedKahnFailureCode::kInvariantFailure, 0, graph.domain.mixed_vertex_count, 0, validation.detail);
  return detail::levelizeBuiltMixedGraph(graph);
}

}  // namespace ista::timing_incremental_levelization
