// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTestSupport.hpp"

namespace ista::timing_dual_csr::test {

struct PropertyOracle
{
  std::vector<csr::NodeRecord> nodes;
  std::vector<DualCsrArcSlot> arcs;

  void apply(const DualCsrBatchDescriptor& descriptor)
  {
    for (const auto& append : descriptor.node_appends) {
      require(append.id.value() == nodes.size(),
              "oracle observed a non-append NodeId");
      nodes.push_back(append.payload);
    }
    for (const auto& change : descriptor.node_payload_changes)
      nodes.at(change.id.value()).net_name = change.replacement_net_name;
    for (const auto& item : descriptor.logical_deactivations)
      arcs.at(item.id.value()).active = false;
    for (const auto& change : descriptor.payload_changes)
      arcs.at(change.id.value()).payload = change.replacement;
    for (const auto& append : descriptor.arc_appends) {
      require(append.id.value() == arcs.size(),
              "oracle observed a non-append ArcId");
      arcs.push_back({append.payload, true});
    }
  }
};

inline std::vector<csr::NodeId> propertyCanonicalNodes(
    const PropertyOracle& oracle, bool roots, bool starts, bool ends)
{
  std::vector<std::uint64_t> indegree(oracle.nodes.size(), 0);
  for (const auto& slot : oracle.arcs)
    if (slot.active && slot.payload.eligibility.levelization)
      ++indegree.at(slot.payload.sink.value());
  std::vector<csr::NodeId> order;
  for (std::size_t id = 0; id < oracle.nodes.size(); ++id)
    order.emplace_back(id);
  std::ranges::sort(order, [&](csr::NodeId lhs, csr::NodeId rhs) {
    return oracle.nodes[lhs.value()].key.full_name
           < oracle.nodes[rhs.value()].key.full_name;
  });
  std::vector<csr::NodeId> result;
  for (const auto id : order) {
    const auto& node = oracle.nodes[id.value()];
    if ((roots && indegree[id.value()] == 0)
        || (starts && node.is_startpoint)
        || (ends && node.is_endpoint))
      result.push_back(id);
  }
  return result;
}

inline std::vector<csr::ArcId> propertySortedActiveIds(
    const PropertyOracle& oracle, std::optional<csr::NodeId> row,
    bool outgoing)
{
  std::vector<csr::ArcId> result;
  for (std::size_t id = 0; id < oracle.arcs.size(); ++id) {
    const auto& slot = oracle.arcs[id];
    if (!slot.active)
      continue;
    if (row && (outgoing ? slot.payload.source : slot.payload.sink) != *row)
      continue;
    result.emplace_back(id);
  }
  std::ranges::sort(result, {}, &csr::ArcId::value);
  return result;
}

inline void requirePropertyParallelOrdinals(
    const PropertyOracle& oracle, const std::string& context)
{
  std::vector<csr::ArcId> order;
  for (std::size_t id = 0; id < oracle.arcs.size(); ++id)
    if (oracle.arcs[id].active)
      order.emplace_back(id);
  std::ranges::sort(order, [&](csr::ArcId lhs, csr::ArcId rhs) {
    return csr::arcKeyLess(oracle.arcs[lhs.value()].payload.key,
                           oracle.arcs[rhs.value()].payload.key);
  });
  std::uint64_t expected = 0;
  for (std::size_t index = 0; index < order.size(); ++index) {
    const auto& key = oracle.arcs[order[index].value()].payload.key;
    if (index == 0
        || !(oracle.arcs[order[index - 1].value()].payload.key.base
             == key.base))
      expected = 0;
    require(key.parallel_ordinal == expected++,
            context + " parallel ordinals are not continuous from zero");
  }
}

inline void requirePropertyNetDag(const PropertyOracle& oracle,
                                  const std::string& context)
{
  std::map<std::string, csr::NodeId> drivers;
  std::set<std::pair<std::string, std::uint64_t>> loads;
  std::vector<std::uint64_t> indegree(oracle.nodes.size(), 0);
  std::vector<std::vector<std::size_t>> outgoing(oracle.nodes.size());
  for (const auto& slot : oracle.arcs) {
    if (!slot.active)
      continue;
    const auto& arc = slot.payload;
    require(arc.source.value() < oracle.nodes.size()
                && arc.sink.value() < oracle.nodes.size(),
            context + " oracle contains an out-of-range active arc");
    const auto& source = oracle.nodes[arc.source.value()];
    const auto& sink = oracle.nodes[arc.sink.value()];
    require(arc.kind == csr::ArcKind::kNet
                && source.net_name == sink.net_name
                && arc.key.base.owner_name == source.net_name,
            context + " active net ownership is incoherent");
    const auto [driver, inserted]
        = drivers.emplace(source.net_name, arc.source);
    require(inserted || driver->second == arc.source,
            context + " active net has multiple drivers");
    require(loads.emplace(source.net_name, arc.sink.value()).second,
            context + " active net repeats a load");
    ++indegree[arc.sink.value()];
    outgoing[arc.source.value()].push_back(arc.sink.value());
  }
  std::vector<std::size_t> queue;
  for (std::size_t node = 0; node < indegree.size(); ++node)
    if (indegree[node] == 0)
      queue.push_back(node);
  std::size_t visited = 0;
  for (std::size_t head = 0; head < queue.size(); ++head) {
    ++visited;
    for (const auto sink : outgoing[queue[head]])
      if (--indegree[sink] == 0)
        queue.push_back(sink);
  }
  require(visited == oracle.nodes.size(), context + " oracle is cyclic");
}

inline void requirePropertyMatches(const DualCsrState& state,
                                   const PropertyOracle& oracle,
                                   const std::string& context)
{
  const auto violations = validateDualCsrState(state);
  require(violations.empty(),
          context + " state validation failed at "
              + (violations.empty() ? std::string{}
                                    : violations.front().field + ": "
                                          + violations.front().message));
  require(state.nodes == oracle.nodes && state.arc_slots == oracle.arcs,
          context + " stable records differ from the independent oracle");
  requirePropertyParallelOrdinals(oracle, context);
  requirePropertyNetDag(oracle, context);

  const auto roots = propertyCanonicalNodes(oracle, true, false, false);
  const auto starts = propertyCanonicalNodes(oracle, false, true, false);
  const auto ends = propertyCanonicalNodes(oracle, false, false, true);
  require(state.reference_payload.ordered_root_ids == roots
              && state.reference_payload.ordered_start_ids == starts
              && state.reference_payload.ordered_end_ids == ends,
          context + " canonical root/start/end reference differs");
  std::vector<csr::NodeId> seed_nodes;
  for (const auto& seed : state.reference_payload.start_seeds)
    seed_nodes.push_back(seed.seed_node);
  std::vector<csr::NodeId> required_nodes;
  for (const auto& endpoint : state.reference_payload.endpoint_required)
    required_nodes.push_back(endpoint.node);
  require(seed_nodes == starts && required_nodes == ends
              && state.reference_payload.clock_owner_by_node.size()
                     == oracle.nodes.size(),
          context + " canonical reference rows differ from node roles");

  DualCsrEdgeViewImpl view(state);
  std::vector<csr::ArcId> active_ids;
  const auto active_status = view.forEachActiveArc(
      [&](const csr::EdgeEntry& entry) {
        active_ids.push_back(entry.arc_id);
        return csr::EdgeVisitResult::continueVisit();
      });
  std::ranges::sort(active_ids, {}, &csr::ArcId::value);
  require(active_status.control == csr::EdgeVisitControl::kContinue
              && active_ids
                     == propertySortedActiveIds(oracle, std::nullopt, true),
          context + " active ArcKey/payload order differs from oracle");
  for (std::size_t node = 0; node < oracle.nodes.size(); ++node) {
    std::vector<csr::ArcId> actual_out;
    std::vector<csr::ArcId> actual_in;
    const auto out_status = view.forEachOutgoing(
        csr::NodeId{node}, [&](const csr::EdgeEntry& entry) {
          actual_out.push_back(entry.arc_id);
          return csr::EdgeVisitResult::continueVisit();
        });
    const auto in_status = view.forEachIncoming(
        csr::NodeId{node}, [&](const csr::EdgeEntry& entry) {
          actual_in.push_back(entry.arc_id);
          return csr::EdgeVisitResult::continueVisit();
        });
    std::ranges::sort(actual_out, {}, &csr::ArcId::value);
    std::ranges::sort(actual_in, {}, &csr::ArcId::value);
    require(out_status.control == csr::EdgeVisitControl::kContinue
                && in_status.control == csr::EdgeVisitControl::kContinue
                && actual_out
                       == propertySortedActiveIds(
                              oracle, csr::NodeId{node}, true)
                && actual_in
                       == propertySortedActiveIds(
                              oracle, csr::NodeId{node}, false),
            context + " outgoing/incoming transpose differs at node "
                + std::to_string(node));
  }
}

}  // namespace ista::timing_dual_csr::test
