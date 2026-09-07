// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrArcGroupIndex.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "DualCsrArcGroupIndexValidation.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr {
namespace {

struct ArcBaseLess
{
  bool operator()(const csr::ArcKeyBase& lhs,
                  const csr::ArcKeyBase& rhs) const
  {
    return csr::arcKeyLess(csr::ArcKey{lhs, 0},
                           csr::ArcKey{rhs, 0});
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

void rebuildGroups(DualCsrArcGroupIndex& index,
                   const DualCsrState& graph)
{
  std::map<csr::ArcKeyBase, std::vector<csr::ArcId>, ArcBaseLess> groups;
  for (std::size_t slot = 0; slot < index.slots.size(); ++slot)
    if (index.slots[slot].current_active_key)
      groups[index.slots[slot].current_active_key->base].push_back(
          csr::ArcId{static_cast<std::uint64_t>(slot)});
  index.active_groups.clear();
  index.active_groups.reserve(groups.size());
  for (auto& [base, ids] : groups) {
    std::ranges::sort(ids, [&](csr::ArcId lhs, csr::ArcId rhs) {
      return index.slots[lhs.value()]
                 .current_active_key->parallel_ordinal
             < index.slots[rhs.value()]
                   .current_active_key->parallel_ordinal;
    });
    index.active_groups.push_back({std::move(base), std::move(ids)});
  }

  std::map<std::string, std::vector<csr::ArcId>> sink_nets;
  for (std::size_t slot = 0; slot < graph.arc_slots.size(); ++slot) {
    const auto& arc = graph.arc_slots[slot];
    if (!arc.active || arc.payload.sink.value() >= graph.nodes.size())
      continue;
    sink_nets[graph.nodes[arc.payload.sink.value()].net_name].push_back(
        csr::ArcId{static_cast<std::uint64_t>(slot)});
  }
  index.active_sink_net_groups.clear();
  index.active_sink_net_groups.reserve(sink_nets.size());
  for (auto& [net_name, ids] : sink_nets)
    index.active_sink_net_groups.push_back(
        {std::move(net_name), std::move(ids)});

  std::map<DualCsrNetSinkKey, std::vector<csr::ArcId>, NetSinkLess>
      net_sinks;
  for (std::size_t slot = 0; slot < graph.arc_slots.size(); ++slot) {
    const auto& arc = graph.arc_slots[slot];
    if (!arc.active || arc.payload.key.base.kind != csr::ArcKind::kNet)
      continue;
    net_sinks[{arc.payload.key.base.owner_name,
               arc.payload.key.base.sink}]
        .push_back(csr::ArcId{static_cast<std::uint64_t>(slot)});
  }
  index.active_net_sink_groups.clear();
  index.active_net_sink_groups.reserve(net_sinks.size());
  for (auto& [key, ids] : net_sinks)
    index.active_net_sink_groups.push_back(
        {std::move(key), std::move(ids)});

  index.clock_source_node_ids.clear();
  for (const auto& clock : graph.reference_payload.clocks)
    for (const auto& source : clock.ordered_sources)
      if (source.resolved_node)
        index.clock_source_node_ids.push_back(*source.resolved_node);
  std::ranges::sort(index.clock_source_node_ids,
                    [](csr::NodeId lhs, csr::NodeId rhs) {
    return lhs.value() < rhs.value();
  });
  index.clock_source_node_ids.erase(
      std::ranges::unique(index.clock_source_node_ids).begin(),
      index.clock_source_node_ids.end());
}

void replaceActiveGroup(DualCsrArcGroupIndex& index,
                        const csr::ArcKeyBase& base,
                        std::vector<csr::ArcId> ids)
{
  const auto found = std::ranges::lower_bound(
      index.active_groups, base, ArcBaseLess{},
      &DualCsrArcGroup::base);
  const auto position = static_cast<std::size_t>(
      std::distance(index.active_groups.begin(), found));
  const bool exists = found != index.active_groups.end()
                      && !ArcBaseLess{}(base, found->base)
                      && !ArcBaseLess{}(found->base, base);
  if (exists)
    index.active_groups.erase(index.active_groups.begin() + position);
  if (!ids.empty())
    index.active_groups.insert(index.active_groups.begin() + position,
                               {base, std::move(ids)});
}

void replaceSinkNetGroup(DualCsrArcGroupIndex& index,
                         const std::string& net_name,
                         std::vector<csr::ArcId> ids)
{
  const auto found = std::ranges::lower_bound(
      index.active_sink_net_groups, net_name, std::ranges::less{},
      &DualCsrArcSinkNetGroup::sink_net_name);
  const auto position = static_cast<std::size_t>(std::distance(
      index.active_sink_net_groups.begin(), found));
  const bool exists = found != index.active_sink_net_groups.end()
                      && found->sink_net_name == net_name;
  if (exists)
    index.active_sink_net_groups.erase(
        index.active_sink_net_groups.begin() + position);
  if (!ids.empty())
    index.active_sink_net_groups.insert(
        index.active_sink_net_groups.begin() + position,
        {net_name, std::move(ids)});
}

void replaceNetSinkGroup(DualCsrArcGroupIndex& index,
                         const DualCsrNetSinkKey& key,
                         std::vector<csr::ArcId> ids)
{
  const auto found = std::ranges::lower_bound(
      index.active_net_sink_groups, key, NetSinkLess{},
      &DualCsrNetSinkGroup::key);
  const auto position = static_cast<std::size_t>(std::distance(
      index.active_net_sink_groups.begin(), found));
  const bool exists = found != index.active_net_sink_groups.end()
                      && !NetSinkLess{}(key, found->key)
                      && !NetSinkLess{}(found->key, key);
  if (exists)
    index.active_net_sink_groups.erase(
        index.active_net_sink_groups.begin() + position);
  if (!ids.empty())
    index.active_net_sink_groups.insert(
        index.active_net_sink_groups.begin() + position,
        {key, std::move(ids)});
}

const std::string* sinkNet(const DualCsrState& graph,
                           csr::ArcId id) noexcept
{
  if (id.value() >= graph.arc_slots.size())
    return nullptr;
  const auto& arc = graph.arc_slots[id.value()];
  if (!arc.active || arc.payload.sink.value() >= graph.nodes.size())
    return nullptr;
  return &graph.nodes[arc.payload.sink.value()].net_name;
}

void updateAffectedGroups(
    const DualCsrState& committed_graph,
    const DualCsrArcGroupIndex& committed_index,
    const DualCsrState& staged_graph,
    const DualCsrBatchDescriptor& descriptor,
    DualCsrArcGroupIndex& staged)
{
  std::set<csr::ArcKeyBase, ArcBaseLess> affected_bases;
  std::set<std::uint64_t> changed_ids;
  for (const auto& delta : descriptor.arc_group_index_delta) {
    changed_ids.insert(delta.id.value());
    if (delta.expected_active_key)
      affected_bases.insert(delta.expected_active_key->base);
    if (delta.replacement_active_key)
      affected_bases.insert(delta.replacement_active_key->base);
  }
  for (const auto& base : affected_bases) {
    std::set<std::uint64_t> candidates = changed_ids;
    if (const auto* group = committed_index.activeGroup(base))
      for (const auto id : group->active_arc_ids)
        candidates.insert(id.value());
    std::vector<csr::ArcId> ids;
    for (const auto id : candidates) {
      if (id >= staged.slots.size())
        continue;
      const auto& key = staged.slots[id].current_active_key;
      if (key && key->base == base)
        ids.push_back(csr::ArcId{id});
    }
    std::ranges::sort(ids, [&](csr::ArcId lhs, csr::ArcId rhs) {
      return staged.slots[lhs.value()]
                 .current_active_key->parallel_ordinal
             < staged.slots[rhs.value()]
                   .current_active_key->parallel_ordinal;
    });
    replaceActiveGroup(staged, base, std::move(ids));
  }

  std::set<DualCsrNetSinkKey, NetSinkLess> affected_net_sinks;
  for (const auto& delta : descriptor.arc_group_index_delta) {
    if (delta.expected_active_key
        && delta.expected_active_key->base.kind == csr::ArcKind::kNet)
      affected_net_sinks.insert(
          {delta.expected_active_key->base.owner_name,
           delta.expected_active_key->base.sink});
    if (delta.replacement_active_key
        && delta.replacement_active_key->base.kind == csr::ArcKind::kNet)
      affected_net_sinks.insert(
          {delta.replacement_active_key->base.owner_name,
           delta.replacement_active_key->base.sink});
  }
  for (const auto& key : affected_net_sinks) {
    std::set<std::uint64_t> candidates = changed_ids;
    if (const auto* group = committed_index.activeNetSinkGroup(
            key.owner_name, key.sink))
      for (const auto id : group->active_arc_ids)
        candidates.insert(id.value());
    std::vector<csr::ArcId> ids;
    for (const auto id : candidates) {
      if (id >= staged.slots.size())
        continue;
      const auto& active = staged.slots[id].current_active_key;
      if (active && active->base.kind == csr::ArcKind::kNet
          && active->base.owner_name == key.owner_name
          && active->base.sink == key.sink)
        ids.push_back(csr::ArcId{id});
    }
    std::ranges::sort(ids, {}, &csr::ArcId::value);
    replaceNetSinkGroup(staged, key, std::move(ids));
  }

  std::set<std::string> affected_nets;
  std::set<std::uint64_t> net_changed_ids = changed_ids;
  for (const auto& delta : descriptor.arc_group_index_delta) {
    if (const auto* net = sinkNet(committed_graph, delta.id))
      affected_nets.insert(*net);
    if (const auto* net = sinkNet(staged_graph, delta.id))
      affected_nets.insert(*net);
  }
  for (const auto& change : descriptor.node_payload_changes) {
    affected_nets.insert(change.expected_net_name);
    affected_nets.insert(change.replacement_net_name);
    if (const auto* group = committed_index.activeSinkNetGroup(
            change.expected_net_name))
      for (const auto id : group->active_arc_ids)
        if (id.value() < committed_graph.arc_slots.size()
            && committed_graph.arc_slots[id.value()].payload.sink
                   == change.id)
          net_changed_ids.insert(id.value());
  }
  for (const auto& net_name : affected_nets) {
    std::set<std::uint64_t> candidates = net_changed_ids;
    if (const auto* group
        = committed_index.activeSinkNetGroup(net_name))
      for (const auto id : group->active_arc_ids)
        candidates.insert(id.value());
    std::vector<csr::ArcId> ids;
    for (const auto id : candidates) {
      const csr::ArcId arc_id{id};
      const auto* actual_net = sinkNet(staged_graph, arc_id);
      if (actual_net != nullptr && *actual_net == net_name)
        ids.push_back(arc_id);
    }
    std::ranges::sort(ids, {}, &csr::ArcId::value);
    replaceSinkNetGroup(staged, net_name, std::move(ids));
  }
}

std::map<std::uint64_t, const ArcGroupIndexDelta*> deltasById(
    const DualCsrBatchDescriptor& descriptor,
    DualCsrViolations& errors)
{
  std::map<std::uint64_t, const ArcGroupIndexDelta*> result;
  for (std::size_t position = 0;
       position < descriptor.arc_group_index_delta.size(); ++position) {
    const auto& delta = descriptor.arc_group_index_delta[position];
    if (!result.emplace(delta.id.value(), &delta).second)
      reject(errors, DualCsrViolationCode::kConflict,
             "descriptor.arc_group_index_delta["
                 + std::to_string(position) + "].id",
             "ArcId occurs more than once");
    if (position != 0
        && descriptor.arc_group_index_delta[position - 1].id.value()
               >= delta.id.value())
      reject(errors, DualCsrViolationCode::kId,
             "descriptor.arc_group_index_delta["
                 + std::to_string(position) + "].id",
             "index deltas must be strictly ArcId ordered");
  }
  return result;
}

void expectTransition(
    const DualCsrArcGroupIndex& committed,
    std::map<std::uint64_t, const ArcGroupIndexDelta*>& deltas,
    csr::ArcId id, const std::optional<csr::ArcKey>& expected,
    const std::optional<csr::ArcKey>& replacement,
    std::string field, DualCsrViolations& errors)
{
  const auto found = deltas.find(id.value());
  if (found == deltas.end()) {
    reject(errors, DualCsrViolationCode::kExpectedValue, std::move(field),
           "required ArcGroupIndexDelta is missing");
    return;
  }
  const auto& delta = *found->second;
  if (delta.expected_active_key != expected
      || delta.replacement_active_key != replacement)
    reject(errors, DualCsrViolationCode::kExpectedValue, std::move(field),
           "typed active-key transition is not exact");
  if (id.value() < committed.slots.size()
      && delta.ref != committed.slots[id.value()].ref)
    reject(errors, DualCsrViolationCode::kExpectedValue, std::move(field),
           "existing ArcId changed StableArcRef");
  deltas.erase(found);
}

DualCsrViolations validateDeltaCoverage(
    const DualCsrState& committed_graph,
    const DualCsrArcGroupIndex& committed,
    const DualCsrBatchDescriptor& descriptor)
{
  DualCsrViolations errors;
  auto deltas = deltasById(descriptor, errors);
  for (const auto& append : descriptor.arc_appends)
    expectTransition(committed, deltas, append.id, std::nullopt,
                     append.payload.key,
                     "descriptor.arc_group_index_delta.append", errors);
  for (const auto& deactivation : descriptor.logical_deactivations) {
    const auto id = deactivation.id.value();
    const std::optional<csr::ArcKey> expected
        = id < committed_graph.arc_slots.size()
              ? std::optional<csr::ArcKey>{
                    committed_graph.arc_slots[id].payload.key}
              : std::nullopt;
    expectTransition(committed, deltas, deactivation.id, expected,
                     std::nullopt,
                     "descriptor.arc_group_index_delta.deactivate",
                     errors);
  }
  for (const auto& change : descriptor.payload_changes)
    if (change.expected.key != change.replacement.key)
      expectTransition(committed, deltas, change.id,
                       change.expected.key, change.replacement.key,
                       "descriptor.arc_group_index_delta.rekey", errors);
  if (!deltas.empty())
    reject(errors, DualCsrViolationCode::kConflict,
           "descriptor.arc_group_index_delta",
           "delta contains an unchanged or unrecognized ArcId");
  return errors;
}

}  // namespace

DualCsrViolations validateDualCsrArcGroupIndexDelta(
    const DualCsrState& committed_graph,
    const DualCsrArcGroupIndex& committed_index,
    const DualCsrBatchDescriptor& descriptor)
{
  DualCsrViolations errors;
  if (committed_graph.session_token != descriptor.session_token
      || committed_index.session_token != descriptor.session_token
      || committed_graph.generation != descriptor.expected_generation
      || committed_index.generation != descriptor.expected_generation
      || committed_graph.high_water != descriptor.before
      || committed_index.arc_high_water != descriptor.before.arc_slots
      || descriptor.after.arc_slots < descriptor.before.arc_slots)
    reject(errors, DualCsrViolationCode::kGeneration,
           "descriptor.arc_group_index_delta.boundary",
           "descriptor and exact-before graph/index boundary differ");
  std::set<timing_edit::StableArcRef> refs;
  for (const auto& delta : descriptor.arc_group_index_delta) {
    if (delta.id.value() >= descriptor.after.arc_slots)
      reject(errors, DualCsrViolationCode::kId,
             "descriptor.arc_group_index_delta.id",
             "ArcId is outside descriptor after high-water");
    if (!refs.insert(delta.ref).second)
      reject(errors, DualCsrViolationCode::kConflict,
             "descriptor.arc_group_index_delta.ref",
             "StableArcRef occurs in more than one index delta");
  }
  auto coverage = validateDeltaCoverage(
      committed_graph, committed_index, descriptor);
  errors.insert(errors.end(),
                std::make_move_iterator(coverage.begin()),
                std::make_move_iterator(coverage.end()));
  return errors;
}

const DualCsrArcGroupIndexSlot* DualCsrArcGroupIndex::slot(
    csr::ArcId id) const noexcept
{
  if (id.value() >= slots.size())
    return nullptr;
  return &slots[id.value()];
}

const DualCsrArcGroup* DualCsrArcGroupIndex::activeGroup(
    const csr::ArcKeyBase& base) const noexcept
{
  const auto found = std::ranges::lower_bound(
      active_groups, base, ArcBaseLess{}, &DualCsrArcGroup::base);
  if (found == active_groups.end()
      || ArcBaseLess{}(base, found->base)
      || ArcBaseLess{}(found->base, base))
    return nullptr;
  return &*found;
}

const DualCsrArcSinkNetGroup* DualCsrArcGroupIndex::activeSinkNetGroup(
    std::string_view sink_net_name) const noexcept
{
  const auto found = std::ranges::lower_bound(
      active_sink_net_groups, sink_net_name, std::ranges::less{},
      &DualCsrArcSinkNetGroup::sink_net_name);
  if (found == active_sink_net_groups.end()
      || found->sink_net_name != sink_net_name)
    return nullptr;
  return &*found;
}

const DualCsrNetSinkGroup* DualCsrArcGroupIndex::activeNetSinkGroup(
    std::string_view owner_name, const csr::NodeKey& sink) const noexcept
{
  const DualCsrNetSinkKey key{std::string(owner_name), sink};
  const auto found = std::ranges::lower_bound(
      active_net_sink_groups, key, NetSinkLess{},
      &DualCsrNetSinkGroup::key);
  if (found == active_net_sink_groups.end()
      || NetSinkLess{}(key, found->key)
      || NetSinkLess{}(found->key, key))
    return nullptr;
  return &*found;
}

bool DualCsrArcGroupIndex::isClockSourceNode(csr::NodeId id) const noexcept
{
  return std::ranges::binary_search(
      clock_source_node_ids, id.value(), {},
      [](csr::NodeId value) { return value.value(); });
}

void DualCsrArcGroupIndex::swap(DualCsrArcGroupIndex& other) noexcept
{
  using std::swap;
  swap(session_token, other.session_token);
  swap(generation, other.generation);
  swap(arc_high_water, other.arc_high_water);
  swap(slots, other.slots);
  swap(active_groups, other.active_groups);
  swap(active_sink_net_groups, other.active_sink_net_groups);
  swap(active_net_sink_groups, other.active_net_sink_groups);
  swap(clock_source_node_ids, other.clock_source_node_ids);
}

void swap(DualCsrArcGroupIndex& lhs,
          DualCsrArcGroupIndex& rhs) noexcept
{
  lhs.swap(rhs);
}

DualCsrArcGroupIndexAttempt buildDualCsrArcGroupIndex(
    const DualCsrState& graph,
    std::span<const DualCsrArcGroupIndexBinding> bindings)
{
  DualCsrArcGroupIndexAttempt attempt;
  DualCsrArcGroupIndex index;
  index.session_token = graph.session_token;
  index.generation = graph.generation;
  index.arc_high_water = graph.high_water.arc_slots;
  index.slots.resize(graph.arc_slots.size());
  std::vector<bool> seen(graph.arc_slots.size(), false);
  for (const auto& binding : bindings) {
    const auto id = binding.id.value();
    if (id >= graph.arc_slots.size() || seen[id]) {
      reject(attempt.violations, DualCsrViolationCode::kId,
             "arc_group_index.bindings",
             "binding ArcId is out of range or duplicated");
      continue;
    }
    seen[id] = true;
    index.slots[id].ref = binding.ref;
    if (graph.arc_slots[id].active)
      index.slots[id].current_active_key
          = graph.arc_slots[id].payload.key;
  }
  if (!std::ranges::all_of(seen,
                           [](bool value) { return value; }))
    reject(attempt.violations, DualCsrViolationCode::kShape,
           "arc_group_index.bindings",
           "bindings do not cover every logical ArcId");
  rebuildGroups(index, graph);
  auto errors = validateDualCsrArcGroupIndex(graph, index);
  attempt.violations.insert(attempt.violations.end(),
                            std::make_move_iterator(errors.begin()),
                            std::make_move_iterator(errors.end()));
  if (attempt.violations.empty())
    attempt.index = std::move(index);
  return attempt;
}

DualCsrArcGroupIndexAttempt stageDualCsrArcGroupIndex(
    const DualCsrState& committed_graph,
    const DualCsrArcGroupIndex& committed_index,
    const DualCsrState& staged_graph,
    const DualCsrBatchDescriptor& descriptor)
{
  DualCsrArcGroupIndexAttempt attempt;
  if (committed_index.slots.size() != committed_graph.arc_slots.size() || staged_graph.generation != descriptor.target_generation
      || staged_graph.session_token != descriptor.session_token || staged_graph.high_water != descriptor.after) {
    reject(attempt.violations, DualCsrViolationCode::kGeneration, "index.stage", "graph/index staging boundary differs");
    return attempt;
  }
  if constexpr (incregpusta_debug_checks_enabled)
    attempt.violations = validateDualCsrArcGroupIndex(committed_graph, committed_index);
  auto delta_errors = validateDualCsrArcGroupIndexDelta(
      committed_graph, committed_index, descriptor);
  attempt.violations.insert(attempt.violations.end(),
                            std::make_move_iterator(delta_errors.begin()),
                            std::make_move_iterator(delta_errors.end()));
  if (!attempt.violations.empty())
    return attempt;

  DualCsrArcGroupIndex staged = committed_index;
  staged.session_token = descriptor.session_token;
  staged.generation = descriptor.target_generation;
  staged.arc_high_water = descriptor.after.arc_slots;
  staged.slots.resize(descriptor.after.arc_slots);
  for (const auto& delta : descriptor.arc_group_index_delta) {
    auto& slot = staged.slots[delta.id.value()];
    slot.ref = delta.ref;
    slot.current_active_key = delta.replacement_active_key;
  }
  updateAffectedGroups(committed_graph, committed_index, staged_graph,
                       descriptor, staged);
  if constexpr (incregpusta_debug_checks_enabled)
    attempt.violations = validateDualCsrArcGroupIndex(staged_graph, staged);
  if (attempt.violations.empty())
    attempt.index = std::move(staged);
  return attempt;
}

}  // namespace ista::timing_dual_csr
