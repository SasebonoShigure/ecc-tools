// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ActualDagCycleClassifier.hpp"
#include "IncrementalLevelizationTestSupport.hpp"
#include "MixedLevelizationValidation.hpp"

namespace ista::timing_incremental_levelization::test {
namespace {

using EndpointPair = std::pair<std::uint64_t, std::uint64_t>;

struct GeneratedCase
{
  csr::CsrFifoLevelization previous;
  TestEdgeView edges;
  MixedLevelizationRequest request;
};

std::string previousSummary(const csr::CsrFifoLevelization& previous)
{
  std::ostringstream out;
  out << "levels=";
  for (std::size_t level = 0; level + 1 < previous.level_offsets.size();
       ++level) {
    if (level != 0)
      out << '|';
    out << '[';
    for (std::uint64_t position = previous.level_offsets[level];
         position < previous.level_offsets[level + 1]; ++position) {
      if (position != previous.level_offsets[level])
        out << ',';
      out << previous.level_nodes[static_cast<std::size_t>(position)].value();
    }
    out << ']';
  }
  return out.str();
}

std::string actualGraphSummary(const TestEdgeView& edges)
{
  std::ostringstream out;
  out << "nodes=" << edges.nodeCount() << ",arcs=";
  const auto visit = edges.forEachActiveArc([&](const csr::EdgeEntry& entry) {
    const auto payload = edges.arc(entry.arc_id);
    if (!payload)
      return csr::EdgeVisitResult::fail(payload.error());
    const auto& arc = payload.value();
    out << entry.arc_id.value() << ':' << arc.source.value() << "->"
        << arc.sink.value() << ':' << (arc.eligibility.levelization ? 'E' : 'I')
        << ':' << (entry.storage_kind == csr::EdgeStorageKind::kAuxOut ? 'A' : 'B') << ';';
    return csr::EdgeVisitResult::continueVisit();
  });
  require(visit.control == csr::EdgeVisitControl::kContinue,
          "cannot summarize the active edge view");
  return out.str();
}

std::uint64_t mixedEndpoint(const csr::NodeId node,
                            const csr::CsrFifoLevelization& previous,
                            std::uint64_t n_last_nodes)
{
  const std::uint64_t old_levels = previous.level_offsets.size() - 1;
  return node.value() < n_last_nodes
             ? previous.node_to_level[static_cast<std::size_t>(node.value())]
                   - 1
             : old_levels + node.value() - n_last_nodes;
}

std::string mixedGraphSummary(const TestEdgeView& edges,
                              const csr::CsrFifoLevelization& previous,
                              std::uint64_t n_last_nodes)
{
  std::set<EndpointPair> mapped;
  std::uint64_t old_old = 0;
  std::uint64_t eligible = 0;
  const csr::EdgeVisitResult visit =
      edges.forEachActiveArc([&](const csr::EdgeEntry& entry) {
        const auto payload = edges.arc(entry.arc_id);
        if (!payload)
          return csr::EdgeVisitResult::fail(payload.error());
        const auto& arc = payload.value();
        if (!arc.eligibility.levelization)
          return csr::EdgeVisitResult::continueVisit();
        ++eligible;
        if (arc.source.value() < n_last_nodes
            && arc.sink.value() < n_last_nodes) {
          ++old_old;
        } else {
          mapped.emplace(mixedEndpoint(arc.source, previous, n_last_nodes),
                         mixedEndpoint(arc.sink, previous, n_last_nodes));
        }
        return csr::EdgeVisitResult::continueVisit();
      });
  require(visit.control == csr::EdgeVisitControl::kContinue,
          "cannot summarize the mixed graph");
  std::ostringstream out;
  out << "old_levels=" << previous.level_offsets.size() - 1
      << ",eligible=" << eligible << ",old_old=" << old_old
      << ",mapped=";
  for (const EndpointPair& edge : mapped)
    out << edge.first << "->" << edge.second << ';';
  return out.str();
}

std::string caseContext(std::uint64_t seed, std::uint64_t sample,
                        const GeneratedCase& generated)
{
  std::ostringstream out;
  out << "seed=" << seed << " sample=" << sample << ' '
      << previousSummary(generated.previous) << " actual={"
      << actualGraphSummary(generated.edges) << "} mixed={"
      << mixedGraphSummary(generated.edges, generated.previous,
                           generated.request.n_last_nodes)
      << '}';
  return out.str();
}

bool independentlyAcyclic(const TestEdgeView& edges)
{
  std::vector<std::vector<std::size_t>> adjacency(edges.nodeCount());
  std::vector<std::uint64_t> indegree(edges.nodeCount(), 0);
  const auto visit = edges.forEachActiveArc([&](const csr::EdgeEntry& entry) {
    const auto payload = edges.arc(entry.arc_id);
    if (!payload)
      return csr::EdgeVisitResult::fail(payload.error());
    const auto& arc = payload.value();
    if (!arc.eligibility.levelization)
      return csr::EdgeVisitResult::continueVisit();
    if (arc.source.value() >= edges.nodeCount()
        || arc.sink.value() >= edges.nodeCount())
      return csr::EdgeVisitResult::fail(csr::CsrViewError::kNodeOutOfRange);
    adjacency[static_cast<std::size_t>(arc.source.value())].push_back(
        static_cast<std::size_t>(arc.sink.value()));
    ++indegree[static_cast<std::size_t>(arc.sink.value())];
    return csr::EdgeVisitResult::continueVisit();
  });
  require(visit.control == csr::EdgeVisitControl::kContinue,
          "independent DAG oracle saw a view error");
  std::queue<std::size_t> ready;
  for (std::size_t node = 0; node < edges.nodeCount(); ++node)
    if (indegree[node] == 0)
      ready.push(node);
  std::size_t processed = 0;
  while (!ready.empty()) {
    const std::size_t source = ready.front();
    ready.pop();
    ++processed;
    for (std::size_t sink : adjacency[source])
      if (--indegree[sink] == 0)
        ready.push(sink);
  }
  return processed == edges.nodeCount();
}

void requireIndependentGraph(
    const TestEdgeView& edges, const csr::CsrFifoLevelization& previous,
    std::uint64_t n_last_nodes, const MixedPrecedenceGraph& graph)
{
  const auto& actual = graph.telemetry;
  std::uint64_t eligible = 0;
  std::uint64_t old_old = 0;
  std::uint64_t old_new = 0;
  std::uint64_t new_old = 0;
  std::uint64_t new_new = 0;
  std::uint64_t base_out = 0;
  std::uint64_t aux_out = 0;
  std::uint64_t mapped_candidates = 0;
  std::uint64_t parallel_duplicates = 0;
  std::set<EndpointPair> actual_pairs;
  std::set<EndpointPair> mapped_pairs;
  const csr::EdgeVisitResult visit =
      edges.forEachActiveArc([&](const csr::EdgeEntry& entry) {
        const auto payload = edges.arc(entry.arc_id);
        if (!payload)
          return csr::EdgeVisitResult::fail(payload.error());
        const auto& arc = payload.value();
        if (!arc.eligibility.levelization)
          return csr::EdgeVisitResult::continueVisit();
        ++eligible;
        entry.storage_kind == csr::EdgeStorageKind::kAuxOut ? ++aux_out
                                                             : ++base_out;
        const bool source_old = arc.source.value() < n_last_nodes;
        const bool sink_old = arc.sink.value() < n_last_nodes;
        if (source_old && sink_old) {
          ++old_old;
          return csr::EdgeVisitResult::continueVisit();
        }
        source_old ? ++old_new : sink_old ? ++new_old : ++new_new;
        ++mapped_candidates;
        const EndpointPair actual_pair{arc.source.value(), arc.sink.value()};
        if (!actual_pairs.insert(actual_pair).second) {
          ++parallel_duplicates;
          return csr::EdgeVisitResult::continueVisit();
        }
        mapped_pairs.emplace(
            mixedEndpoint(arc.source, previous, n_last_nodes),
            mixedEndpoint(arc.sink, previous, n_last_nodes));
        return csr::EdgeVisitResult::continueVisit();
      });
  require(visit.control == csr::EdgeVisitControl::kContinue,
          "independent telemetry oracle saw a view error");
  const std::uint64_t old_levels = previous.level_offsets.size() - 1;
  std::set<EndpointPair> precedence = mapped_pairs;
  for (std::uint64_t level = 1; level < old_levels; ++level)
    precedence.emplace(level - 1, level);
  const std::uint64_t vertices = old_levels + edges.nodeCount() - n_last_nodes;
  require(graph.domain == MixedIdDomain{n_last_nodes, edges.nodeCount(),
                                        edges.nodeCount() - n_last_nodes, old_levels, vertices},
          "mixed domain differs from the independent model");
  std::vector<std::uint64_t> offsets{0};
  std::vector<MixedVertexId> successors;
  std::vector<std::uint64_t> indegree(vertices, 0);
  auto edge = precedence.begin();
  for (std::uint64_t source = 0; source < vertices; ++source) {
    while (edge != precedence.end() && edge->first == source) {
      successors.emplace_back(edge->second);
      ++indegree[edge->second];
      ++edge;
    }
    offsets.push_back(successors.size());
  }
  require(graph.adjacency_offsets == offsets && graph.successors == successors
              && graph.indegree == indegree,
          "mixed adjacency differs from independent endpoint constraints");
  const std::uint64_t proxy_aliases =
      static_cast<std::uint64_t>(actual_pairs.size() - mapped_pairs.size());
  require(actual.n_last_nodes == n_last_nodes
              && actual.n_current_nodes == edges.nodeCount()
              && actual.n_new_nodes == edges.nodeCount() - n_last_nodes
              && actual.n_old_levels == old_levels
              && actual.mixed_vertex_count
                     == old_levels + edges.nodeCount() - n_last_nodes,
          "mixed domain telemetry differs from the independent model");
  require(actual.actual_eligible_arc_visits == eligible
              && actual.old_old == old_old && actual.old_new == old_new
              && actual.new_old == new_old && actual.new_new == new_new
              && actual.base_out == base_out && actual.aux_out == aux_out,
          "edge-class telemetry differs from the independent model");
  require(actual.mapped_actual_arc_candidates == mapped_candidates
              && actual.unique_actual_endpoint_pairs == actual_pairs.size()
              && actual.actual_parallel_duplicates_collapsed
                     == parallel_duplicates
              && actual.proxy_alias_duplicates_collapsed == proxy_aliases
              && actual.virtual_chain_edges
                     == (old_levels == 0 ? 0 : old_levels - 1)
              && actual.unique_precedence_edges == precedence.size(),
          "dedup or precedence telemetry differs from the independent model");
}

std::vector<std::vector<std::uint64_t>> randomPreviousLevels(
    std::mt19937_64& random, std::uint64_t old_count)
{
  std::vector<std::uint64_t> nodes;
  for (std::uint64_t node = 0; node < old_count; ++node)
    nodes.push_back(node);
  std::shuffle(nodes.begin(), nodes.end(), random);
  const std::size_t level_count =
      2 + static_cast<std::size_t>(random() % (old_count - 2));
  std::vector<std::vector<std::uint64_t>> levels(level_count);
  std::size_t next = 0;
  for (auto& level : levels)
    level.push_back(nodes[next++]);
  levels.front().push_back(nodes[next++]);
  while (next < nodes.size()) {
    levels[static_cast<std::size_t>(random() % level_count)].push_back(
        nodes[next++]);
  }
  for (auto& level : levels)
    std::sort(level.begin(), level.end());
  return levels;
}

std::uint64_t actualEndpointForMixed(
    std::uint64_t mixed, std::uint64_t old_count,
    const csr::CsrFifoLevelization& previous, std::mt19937_64& random)
{
  const std::uint64_t old_levels = previous.level_offsets.size() - 1;
  if (mixed >= old_levels)
    return old_count + mixed - old_levels;
  const std::uint64_t begin =
      previous.level_offsets[static_cast<std::size_t>(mixed)];
  const std::uint64_t end =
      previous.level_offsets[static_cast<std::size_t>(mixed + 1)];
  const std::uint64_t position = begin + random() % (end - begin);
  return previous.level_nodes[static_cast<std::size_t>(position)].value();
}

GeneratedCase compatibleSample(std::mt19937_64& random,
                               std::uint64_t generation)
{
  const std::uint64_t old_count = 4 + random() % 5;
  const std::uint64_t new_count = 2 + random() % 4;
  const auto by_level = randomPreviousLevels(random, old_count);
  const auto previous = makeLevels(static_cast<std::size_t>(old_count),
                                   by_level);
  const std::uint64_t old_levels = by_level.size();
  std::vector<std::uint64_t> order{0, old_levels};
  for (std::uint64_t level = 1; level < old_levels; ++level)
    order.push_back(level);
  order.push_back(old_levels + 1);
  for (std::uint64_t suffix = 2; suffix < new_count; ++suffix) {
    const std::size_t position = static_cast<std::size_t>(
        random() % (order.size() + 1));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(position),
                 old_levels + suffix);
  }

  TestEdgeView edges(static_cast<std::size_t>(old_count + new_count));
  const auto add_mixed = [&](std::uint64_t source, std::uint64_t sink,
                             csr::EdgeStorageKind storage) {
    edges.addArc({actualEndpointForMixed(source, old_count, previous, random),
                  actualEndpointForMixed(sink, old_count, previous, random),
                  true, storage});
  };
  add_mixed(0, 1, csr::EdgeStorageKind::kBaseOut);
  const std::uint64_t alias_sink = old_count;
  const std::uint64_t first_old = by_level.front()[0];
  const std::uint64_t second_old = by_level.front()[1];
  edges.addArc({first_old, alias_sink, true,
                csr::EdgeStorageKind::kBaseOut});
  edges.addArc({first_old, alias_sink, true,
                csr::EdgeStorageKind::kAuxOut});
  edges.addArc({second_old, alias_sink, true,
                csr::EdgeStorageKind::kAuxOut});
  add_mixed(old_levels, 1, csr::EdgeStorageKind::kBaseOut);
  add_mixed(old_levels, old_levels + 1,
            csr::EdgeStorageKind::kAuxOut);

  for (std::size_t source_pos = 0; source_pos < order.size(); ++source_pos)
    for (std::size_t sink_pos = source_pos + 1; sink_pos < order.size();
         ++sink_pos) {
      if (random() % 5 != 0)
        continue;
      add_mixed(order[source_pos], order[sink_pos],
                random() % 3 == 0 ? csr::EdgeStorageKind::kAuxOut
                                  : csr::EdgeStorageKind::kBaseOut);
    }
  edges.addArc({actualEndpointForMixed(order.back(), old_count, previous,
                                       random),
                actualEndpointForMixed(order.front(), old_count, previous,
                                       random),
                false, csr::EdgeStorageKind::kAuxOut});
  return {previous, std::move(edges), {old_count, generation - 1, generation}};
}

void requireSuccessfulProperties(const GeneratedCase& generated)
{
  require(independentlyAcyclic(generated.edges),
          "compatible generator emitted an actual cycle");
  bool has_multi_old_level = false;
  for (std::size_t level = 0;
       level + 1 < generated.previous.level_offsets.size(); ++level)
    has_multi_old_level |= generated.previous.level_offsets[level + 1]
                           - generated.previous.level_offsets[level] > 1;
  require(has_multi_old_level,
          "successful sample lost its multi-node old level");
  const auto attempt = buildMixedLevelizationGraph(
      generated.edges, generated.previous, generated.request);
  const auto* graph = std::get_if<MixedPrecedenceGraph>(&attempt);
  require(graph != nullptr, "compatible graph was rejected");
  requireIndependentGraph(generated.edges, generated.previous,
                          generated.request.n_last_nodes, *graph);
  require(classifyActualDag(generated.edges).isDag() == independentlyAcyclic(generated.edges),
          "actual DAG classification differs from the independent model");
  const auto& telemetry = graph->telemetry;
  require(validateMixedTelemetryIdentities(telemetry),
          "builder telemetry violates deterministic identities");
  require(telemetry.old_old > 0 && telemetry.old_new > 0
              && telemetry.new_old > 0 && telemetry.new_new > 0,
          "successful sample did not exercise all four edge classes");
  require(telemetry.actual_parallel_duplicates_collapsed > 0
              && telemetry.proxy_alias_duplicates_collapsed > 0
              && telemetry.aux_out > 0,
          "successful sample missed parallel, proxy-alias, or Aux coverage");
}

void fixedMultiOldProxyCanary()
{
  GeneratedCase generated{
      makeLevels(5, {{0, 1}, {2}, {3, 4}}),
      TestEdgeView(7), {5, 40, 41}};
  auto& edges = generated.edges;
  edges.addArc({0, 2, true, csr::EdgeStorageKind::kAuxOut});
  edges.addArc({0, 5});
  edges.addArc({0, 5, true, csr::EdgeStorageKind::kAuxOut});
  edges.addArc({1, 5, true, csr::EdgeStorageKind::kAuxOut});
  edges.addArc({5, 2});
  edges.addArc({5, 6, true, csr::EdgeStorageKind::kAuxOut});
  edges.addArc({4, 6});
  try {
    requireSuccessfulProperties(generated);
  } catch (const std::exception& error) {
    throw std::runtime_error(caseContext(0, 0, generated) + ": "
                             + error.what());
  }
}

void fixedSeedProperties()
{
  constexpr std::uint64_t seeds[] = {1, 7, 42, 20260901, 0xdeadbeefULL};
  constexpr std::uint64_t kSamplesPerSeed = 24;
  for (std::uint64_t seed : seeds) {
    std::mt19937_64 random(seed);
    for (std::uint64_t sample = 0; sample < kSamplesPerSeed; ++sample) {
      GeneratedCase generated = compatibleSample(random, sample + 1);
      try {
        requireSuccessfulProperties(generated);
      } catch (const std::exception& error) {
        throw std::runtime_error(caseContext(seed, sample, generated)
                                 + ": " + error.what());
      }
    }
  }
}

}  // namespace
}  // namespace ista::timing_incremental_levelization::test

int main()
{
  try {
    ista::timing_incremental_levelization::test::fixedMultiOldProxyCanary();
    ista::timing_incremental_levelization::test::fixedSeedProperties();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
