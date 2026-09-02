// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "MixedLevelizationValidation.hpp"

#include <algorithm>
#include <limits>
#include <vector>

#include "MixedLevelizationInternal.hpp"

namespace ista::timing_incremental_levelization {
namespace {

MixedValidationResult invalid(MixedValidationCode code, std::string detail)
{
  return {code, std::move(detail)};
}

}  // namespace

MixedValidationResult validateMixedPrecedenceGraph(
    const MixedPrecedenceGraph& graph)
{
  const auto domain = makeMixedIdDomain(
      graph.domain.n_last_nodes, graph.domain.n_current_nodes,
      graph.domain.n_old_levels);
  const auto* expected = std::get_if<MixedIdDomain>(&domain);
  if (expected == nullptr || *expected != graph.domain)
    return invalid(MixedValidationCode::kMalformedDomain,
                   "mixed graph domain fields are inconsistent");
  std::uint64_t expected_generation = 0;
  if (!detail::checkedAdd(graph.generation_before, 1,
                          expected_generation)
      || graph.generation_after != expected_generation)
    return invalid(MixedValidationCode::kMalformedDomain,
                   "mixed graph generation binding is not consecutive");
  if (graph.domain.mixed_vertex_count
      == std::numeric_limits<std::uint64_t>::max())
    return invalid(MixedValidationCode::kMalformedDomain,
                   "mixed graph offset count overflows");
  const std::uint64_t expected_offset_count =
      graph.domain.mixed_vertex_count + 1;
  if (expected_offset_count
          > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())
      || graph.adjacency_offsets.size()
             != static_cast<std::size_t>(expected_offset_count)
      || graph.indegree.size()
             != static_cast<std::size_t>(graph.domain.mixed_vertex_count))
    return invalid(MixedValidationCode::kMalformedAdjacency,
                   "mixed adjacency or indegree has the wrong shape");
  if (graph.adjacency_offsets.empty()
      || graph.adjacency_offsets.front() != 0
      || graph.adjacency_offsets.back() != graph.successors.size())
    return invalid(MixedValidationCode::kMalformedAdjacency,
                   "mixed adjacency offsets do not cover successors");

  std::vector<std::uint64_t> indegree(graph.indegree.size(), 0);
  for (std::size_t source = 0; source < graph.indegree.size(); ++source) {
    bool found_required_chain_edge = false;
    const std::uint64_t begin = graph.adjacency_offsets[source];
    const std::uint64_t end = graph.adjacency_offsets[source + 1];
    if (begin > end || end > graph.successors.size())
      return invalid(MixedValidationCode::kMalformedAdjacency,
                     "mixed adjacency row bounds are malformed");
    std::optional<std::uint64_t> last;
    for (std::uint64_t index = begin; index < end; ++index) {
      const MixedVertexId sink =
          graph.successors[static_cast<std::size_t>(index)];
      if (sink.value() >= graph.domain.mixed_vertex_count)
        return invalid(MixedValidationCode::kMalformedAdjacency,
                       "mixed successor is out of range");
      if (last.has_value() && *last >= sink.value())
        return invalid(MixedValidationCode::kMalformedAdjacency,
                       "mixed successors are not sorted and unique");
      last = sink.value();
      if (source < graph.domain.n_old_levels
          && sink.value() < graph.domain.n_old_levels) {
        if (source + 1 >= graph.domain.n_old_levels
            || sink.value() != source + 1)
          return invalid(MixedValidationCode::kMalformedAdjacency,
                         "virtual-to-virtual edge is not a chain edge");
        found_required_chain_edge = true;
      }
      if (!detail::checkedIncrement(
              indegree[static_cast<std::size_t>(sink.value())]))
        return invalid(MixedValidationCode::kIndegreeMismatch,
                       "recomputed mixed indegree overflows");
    }
    if (source + 1 < graph.domain.n_old_levels
        && !found_required_chain_edge)
      return invalid(MixedValidationCode::kMalformedAdjacency,
                     "mandatory virtual chain edge is missing");
  }
  if (indegree != graph.indegree)
    return invalid(MixedValidationCode::kIndegreeMismatch,
                   "stored mixed indegree disagrees with adjacency");
  if (graph.telemetry.n_last_nodes != graph.domain.n_last_nodes
      || graph.telemetry.n_current_nodes != graph.domain.n_current_nodes
      || graph.telemetry.n_new_nodes != graph.domain.n_new_nodes
      || graph.telemetry.n_old_levels != graph.domain.n_old_levels
      || graph.telemetry.mixed_vertex_count
             != graph.domain.mixed_vertex_count
      || graph.telemetry.unique_precedence_edges
             != graph.successors.size()
      || !validateMixedTelemetryIdentities(graph.telemetry))
    return invalid(MixedValidationCode::kMalformedAdjacency,
                   "mixed telemetry identities are inconsistent");
  return {};
}

MixedValidationResult validateMixedKahnLevelization(
    const MixedPrecedenceGraph& graph,
    const MixedKahnLevelization& levels)
{
  const MixedValidationResult graph_validation =
      validateMixedPrecedenceGraph(graph);
  if (!graph_validation.valid())
    return graph_validation;
  const std::uint64_t count = graph.domain.mixed_vertex_count;
  if (levels.level_offsets.empty() || levels.level_offsets.front() != 0
      || levels.level_offsets.back() != count
      || levels.level_vertices.size() != count
      || levels.vertex_to_level.size() != count
      || levels.processed_count != count)
    return invalid(MixedValidationCode::kMalformedLevelization,
                   "mixed Kahn result does not completely cover vertices");
  if (count == 0 && levels.level_offsets.size() != 1)
    return invalid(MixedValidationCode::kMalformedLevelization,
                   "empty mixed graph must have offsets {0}");

  std::vector<std::uint8_t> seen(static_cast<std::size_t>(count), 0);
  std::uint64_t observed_max_frontier = 0;
  for (std::size_t level = 0; level + 1 < levels.level_offsets.size();
       ++level) {
    const std::uint64_t begin = levels.level_offsets[level];
    const std::uint64_t end = levels.level_offsets[level + 1];
    if (begin >= end || end > count)
      return invalid(MixedValidationCode::kMalformedLevelization,
                     "mixed Kahn levels must be nonempty and in range");
    observed_max_frontier = std::max(observed_max_frontier, end - begin);
    for (std::uint64_t position = begin; position < end; ++position) {
      const MixedVertexId vertex =
          levels.level_vertices[static_cast<std::size_t>(position)];
      if (vertex.value() >= count
          || seen[static_cast<std::size_t>(vertex.value())] != 0)
        return invalid(MixedValidationCode::kMalformedLevelization,
                       "mixed Kahn vertices are not a permutation");
      seen[static_cast<std::size_t>(vertex.value())] = 1;
      if (levels.vertex_to_level[
              static_cast<std::size_t>(vertex.value())]
          != static_cast<std::uint64_t>(level) + 1)
        return invalid(MixedValidationCode::kMalformedLevelization,
                       "mixed Kahn inverse map disagrees with offsets");
    }
  }
  if (observed_max_frontier != levels.max_frontier)
    return invalid(MixedValidationCode::kMalformedLevelization,
                   "mixed Kahn max-frontier telemetry is incorrect");
  for (std::size_t source = 0; source < graph.indegree.size(); ++source) {
    const std::uint64_t source_level = levels.vertex_to_level[source];
    for (std::uint64_t index = graph.adjacency_offsets[source];
         index < graph.adjacency_offsets[source + 1]; ++index) {
      const std::uint64_t sink =
          graph.successors[static_cast<std::size_t>(index)].value();
      if (source_level
          >= levels.vertex_to_level[static_cast<std::size_t>(sink)])
        return invalid(MixedValidationCode::kMalformedLevelization,
                       "mixed precedence edge does not cross levels");
    }
  }
  return {};
}

}  // namespace ista::timing_incremental_levelization
