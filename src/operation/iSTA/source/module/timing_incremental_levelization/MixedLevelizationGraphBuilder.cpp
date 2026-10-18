// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "MixedLevelizationGraphBuilder.hpp"

#include <limits>
#include <set>
#include <utility>

#include "IncreGpuStaBuildConfig.hpp"
#include "MixedLevelizationInternal.hpp"
#include "MixedLevelizationValidation.hpp"
#include "MixedTopologyInternal.hpp"

namespace ista::timing_incremental_levelization {
namespace {

bool recordDiagnosticVisit(std::uint64_t& value) noexcept
{
  if constexpr (!incregpusta_debug_checks_enabled)
    return true;
  return detail::checkedIncrement(value);
}

using EndpointPair = std::pair<std::uint64_t, std::uint64_t>;

MixedGraphBuildFailure makeFailure(
    MixedGraphBuildFailureCode code, std::string detail,
    const MixedLevelizationTelemetry& telemetry,
    std::optional<csr::CsrViewError> view_error = std::nullopt)
{
  return {code, view_error, std::move(detail), telemetry};
}

bool addMappedPair(const detail::TopologyArc& arc, const detail::PreviousLevels& previous, const MixedIdDomain& domain, std::set<EndpointPair>& mapped_pairs)
{
  const bool source_old = arc.source.value() < domain.n_last_nodes;
  const bool sink_old = arc.sink.value() < domain.n_last_nodes;
  MixedVertexMappingAttempt source_mixed{
      MixedMappingFailure{MixedMappingFailureCode::kMalformedDomain, {}}};
  MixedVertexMappingAttempt sink_mixed{
      MixedMappingFailure{MixedMappingFailureCode::kMalformedDomain, {}}};

  if (source_old) {
    const auto source_level = detail::oldNodeLevel(arc.source, previous, domain);
    const auto* level = std::get_if<VirtualOldLevel>(&source_level);
    if (level == nullptr)
      return false;
    source_mixed = mixedVertexForVirtualLevel(*level, domain);
  } else {
    source_mixed = mixedVertexForNewNode(NewActualNode{arc.source}, domain);
  }
  if (sink_old) {
    const auto sink_level = detail::oldNodeLevel(arc.sink, previous, domain);
    const auto* level = std::get_if<VirtualOldLevel>(&sink_level);
    if (level == nullptr)
      return false;
    sink_mixed = mixedVertexForVirtualLevel(*level, domain);
  } else {
    sink_mixed = mixedVertexForNewNode(NewActualNode{arc.sink}, domain);
  }

  const auto* source = std::get_if<MixedVertexId>(&source_mixed);
  const auto* sink = std::get_if<MixedVertexId>(&sink_mixed);
  if (source == nullptr || sink == nullptr)
    return false;
  mapped_pairs.emplace(source->value(), sink->value());
  return true;
}

template <bool ValidatePrevious>
MixedGraphBuildAttempt buildGraph(const detail::TopologyTraversal& edges, const detail::PreviousLevels& previous, const MixedLevelizationRequest& request)
{
  MixedLevelizationTelemetry telemetry;
  telemetry.n_last_nodes = request.n_last_nodes;
  telemetry.n_current_nodes = static_cast<std::uint64_t>(edges.node_count);

  std::uint64_t expected_generation = 0;
  if (!detail::checkedAdd(request.generation_before, 1,
                          expected_generation)
      || request.generation_after != expected_generation)
    return makeFailure(MixedGraphBuildFailureCode::kInvariantFailure,
                       "generation binding is not consecutive", telemetry);
  if (request.n_last_nodes > telemetry.n_current_nodes)
    return makeFailure(MixedGraphBuildFailureCode::kMappingOverflow,
                       "old-node high-water exceeds current node count",
                       telemetry);

  std::string previous_detail;
  const bool previous_valid = ValidatePrevious ? detail::validatePreviousLevelizationShape(previous, request.n_last_nodes, previous_detail)
                                               : detail::validatePreviousLevelMappingDimensions(previous, request.n_last_nodes, previous_detail);
  if (!previous_valid)
    return makeFailure(MixedGraphBuildFailureCode::kMalformedPreviousLevels,
                       std::move(previous_detail), telemetry);

  const std::uint64_t n_old_levels =
      static_cast<std::uint64_t>(previous.level_offsets.size() - 1);
  const auto domain_attempt = makeMixedIdDomain(
      request.n_last_nodes, telemetry.n_current_nodes, n_old_levels);
  const auto* domain = std::get_if<MixedIdDomain>(&domain_attempt);
  if (domain == nullptr)
    return makeFailure(MixedGraphBuildFailureCode::kMappingOverflow,
                       "mixed ID domain cannot be represented", telemetry);
  telemetry.n_new_nodes = domain->n_new_nodes;
  telemetry.n_old_levels = domain->n_old_levels;
  telemetry.mixed_vertex_count = domain->mixed_vertex_count;
  if (domain->mixed_vertex_count
      >= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    return makeFailure(MixedGraphBuildFailureCode::kMappingOverflow,
                       "mixed graph is not addressable", telemetry);

  std::set<EndpointPair> actual_pairs;
  std::set<EndpointPair> mapped_pairs;
  bool counter_overflow = false;
  bool mapping_error = false;
  bool old_old_conflict = false;
  const csr::EdgeVisitResult visit = edges.visit([&](const detail::TopologyArc& arc) {
    if (arc.source.value() >= domain->n_current_nodes || arc.sink.value() >= domain->n_current_nodes)
      return csr::EdgeVisitResult::fail(csr::CsrViewError::kNodeOutOfRange);
    if (!arc.levelization_eligible)
      return csr::EdgeVisitResult::continueVisit();
    if (arc.storage_kind == csr::EdgeStorageKind::kBaseOut) {
      counter_overflow |= !recordDiagnosticVisit(telemetry.base_out);
    } else if (arc.storage_kind == csr::EdgeStorageKind::kAuxOut) {
      counter_overflow |= !recordDiagnosticVisit(telemetry.aux_out);
    } else {
      return csr::EdgeVisitResult::fail(csr::CsrViewError::kMalformedAdjacency);
    }
    counter_overflow |= !recordDiagnosticVisit(telemetry.actual_eligible_arc_visits);

    const bool source_old = arc.source.value() < domain->n_last_nodes;
    const bool sink_old = arc.sink.value() < domain->n_last_nodes;
    if (source_old && sink_old) {
      counter_overflow |= !recordDiagnosticVisit(telemetry.old_old);
      const std::uint64_t source_level = previous.levelOf(static_cast<std::size_t>(arc.source.value()));
      const std::uint64_t sink_level = previous.levelOf(static_cast<std::size_t>(arc.sink.value()));
      if (source_level == 0 || source_level > domain->n_old_levels || sink_level == 0 || sink_level > domain->n_old_levels)
        return csr::EdgeVisitResult::fail(csr::CsrViewError::kNodeOutOfRange);
      old_old_conflict |= source_level >= sink_level;
      return csr::EdgeVisitResult::continueVisit();
    }

    if (source_old) {
      counter_overflow |= !recordDiagnosticVisit(telemetry.old_new);
    } else if (sink_old) {
      counter_overflow |= !recordDiagnosticVisit(telemetry.new_old);
    } else {
      counter_overflow |= !recordDiagnosticVisit(telemetry.new_new);
    }
    counter_overflow |= !recordDiagnosticVisit(telemetry.mapped_actual_arc_candidates);

    if constexpr (incregpusta_debug_checks_enabled) {
      const EndpointPair actual{arc.source.value(), arc.sink.value()};
      if (!actual_pairs.insert(actual).second) {
        counter_overflow |= !recordDiagnosticVisit(telemetry.actual_parallel_duplicates_collapsed);
        return csr::EdgeVisitResult::continueVisit();
      }
    }
    mapping_error |= !addMappedPair(arc, previous, *domain, mapped_pairs);
    return mapping_error ? csr::EdgeVisitResult::fail(csr::CsrViewError::kCallbackError) : csr::EdgeVisitResult::continueVisit();
  });

  if constexpr (incregpusta_debug_checks_enabled) {
    telemetry.unique_actual_endpoint_pairs = static_cast<std::uint64_t>(actual_pairs.size());
    if (actual_pairs.size() < mapped_pairs.size())
      counter_overflow = true;
    else
      telemetry.proxy_alias_duplicates_collapsed = static_cast<std::uint64_t>(actual_pairs.size() - mapped_pairs.size());
  }

  if (mapping_error)
    return makeFailure(MixedGraphBuildFailureCode::kMappingOverflow,
                       "an actual endpoint could not be mapped", telemetry);
  if (counter_overflow)
    return makeFailure(MixedGraphBuildFailureCode::kInvariantFailure,
                       "mixed builder telemetry counter overflow", telemetry);
  if (visit.control != csr::EdgeVisitControl::kContinue)
    return makeFailure(
        MixedGraphBuildFailureCode::kViewError,
        "active edge traversal failed or stopped unexpectedly", telemetry,
        visit.error.value_or(csr::CsrViewError::kCallbackError));
  if (old_old_conflict)
    return makeFailure(MixedGraphBuildFailureCode::kOldOldOrderConflict,
                       "current old-to-old precedence violates previous order",
                       telemetry);

  std::set<EndpointPair> precedence;
  for (std::uint64_t level = 1; level < domain->n_old_levels; ++level) {
    precedence.emplace(level - 1, level);
    if (!recordDiagnosticVisit(telemetry.virtual_chain_edges))
      return makeFailure(MixedGraphBuildFailureCode::kInvariantFailure,
                         "virtual chain edge counter overflow", telemetry);
  }
  precedence.insert(mapped_pairs.begin(), mapped_pairs.end());
  telemetry.unique_precedence_edges =
      static_cast<std::uint64_t>(precedence.size());

  MixedPrecedenceGraph graph;
  graph.domain = *domain;
  graph.generation_before = request.generation_before;
  graph.generation_after = request.generation_after;
  graph.telemetry = telemetry;
  const std::size_t vertex_count =
      static_cast<std::size_t>(domain->mixed_vertex_count);
  graph.adjacency_offsets.clear();
  graph.adjacency_offsets.reserve(vertex_count + 1);
  graph.adjacency_offsets.push_back(0);
  graph.indegree.assign(vertex_count, 0);
  graph.successors.reserve(precedence.size());

  auto edge = precedence.begin();
  for (std::size_t source = 0; source < vertex_count; ++source) {
    while (edge != precedence.end()
           && edge->first == static_cast<std::uint64_t>(source)) {
      if (edge->second >= domain->mixed_vertex_count)
        return makeFailure(MixedGraphBuildFailureCode::kInvariantFailure,
                           "mapped successor escapes mixed domain", telemetry);
      graph.successors.emplace_back(edge->second);
      std::uint64_t& degree =
          graph.indegree[static_cast<std::size_t>(edge->second)];
      if (!detail::checkedIncrement(degree))
        return makeFailure(MixedGraphBuildFailureCode::kInvariantFailure,
                           "mixed indegree overflows uint64_t", telemetry);
      ++edge;
    }
    graph.adjacency_offsets.push_back(
        static_cast<std::uint64_t>(graph.successors.size()));
  }
  if (edge != precedence.end())
    return makeFailure(MixedGraphBuildFailureCode::kInvariantFailure,
                       "mapped source escapes mixed domain", telemetry);

  if constexpr (incregpusta_debug_checks_enabled) {
    const MixedValidationResult validation = validateMixedPrecedenceGraph(graph);
    if (!validation.valid())
      return makeFailure(MixedGraphBuildFailureCode::kInvariantFailure, validation.detail, telemetry);
  }
  return graph;
}

}  // namespace

MixedGraphBuildAttempt buildMixedLevelizationGraph(const csr::ActiveCsrEdgeView& edges, const csr::CsrFifoLevelization& previous,
                                                   const MixedLevelizationRequest& request)
{
  return buildGraph<true>(detail::topologyTraversal(edges), previous, request);
}

MixedGraphBuildAttempt detail::buildOwnedMixedGraph(const TopologyTraversal& edges, const detail::PreviousLevels& previous,
                                                    const MixedLevelizationRequest& request)
{
  return buildGraph<false>(edges, previous, request);
}

}  // namespace ista::timing_incremental_levelization
