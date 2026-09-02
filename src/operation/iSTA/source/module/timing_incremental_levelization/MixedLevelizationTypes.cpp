// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "MixedLevelizationTypes.hpp"

#include <limits>

#include "MixedLevelizationInternal.hpp"
#include "MixedLevelizationTelemetry.hpp"

namespace ista::timing_incremental_levelization {
namespace {

MixedMappingFailure failure(MixedMappingFailureCode code, std::string detail)
{
  return {code, std::move(detail)};
}

}  // namespace

MixedIdDomainAttempt makeMixedIdDomain(std::uint64_t n_last_nodes,
                                       std::uint64_t n_current_nodes,
                                       std::uint64_t n_old_levels)
{
  if (n_current_nodes < n_last_nodes)
    return failure(MixedMappingFailureCode::kMalformedDomain,
                   "current node count precedes old-node high-water");
  if (n_old_levels > n_last_nodes)
    return failure(MixedMappingFailureCode::kMalformedDomain,
                   "old level count exceeds old node count");

  const std::uint64_t n_new_nodes = n_current_nodes - n_last_nodes;
  std::uint64_t mixed_vertex_count = 0;
  if (!detail::checkedAdd(n_old_levels, n_new_nodes,
                          mixed_vertex_count))
    return failure(MixedMappingFailureCode::kOverflow,
                   "mixed vertex count overflows uint64_t");
  return MixedIdDomain{n_last_nodes, n_current_nodes, n_new_nodes,
                       n_old_levels, mixed_vertex_count};
}

bool isVirtualOldLevel(MixedVertexId vertex,
                       const MixedIdDomain& domain) noexcept
{
  return vertex.value() < domain.n_old_levels
         && vertex.value() < domain.mixed_vertex_count;
}

bool isNewActualNode(MixedVertexId vertex,
                     const MixedIdDomain& domain) noexcept
{
  return vertex.value() >= domain.n_old_levels
         && vertex.value() < domain.mixed_vertex_count;
}

MixedVertexMappingAttempt mixedVertexForVirtualLevel(
    VirtualOldLevel level, const MixedIdDomain& domain)
{
  if (level.value() >= domain.n_old_levels)
    return failure(MixedMappingFailureCode::kVirtualLevelOutOfRange,
                   "virtual old level is outside the mixed domain");
  return MixedVertexId{level.value()};
}

MixedVertexMappingAttempt mixedVertexForNewNode(
    NewActualNode node, const MixedIdDomain& domain)
{
  if (node.value().value() < domain.n_last_nodes
      || node.value().value() >= domain.n_current_nodes)
    return failure(MixedMappingFailureCode::kNewNodeOutOfRange,
                   "new actual node is outside the batch suffix");
  const std::uint64_t suffix = node.value().value() - domain.n_last_nodes;
  std::uint64_t mixed = 0;
  if (!detail::checkedAdd(domain.n_old_levels, suffix, mixed))
    return failure(MixedMappingFailureCode::kOverflow,
                   "new-node mixed mapping overflows uint64_t");
  if (mixed >= domain.mixed_vertex_count)
    return failure(MixedMappingFailureCode::kMalformedDomain,
                   "new-node mapping escapes mixed vertex count");
  return MixedVertexId{mixed};
}

ActualNodeMappingAttempt newNodeForMixedVertex(
    MixedVertexId vertex, const MixedIdDomain& domain)
{
  if (vertex.value() >= domain.mixed_vertex_count)
    return failure(MixedMappingFailureCode::kMixedVertexOutOfRange,
                   "mixed vertex is outside the mixed domain");
  if (vertex.value() < domain.n_old_levels)
    return failure(MixedMappingFailureCode::kWrongMixedVertexKind,
                   "virtual old level cannot be decoded as a new node");
  const std::uint64_t suffix = vertex.value() - domain.n_old_levels;
  std::uint64_t actual = 0;
  if (!detail::checkedAdd(domain.n_last_nodes, suffix, actual))
    return failure(MixedMappingFailureCode::kOverflow,
                   "mixed-to-actual mapping overflows uint64_t");
  if (actual >= domain.n_current_nodes)
    return failure(MixedMappingFailureCode::kMalformedDomain,
                   "decoded actual node escapes current node count");
  return NewActualNode{csr::NodeId{actual}};
}

VirtualLevelMappingAttempt virtualLevelForMixedVertex(
    MixedVertexId vertex, const MixedIdDomain& domain)
{
  if (vertex.value() >= domain.mixed_vertex_count)
    return failure(MixedMappingFailureCode::kMixedVertexOutOfRange,
                   "mixed vertex is outside the mixed domain");
  if (vertex.value() >= domain.n_old_levels)
    return failure(MixedMappingFailureCode::kWrongMixedVertexKind,
                   "new-node vertex cannot be decoded as an old level");
  return VirtualOldLevel{vertex.value()};
}

VirtualLevelMappingAttempt virtualLevelForOldNode(
    csr::NodeId node, const csr::CsrFifoLevelization& previous,
    const MixedIdDomain& domain)
{
  if (node.value() >= domain.n_last_nodes)
    return failure(MixedMappingFailureCode::kOldNodeOutOfRange,
                   "old actual node is outside the committed prefix");
  if (domain.n_last_nodes > std::numeric_limits<std::size_t>::max()
      || previous.node_to_level.size()
             != static_cast<std::size_t>(domain.n_last_nodes))
    return failure(MixedMappingFailureCode::kMalformedPreviousLevels,
                   "previous inverse map does not cover old nodes");
  const std::uint64_t one_based =
      previous.node_to_level[static_cast<std::size_t>(node.value())];
  if (one_based == 0 || one_based > domain.n_old_levels)
    return failure(MixedMappingFailureCode::kMalformedPreviousLevels,
                   "old node has an invalid previous level");
  return VirtualOldLevel{one_based - 1};
}

bool validateMixedTelemetryIdentities(
    const MixedLevelizationTelemetry& telemetry) noexcept
{
  std::uint64_t category_sum = 0;
  std::uint64_t storage_sum = 0;
  std::uint64_t expected = 0;
  if (!detail::checkedAdd(telemetry.old_old, telemetry.old_new, category_sum)
      || !detail::checkedAdd(category_sum, telemetry.new_old, category_sum)
      || !detail::checkedAdd(category_sum, telemetry.new_new, category_sum)
      || category_sum != telemetry.actual_eligible_arc_visits)
    return false;
  if (!detail::checkedAdd(telemetry.base_out, telemetry.aux_out, storage_sum)
      || storage_sum != telemetry.actual_eligible_arc_visits)
    return false;
  if (!detail::checkedAdd(telemetry.old_new, telemetry.new_old, expected)
      || !detail::checkedAdd(expected, telemetry.new_new, expected)
      || expected != telemetry.mapped_actual_arc_candidates)
    return false;
  if (!detail::checkedAdd(telemetry.unique_actual_endpoint_pairs,
                          telemetry.actual_parallel_duplicates_collapsed,
                          expected)
      || expected != telemetry.mapped_actual_arc_candidates)
    return false;
  if (telemetry.unique_precedence_edges < telemetry.virtual_chain_edges)
    return false;
  const std::uint64_t unique_mapped =
      telemetry.unique_precedence_edges - telemetry.virtual_chain_edges;
  if (!detail::checkedAdd(telemetry.proxy_alias_duplicates_collapsed,
                          unique_mapped, expected)
      || expected != telemetry.unique_actual_endpoint_pairs)
    return false;
  if (!detail::checkedAdd(telemetry.n_old_levels,
                          telemetry.n_new_nodes, expected)
      || expected != telemetry.mixed_vertex_count)
    return false;
  const std::uint64_t expected_chain =
      telemetry.n_old_levels == 0 ? 0 : telemetry.n_old_levels - 1;
  if (telemetry.virtual_chain_edges != expected_chain)
    return false;
  return telemetry.n_current_nodes >= telemetry.n_last_nodes
         && telemetry.n_new_nodes
                == telemetry.n_current_nodes - telemetry.n_last_nodes
         && telemetry.fallback_count == 0;
}

}  // namespace ista::timing_incremental_levelization
