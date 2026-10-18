// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "MixedLevelReconstructor.hpp"

#include <limits>
#include <vector>

#include "MixedLevelizationInternal.hpp"

namespace ista::timing_incremental_levelization {
namespace {

MixedReconstructionFailure failure(MixedReconstructionFailureCode code,
                                   std::string detail)
{
  return {code, std::move(detail)};
}

}  // namespace

MixedReconstructionAttempt reconstructMixedLevelization(
    const MixedIdDomain& domain,
    const csr::CsrFifoLevelization& previous,
    const MixedKahnLevelization& mixed)
{
  const auto expected_domain = makeMixedIdDomain(
      domain.n_last_nodes, domain.n_current_nodes, domain.n_old_levels);
  const auto* expected = std::get_if<MixedIdDomain>(&expected_domain);
  if (expected == nullptr || *expected != domain)
    return failure(MixedReconstructionFailureCode::kMappingOverflow,
                   "reconstruction received an inconsistent mixed domain");
  std::string previous_detail;
  const bool previous_valid = detail::validatePreviousLevelizationShape(previous, domain.n_last_nodes, previous_detail);
  if (!previous_valid || previous.level_offsets.size() - 1 != domain.n_old_levels)
    return failure(MixedReconstructionFailureCode::kMalformedPreviousLevels,
                   std::move(previous_detail));
  if (domain.n_current_nodes
      > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    return failure(MixedReconstructionFailureCode::kMappingOverflow,
                   "current actual node domain is not addressable");
  const std::size_t mixed_count =
      static_cast<std::size_t>(domain.mixed_vertex_count);
  if (mixed.level_offsets.empty() || mixed.level_offsets.front() != 0
      || mixed.level_offsets.back() != domain.mixed_vertex_count
      || mixed.level_vertices.size() != mixed_count
      || mixed.vertex_to_level.size() != mixed_count
      || mixed.processed_count != domain.mixed_vertex_count)
    return failure(MixedReconstructionFailureCode::kInvariantFailure,
                   "mixed levelization is incomplete or malformed");
  if (mixed_count == 0 && mixed.level_offsets.size() != 1)
    return failure(MixedReconstructionFailureCode::kInvariantFailure,
                   "empty mixed result must use offsets {0}");

  std::vector<std::uint8_t> mixed_seen(mixed_count, 0);
  const std::size_t actual_count =
      static_cast<std::size_t>(domain.n_current_nodes);
  std::vector<std::uint8_t> actual_seen(actual_count, 0);
  csr::CsrFifoLevelization output;
  output.level_offsets.clear();
  output.level_offsets.push_back(0);
  output.level_nodes.reserve(actual_count);
  output.node_to_level.assign(actual_count, 0);

  for (std::size_t level = 0; level + 1 < mixed.level_offsets.size();
       ++level) {
    const std::uint64_t begin = mixed.level_offsets[level];
    const std::uint64_t end = mixed.level_offsets[level + 1];
    if (begin >= end || end > domain.mixed_vertex_count)
      return failure(MixedReconstructionFailureCode::kInvariantFailure,
                     "mixed levels must be nonempty and in range");
    std::uint64_t virtual_markers = 0;
    std::optional<MixedVertexId> previous_vertex;
    const std::uint64_t output_level =
        static_cast<std::uint64_t>(level) + 1;
    for (std::uint64_t position = begin; position < end; ++position) {
      const MixedVertexId vertex =
          mixed.level_vertices[static_cast<std::size_t>(position)];
      if (vertex.value() >= domain.mixed_vertex_count
          || mixed_seen[static_cast<std::size_t>(vertex.value())] != 0)
        return failure(MixedReconstructionFailureCode::kInvariantFailure,
                       "mixed vertices are not a complete permutation");
      if (previous_vertex.has_value()
          && !(*previous_vertex < vertex))
        return failure(MixedReconstructionFailureCode::kInvariantFailure,
                       "mixed frontier is not in deterministic ID order");
      previous_vertex = vertex;
      mixed_seen[static_cast<std::size_t>(vertex.value())] = 1;
      if (mixed.vertex_to_level[static_cast<std::size_t>(vertex.value())]
          != output_level)
        return failure(MixedReconstructionFailureCode::kInvariantFailure,
                       "mixed inverse map disagrees with its offsets");

      if (isVirtualOldLevel(vertex, domain)) {
        if (!detail::checkedIncrement(virtual_markers)
            || virtual_markers > 1)
          return failure(MixedReconstructionFailureCode::kInvariantFailure,
                         "one mixed level contains multiple old-level markers");
        const auto decoded = virtualLevelForMixedVertex(vertex, domain);
        const auto* old_level = std::get_if<VirtualOldLevel>(&decoded);
        if (old_level == nullptr)
          return failure(MixedReconstructionFailureCode::kMappingOverflow,
                         "virtual marker cannot be decoded");
        const std::size_t old_index =
            static_cast<std::size_t>(old_level->value());
        const std::uint64_t old_begin = previous.level_offsets[old_index];
        const std::uint64_t old_end = previous.level_offsets[old_index + 1];
        if (old_begin >= old_end || old_end > previous.level_nodes.size())
          return failure(MixedReconstructionFailureCode::kInvariantFailure, "old-level row escapes its node permutation");
        for (std::uint64_t old_position = old_begin;
             old_position < old_end; ++old_position) {
          const csr::NodeId actual = previous.level_nodes[
              static_cast<std::size_t>(old_position)];
          const std::size_t actual_index =
              static_cast<std::size_t>(actual.value());
          if (actual.value() >= domain.n_last_nodes
              || actual_seen[actual_index] != 0)
            return failure(MixedReconstructionFailureCode::kInvariantFailure,
                           "old-level expansion duplicates or escapes old nodes");
          actual_seen[actual_index] = 1;
          output.level_nodes.push_back(actual);
          output.node_to_level[actual_index] = output_level;
        }
      } else if (isNewActualNode(vertex, domain)) {
        const auto decoded = newNodeForMixedVertex(vertex, domain);
        const auto* new_node = std::get_if<NewActualNode>(&decoded);
        if (new_node == nullptr)
          return failure(MixedReconstructionFailureCode::kMappingOverflow,
                         "new-node marker cannot be decoded");
        const csr::NodeId actual = new_node->value();
        const std::size_t actual_index =
            static_cast<std::size_t>(actual.value());
        if (actual.value() < domain.n_last_nodes
            || actual.value() >= domain.n_current_nodes
            || actual_seen[actual_index] != 0)
          return failure(MixedReconstructionFailureCode::kInvariantFailure,
                         "new-node expansion duplicates or escapes new nodes");
        actual_seen[actual_index] = 1;
        output.level_nodes.push_back(actual);
        output.node_to_level[actual_index] = output_level;
      } else {
        return failure(MixedReconstructionFailureCode::kInvariantFailure,
                       "mixed vertex has no valid tag interpretation");
      }
    }
    output.level_offsets.push_back(
        static_cast<std::uint64_t>(output.level_nodes.size()));
  }

  for (std::uint8_t seen : mixed_seen)
    if (seen == 0)
      return failure(MixedReconstructionFailureCode::kInvariantFailure,
                     "reconstruction omits a mixed vertex");
  for (std::uint8_t seen : actual_seen)
    if (seen == 0)
      return failure(MixedReconstructionFailureCode::kInvariantFailure,
                     "reconstruction omits an actual node");
  if (output.level_nodes.size() != actual_count
      || output.level_offsets.back() != domain.n_current_nodes)
    return failure(MixedReconstructionFailureCode::kInvariantFailure,
                   "reconstructed levels do not cover current nodes");
  output.complete = true;
  return output;
}

}  // namespace ista::timing_incremental_levelization
