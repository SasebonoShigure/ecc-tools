// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrArcGroupIndexValidation.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "DualCsrStateValidation.hpp"
#include "TimingEditCanonicalEncoding.hpp"

namespace ista::timing_dual_csr {
namespace {

struct ArcBaseLess
{
  bool operator()(const csr::ArcKeyBase& lhs,
                  const csr::ArcKeyBase& rhs) const
  {
    return csr::arcKeyBaseLess(lhs, rhs);
  }
};

struct NetSinkLess
{
  bool operator()(const DualCsrNetSinkKey& lhs,
                  const DualCsrNetSinkKey& rhs) const
  {
    if (lhs.owner_name != rhs.owner_name)
      return lhs.owner_name < rhs.owner_name;
    return csr::nodeKeyLess(lhs.sink, rhs.sink);
  }
};

void reject(DualCsrViolations& errors, DualCsrViolationCode code,
            std::string field, std::string message)
{
  errors.push_back({code, std::move(field), std::move(message)});
}

}  // namespace

DualCsrViolations validateDualCsrArcGroupIndex(
    const DualCsrState& graph, const DualCsrArcGroupIndex& index)
{
  DualCsrViolations errors;
  if (index.session_token != graph.session_token)
    reject(errors, DualCsrViolationCode::kSession, "arc_group_index.session",
           "index session differs from graph session");
  if (index.generation != graph.generation)
    reject(errors, DualCsrViolationCode::kGeneration,
           "arc_group_index.generation",
           "index generation differs from graph generation");
  if (index.arc_high_water != graph.high_water.arc_slots
      || index.slots.size() != graph.arc_slots.size())
    reject(errors, DualCsrViolationCode::kHighWater,
           "arc_group_index.slots",
           "index ArcId high-water differs from graph");
  if (!validateDualCsrState(graph).empty())
    reject(errors, DualCsrViolationCode::kShape, "arc_group_index.graph",
           "index cannot bind a malformed graph");
  if (index.slots.size() != graph.arc_slots.size())
    return errors;

  std::set<timing_edit::StableArcRef> refs;
  std::map<csr::ArcKeyBase, std::vector<csr::ArcId>, ArcBaseLess>
      expected_groups;
  std::map<std::string, std::vector<csr::ArcId>>
      expected_sink_net_groups;
  std::map<DualCsrNetSinkKey, std::vector<csr::ArcId>, NetSinkLess>
      expected_net_sink_groups;
  for (std::size_t slot = 0; slot < index.slots.size(); ++slot) {
    const auto& indexed = index.slots[slot];
    const auto& graph_slot = graph.arc_slots[slot];
    if (!refs.insert(indexed.ref)
             .second)
      reject(errors, DualCsrViolationCode::kConflict,
             "arc_group_index.slots[" + std::to_string(slot) + "].ref",
             "StableArcRef is bound to more than one ArcId");
    if (indexed.current_active_key.has_value() != graph_slot.active
        || (graph_slot.active
            && *indexed.current_active_key != graph_slot.payload.key))
      reject(errors, DualCsrViolationCode::kExpectedValue,
             "arc_group_index.slots[" + std::to_string(slot) + "].key",
             "current active key differs from graph slot");
    if (graph_slot.active) {
      expected_groups[graph_slot.payload.key.base].push_back(
          csr::ArcId{static_cast<std::uint64_t>(slot)});
      if (graph_slot.payload.sink.value() < graph.nodes.size())
        expected_sink_net_groups[
            graph.nodes[graph_slot.payload.sink.value()].net_name]
            .push_back(csr::ArcId{static_cast<std::uint64_t>(slot)});
      if (graph_slot.payload.key.base.kind == csr::ArcKind::kNet)
        expected_net_sink_groups[
            {graph_slot.payload.key.base.owner_name,
             graph_slot.payload.key.base.sink}]
            .push_back(csr::ArcId{static_cast<std::uint64_t>(slot)});
    }
  }
  for (auto& [base, ids] : expected_groups) {
    static_cast<void>(base);
    std::ranges::sort(ids, [&](csr::ArcId lhs, csr::ArcId rhs) {
      return graph.arc_slots[lhs.value()].payload.key.parallel_ordinal
             < graph.arc_slots[rhs.value()].payload.key.parallel_ordinal;
    });
  }

  if (index.active_groups.size() != expected_groups.size())
    reject(errors, DualCsrViolationCode::kShape,
           "arc_group_index.active_groups",
           "active group count differs from graph");
  std::size_t position = 0;
  for (const auto& [base, expected_ids] : expected_groups) {
    if (position >= index.active_groups.size())
      break;
    const auto& actual = index.active_groups[position++];
    if (actual.base != base || actual.active_arc_ids != expected_ids)
      reject(errors, DualCsrViolationCode::kExpectedValue,
             "arc_group_index.active_groups["
                 + std::to_string(position - 1) + "]",
             "base or ordinal-ordered ArcId group differs from graph");
  }
  for (std::size_t group = 1; group < index.active_groups.size(); ++group)
    if (!ArcBaseLess{}(index.active_groups[group - 1].base,
                       index.active_groups[group].base))
      reject(errors, DualCsrViolationCode::kKey,
             "arc_group_index.active_groups[" + std::to_string(group)
                 + "].base",
             "active groups are not strictly base-key ordered");

  if (index.active_sink_net_groups.size()
      != expected_sink_net_groups.size())
    reject(errors, DualCsrViolationCode::kShape,
           "arc_group_index.active_sink_net_groups",
           "active sink-net group count differs from graph");
  position = 0;
  for (const auto& [net_name, expected_ids] :
       expected_sink_net_groups) {
    if (position >= index.active_sink_net_groups.size())
      break;
    const auto& actual = index.active_sink_net_groups[position++];
    if (actual.sink_net_name != net_name
        || actual.active_arc_ids != expected_ids)
      reject(errors, DualCsrViolationCode::kExpectedValue,
             "arc_group_index.active_sink_net_groups["
                 + std::to_string(position - 1) + "]",
             "sink net or ArcId group differs from graph");
  }
  for (std::size_t group = 1;
       group < index.active_sink_net_groups.size(); ++group)
    if (!(index.active_sink_net_groups[group - 1].sink_net_name
          < index.active_sink_net_groups[group].sink_net_name))
      reject(errors, DualCsrViolationCode::kKey,
             "arc_group_index.active_sink_net_groups["
                 + std::to_string(group) + "].sink_net_name",
             "sink-net groups are not strictly name ordered");

  if (index.active_net_sink_groups.size()
      != expected_net_sink_groups.size())
    reject(errors, DualCsrViolationCode::kShape,
           "arc_group_index.active_net_sink_groups",
           "directed net/sink group count differs from graph");
  position = 0;
  for (const auto& [key, expected_ids] : expected_net_sink_groups) {
    if (position >= index.active_net_sink_groups.size())
      break;
    const auto& actual = index.active_net_sink_groups[position++];
    if (actual.key != key || actual.active_arc_ids != expected_ids)
      reject(errors, DualCsrViolationCode::kExpectedValue,
             "arc_group_index.active_net_sink_groups["
                 + std::to_string(position - 1) + "]",
             "directed net/sink key or ArcIds differ from graph");
  }
  for (std::size_t group = 1;
       group < index.active_net_sink_groups.size(); ++group)
    if (!NetSinkLess{}(index.active_net_sink_groups[group - 1].key,
                       index.active_net_sink_groups[group].key))
      reject(errors, DualCsrViolationCode::kKey,
             "arc_group_index.active_net_sink_groups["
                 + std::to_string(group) + "].key",
             "directed net/sink groups are not strictly ordered");
  std::vector<csr::NodeId> expected_clock_sources;
  for (const auto& clock : graph.reference_payload.clocks)
    for (const auto& source : clock.ordered_sources)
      if (source.resolved_node)
        expected_clock_sources.push_back(*source.resolved_node);
  std::ranges::sort(expected_clock_sources,
                    [](csr::NodeId lhs, csr::NodeId rhs) {
    return lhs.value() < rhs.value();
  });
  expected_clock_sources.erase(
      std::ranges::unique(expected_clock_sources).begin(),
      expected_clock_sources.end());
  if (index.clock_source_node_ids != expected_clock_sources)
    reject(errors, DualCsrViolationCode::kExpectedValue,
           "arc_group_index.clock_source_node_ids",
           "clock-source NodeId point index differs from reference payload");
  return errors;
}

}  // namespace ista::timing_dual_csr
