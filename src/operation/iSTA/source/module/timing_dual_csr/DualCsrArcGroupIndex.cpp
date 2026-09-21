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
#include "DualCsrUpdaterInternal.hpp"
#include <type_traits>
#include "TimingSemanticKey.hpp"

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
  const auto owner = [](const auto& group) { return std::string_view(group.key.owner_name); };
  const auto first = std::ranges::lower_bound(active_net_sink_groups, owner_name, {}, owner);
  const auto last = std::ranges::upper_bound(first, active_net_sink_groups.end(), owner_name, {}, owner);
  const auto found = std::ranges::lower_bound(first, last, sink, csr::nodeKeyLess,
                                             [](const auto& group) -> const csr::NodeKey& { return group.key.sink; });
  return found != last && found->key.sink == sink ? &*found : nullptr;
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

namespace detail {
namespace {

template <typename T>
struct GroupEdit
{
  std::size_t position;
  bool existed;
  bool replacement_present;
  T value;
};

template <typename T, typename Less, typename Key>
void prepareGroupEdit(const std::vector<T>& groups, T replacement, Less less, Key key,
                      std::vector<GroupEdit<T>>& edits)
{
  const auto& wanted = replacement.*key;
  const auto found = std::ranges::lower_bound(groups, wanted, less, key);
  const bool existed = found != groups.end() && !less(wanted, (*found).*key) && !less((*found).*key, wanted);
  const bool present = !replacement.active_arc_ids.empty();
  if (existed || present)
    edits.push_back({static_cast<std::size_t>(found - groups.begin()), existed, present, std::move(replacement)});
}

template <typename T>
void reserveGroupEdits(std::vector<T>& groups, std::vector<GroupEdit<T>>& edits)
{
  std::ranges::reverse(edits);
  std::ranges::stable_sort(edits, [](const auto& lhs, const auto& rhs) { return lhs.position > rhs.position; });
  const auto additions = std::ranges::count_if(edits, [](const auto& value) { return !value.existed; });
  if (static_cast<std::size_t>(additions) > groups.max_size() - groups.size())
    throw std::length_error("Dual-CSR index group capacity exhausted");
  const auto required = groups.size() + additions;
  if (required > groups.capacity())
    groups.reserve(std::max(required, groups.capacity() > groups.max_size() / 2
                                         ? groups.max_size() : std::max<std::size_t>(1, groups.capacity() * 2)));
}

template <typename T>
void applyGroups(std::vector<T>& groups, std::vector<GroupEdit<T>>& edits) noexcept
{
  static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);
  for (auto& edit : edits) {
    auto position = groups.begin() + edit.position;
    if (edit.existed) {
      std::swap(*position, edit.value);
      if (!edit.replacement_present)
        groups.erase(position);
    } else {
      groups.insert(position, std::move(edit.value));
    }
  }
}

template <typename T>
void undoGroups(std::vector<T>& groups, std::vector<GroupEdit<T>>& edits) noexcept
{
  for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
    auto position = groups.begin() + it->position;
    if (!it->existed)
      groups.erase(position);
    else if (!it->replacement_present)
      groups.insert(position, std::move(it->value));
    else
      std::swap(*position, it->value);
  }
}

}  // namespace

struct PreparedDualCsrIndexPatch::Data
{
  DualCsrArcGroupIndex* state;
  timing_edit::TransactionGeneration before_generation, after_generation;
  std::uint64_t before_high_water, after_high_water;
  std::size_t old_slots;
  std::vector<std::pair<std::size_t, DualCsrArcGroupIndexSlot>> slots;
  std::vector<GroupEdit<DualCsrArcGroup>> groups;
  std::vector<GroupEdit<DualCsrArcSinkNetGroup>> sink_nets;
  std::vector<GroupEdit<DualCsrNetSinkGroup>> net_sinks;
  std::optional<std::vector<csr::NodeId>> clock_sources;
  enum class Phase { kPrepared, kApplied, kFinished };
  Phase phase{Phase::kPrepared};

  Data(const DualCsrState& graph, DualCsrArcGroupIndex& index, const DualCsrBatchDescriptor& descriptor)
      : state(&index), before_generation(index.generation), after_generation(descriptor.target_generation),
        before_high_water(index.arc_high_water), after_high_water(descriptor.after.arc_slots), old_slots(index.slots.size())
  {
    for (const auto& delta : descriptor.arc_group_index_delta)
      slots.emplace_back(delta.id.value(), DualCsrArcGroupIndexSlot{delta.ref, delta.replacement_active_key});
    const auto activeKey = [&](csr::ArcId id) -> const std::optional<csr::ArcKey>& {
      const auto changed = std::ranges::lower_bound(descriptor.arc_group_index_delta, id.value(), {}, [](const auto& item) { return item.id.value(); });
      if (changed != descriptor.arc_group_index_delta.end() && changed->id == id)
        return changed->replacement_active_key;
      return index.slots.at(id.value()).current_active_key;
    };
    const auto afterPayload = [&](csr::ArcId id) -> const csr::ArcSemanticPayload& {
      if (id.value() >= graph.arc_slots.size())
        return descriptor.arc_appends.at(id.value() - graph.arc_slots.size()).payload;
      const auto changed = std::ranges::find(descriptor.payload_changes, id, &ArcPayloadChangeDescriptor::id);
      return changed != descriptor.payload_changes.end() ? changed->replacement : graph.arc_slots[id.value()].payload;
    };
    const auto afterNet = [&](csr::ArcId id) -> const std::string* {
      if (id.value() >= descriptor.after.arc_slots || !activeKey(id))
        return nullptr;
      const auto node = afterPayload(id).sink;
      if (node.value() >= graph.nodes.size())
        return &descriptor.node_appends.at(node.value() - graph.nodes.size()).payload.net_name;
      const auto changed = std::ranges::find(descriptor.node_payload_changes, node, &NodePayloadChange::id);
      return changed != descriptor.node_payload_changes.end() ? &changed->replacement_net_name : &graph.nodes[node.value()].net_name;
    };
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
    if (const auto* group = index.activeGroup(base))
      for (const auto id : group->active_arc_ids)
        candidates.insert(id.value());
    std::vector<csr::ArcId> ids;
    for (const auto id : candidates) {
      if (id >= descriptor.after.arc_slots)
        continue;
      const auto& key = activeKey(csr::ArcId{id});
      if (key && key->base == base)
        ids.push_back(csr::ArcId{id});
    }
    std::ranges::sort(ids, [&](csr::ArcId lhs, csr::ArcId rhs) {
      return activeKey(lhs)->parallel_ordinal
             < activeKey(rhs)->parallel_ordinal;
    });
    prepareGroupEdit(index.active_groups, DualCsrArcGroup{base, std::move(ids)}, ArcBaseLess{}, &DualCsrArcGroup::base, groups);
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
    if (const auto* group = index.activeNetSinkGroup(
            key.owner_name, key.sink))
      for (const auto id : group->active_arc_ids)
        candidates.insert(id.value());
    std::vector<csr::ArcId> ids;
    for (const auto id : candidates) {
      if (id >= descriptor.after.arc_slots)
        continue;
      const auto& active = activeKey(csr::ArcId{id});
      if (active && active->base.kind == csr::ArcKind::kNet
          && active->base.owner_name == key.owner_name
          && active->base.sink == key.sink)
        ids.push_back(csr::ArcId{id});
    }
    std::ranges::sort(ids, {}, &csr::ArcId::value);
    prepareGroupEdit(index.active_net_sink_groups, DualCsrNetSinkGroup{key, std::move(ids)}, NetSinkLess{}, &DualCsrNetSinkGroup::key, net_sinks);
  }

  std::set<std::string> affected_nets;
  std::set<std::uint64_t> net_changed_ids = changed_ids;
  for (const auto& delta : descriptor.arc_group_index_delta) {
    if (const auto* net = sinkNet(graph, delta.id))
      affected_nets.insert(*net);
    if (const auto* net = afterNet(delta.id))
      affected_nets.insert(*net);
  }
  for (const auto& change : descriptor.node_payload_changes) {
    affected_nets.insert(change.expected_net_name);
    affected_nets.insert(change.replacement_net_name);
    if (const auto* group = index.activeSinkNetGroup(
            change.expected_net_name))
      for (const auto id : group->active_arc_ids)
        if (id.value() < graph.arc_slots.size()
            && graph.arc_slots[id.value()].payload.sink
                   == change.id)
          net_changed_ids.insert(id.value());
  }
  for (const auto& net_name : affected_nets) {
    std::set<std::uint64_t> candidates = net_changed_ids;
    if (const auto* group
        = index.activeSinkNetGroup(net_name))
      for (const auto id : group->active_arc_ids)
        candidates.insert(id.value());
    std::vector<csr::ArcId> ids;
    for (const auto id : candidates) {
      const csr::ArcId arc_id{id};
      const auto* actual_net = afterNet(arc_id);
      if (actual_net != nullptr && *actual_net == net_name)
        ids.push_back(arc_id);
    }
    std::ranges::sort(ids, {}, &csr::ArcId::value);
    prepareGroupEdit(index.active_sink_net_groups, DualCsrArcSinkNetGroup{net_name, std::move(ids)}, std::ranges::less{}, &DualCsrArcSinkNetGroup::sink_net_name, sink_nets);
  }

    if (descriptor.reference_payload.clocks) {
      clock_sources.emplace();
      for (const auto& clock : descriptor.reference_payload.clocks->replacement)
        for (const auto& source : clock.ordered_sources)
          if (source.resolved_node)
            clock_sources->push_back(*source.resolved_node);
      std::ranges::sort(*clock_sources, {}, &csr::NodeId::value);
      clock_sources->erase(std::ranges::unique(*clock_sources).begin(), clock_sources->end());
    }
    if (after_high_water > index.slots.max_size())
      throw std::length_error("Dual-CSR index slot capacity exhausted");
    if (after_high_water > index.slots.capacity())
      index.slots.reserve(std::max<std::size_t>(after_high_water, index.slots.capacity() > index.slots.max_size() / 2
                                                   ? index.slots.max_size() : std::max<std::size_t>(1, index.slots.capacity() * 2)));
    reserveGroupEdits(index.active_groups, groups);
    reserveGroupEdits(index.active_sink_net_groups, sink_nets);
    reserveGroupEdits(index.active_net_sink_groups, net_sinks);
  }

  void apply() noexcept
  {
    if (phase != Phase::kPrepared)
      return;
    state->slots.resize(after_high_water);
    for (auto& [id, value] : slots)
      std::swap(state->slots[id], value);
    applyGroups(state->active_groups, groups);
    applyGroups(state->active_sink_net_groups, sink_nets);
    applyGroups(state->active_net_sink_groups, net_sinks);
    if (clock_sources)
      state->clock_source_node_ids.swap(*clock_sources);
    state->generation = after_generation;
    state->arc_high_water = after_high_water;
    phase = Phase::kApplied;
  }
  void rollback() noexcept
  {
    if (phase != Phase::kApplied)
      return;
    undoGroups(state->active_net_sink_groups, net_sinks);
    undoGroups(state->active_sink_net_groups, sink_nets);
    undoGroups(state->active_groups, groups);
    for (auto& [id, value] : slots)
      std::swap(state->slots[id], value);
    state->slots.resize(old_slots);
    if (clock_sources)
      state->clock_source_node_ids.swap(*clock_sources);
    state->generation = before_generation;
    state->arc_high_water = before_high_water;
    phase = Phase::kFinished;
  }
};

PreparedDualCsrIndexPatch::PreparedDualCsrIndexPatch(const DualCsrState& graph, DualCsrArcGroupIndex& index, const DualCsrBatchDescriptor& descriptor)
    : _data(std::make_unique<Data>(graph, index, descriptor))
{
}
PreparedDualCsrIndexPatch::PreparedDualCsrIndexPatch(PreparedDualCsrIndexPatch&&) noexcept = default;
PreparedDualCsrIndexPatch::~PreparedDualCsrIndexPatch() { rollback(); }
void PreparedDualCsrIndexPatch::apply() noexcept { _data->apply(); }
void PreparedDualCsrIndexPatch::rollback() noexcept { if (_data) _data->rollback(); }
void PreparedDualCsrIndexPatch::commit() noexcept { if (_data) _data->phase = Data::Phase::kFinished; }

}  // namespace detail

}  // namespace ista::timing_dual_csr
