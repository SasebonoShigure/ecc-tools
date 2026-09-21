// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrStructuredCapturePlanInternal.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "StableArcEndpointRefsInternal.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("dual csr structured capture plan: "
                              + std::move(message));
}

struct ArcBaseLess
{
  bool operator()(const csr::ArcKeyBase& lhs,
                  const csr::ArcKeyBase& rhs) const
  {
    return csr::arcKeyLess(csr::ArcKey{lhs, 0},
                           csr::ArcKey{rhs, 0});
  }
};

csr::ArcId stagedArcId(
    const timing_edit::StableArcRef& ref,
    timing_edit::TimingEditResolverView staged,
    const timing_edit::AppendOnlyHighWater& after)
{
  const auto* found = staged.findArcBinding(ref);
  if (found == nullptr
      || found->slot.value() >= after.arc_slots)
    invalid("requested stable arc has no staged logical ArcId");
  return csr::ArcId{found->slot.value()};
}

std::optional<csr::ArcKey> currentKey(
    const timing_edit::StableArcRef& ref,
    const timing_edit::AppendOnlySlotPlan& slots,
    const timing_edit::TimingEditResolverState& committed,
    timing_edit::TimingEditResolverView staged,
    const DualCsrArcGroupIndex& index,
    const DualSemanticReadView& view)
{
  const auto id = stagedArcId(ref, staged, slots.after);
  if (id.value() >= slots.before.arc_slots)
    return std::nullopt;
  const auto committed_binding = committed.arcBindings().find(ref);
  if (committed_binding == committed.arcBindings().end()
      || committed_binding->second.tombstone
      || committed_binding->second.slot.value() != id.value())
    invalid("current stable arc disagrees with committed resolver");
  const auto* indexed = index.slot(id);
  if (indexed == nullptr || indexed->ref != ref
      || !indexed->current_active_key.has_value())
    invalid("current stable arc disagrees with committed group index");
  const auto payload = view.arcById(id);
  if (!payload
      || payload.value().key != *indexed->current_active_key)
    invalid("current stable arc disagrees with keyed committed view");
  return indexed->current_active_key;
}

csr::ArcKeyBase postBase(
    const timing_edit::StableArcRef& ref,
    const std::optional<csr::ArcKey>& current,
    timing_edit::TimingEditResolverView staged)
{
  if (current.has_value())
    return current->base;
  return materializedArcKey(ref, staged).base;
}

void addCurrentGroup(
    const csr::ArcKeyBase& base,
    const DualCsrArcGroupIndex& index,
    std::set<timing_edit::StableArcRef>& requested)
{
  const auto* group = index.activeGroup(base);
  if (group == nullptr)
    return;
  for (const auto id : group->active_arc_ids) {
    const auto* slot = index.slot(id);
    if (slot == nullptr || !slot->current_active_key.has_value()
        || slot->current_active_key->base != base)
      invalid("committed group index contains an invalid active peer");
    requested.insert(slot->ref);
  }
}

std::uint64_t postMultiplicity(
    const csr::ArcKeyBase& base,
    const timing_edit::PreparedBatch& batch,
    timing_edit::TimingEditResolverView staged,
    const DualCsrArcGroupIndex& index,
    const std::set<timing_edit::StableArcRef>& deactivated)
{
  std::uint64_t count = 0;
  if (const auto* group = index.activeGroup(base)) {
    for (const auto id : group->active_arc_ids) {
      const auto* slot = index.slot(id);
      if (slot == nullptr)
        invalid("committed group index contains a missing slot");
      if (!deactivated.contains(slot->ref))
        ++count;
    }
  }
  for (const auto& assignment : batch.slotPlan().arcs) {
    if (assignment.tombstone)
      continue;
    if (assignment.slot.value() < batch.slotPlan().before.arc_slots
        || assignment.slot.value() >= batch.slotPlan().after.arc_slots)
      invalid("active arc assignment is not an append slot");
    if (materializedArcKey(assignment.ref, staged).base == base)
      ++count;
  }
  return count;
}

AffectedNodeModelCapture nodeModelRequest(
    const timing_edit::StableNodeRef& ref,
    const timing_edit::PreparedBatch& batch,
    timing_edit::TimingEditResolverView staged,
    const DualSemanticReadView& view)
{
  const auto key = stableNodeKey(ref, staged);
  if (key.kind != csr::NodeKind::kInstancePin
      || key.instance_name.empty() || key.pin_name.empty())
    invalid("node-model dependency is not an instance pin");
  const auto* binding = staged.findNodeBinding(ref);
  if (binding == nullptr
      || binding->tombstone
      || binding->native_name != key.full_name
      || binding->slot.value()
             >= batch.slotPlan().after.node_slots)
    invalid("node-model dependency has no staged node slot");
  std::string cell;
  if (binding->slot.value()
      < batch.slotPlan().before.node_slots) {
    const auto current = view.nodeById(
        csr::NodeId{binding->slot.value()});
    if (!current || current.value().key != key)
      invalid("existing node-model dependency differs from its slot");
    cell = current.value().instance_cell;
  } else {
    const auto* created = std::get_if<timing_edit::CreatedNodeRef>(&ref);
    if (created == nullptr)
      invalid("appended node-model dependency has no created ref");
    const auto edit = std::ranges::find(
        batch.normalizedEdits(), created->creation_token,
        &timing_edit::BufferInsertionMutation::creation_token);
    if (edit == batch.normalizedEdits().end())
      invalid("created node-model dependency has no semantic edit");
    cell = edit->library_cell.cell_name;
  }
  if (cell.empty())
    invalid("node-model dependency has no exact cell identity");
  return {ref, key, std::move(cell), key.pin_name};
}

void addNodeModelRequest(
    std::map<timing_edit::StableNodeRef,
             AffectedNodeModelCapture>& requests,
    AffectedNodeModelCapture request)
{
  const auto [found, inserted]
      = requests.emplace(request.ref, request);
  if (!inserted && found->second != request)
    invalid("one StableNodeRef has conflicting model dependencies");
}

}  // namespace

AffectedCapturePlan makeStructuredCapturePlan(
    const timing_edit::PreparedBatch& batch,
    const timing_edit::TimingEditResolverState& committed,
    timing_edit::TimingEditResolverView staged,
    const DualCsrArcGroupIndex& arc_group_index,
    const DualSemanticReadView& view)
{
  std::map<timing_edit::StableNodeRef, csr::NodeKey> nodes;
  for (const auto& assignment : batch.slotPlan().nodes)
    if (!assignment.tombstone)
      nodes.emplace(assignment.ref,
                    stableNodeKey(assignment.ref, staged));
  for (const auto& edit : batch.normalizedEdits())
    for (const auto& sink : edit.moved_sinks)
      nodes.emplace(sink, stableNodeKey(sink, staged));

  std::map<timing_edit::StableNetRef, std::string> load_nets;
  for (const auto& ref : batch.invalidationClosure().load_nets)
    load_nets.emplace(ref, staged.resolveNet(ref));

  std::set<timing_edit::StableArcRef> deactivated;
  for (const auto& assignment : batch.slotPlan().arcs)
    if (assignment.tombstone)
      deactivated.insert(assignment.ref);
  std::set<timing_edit::StableArcRef> requested = deactivated;
  requested.insert(batch.invalidationClosure().arc_delays.begin(),
                   batch.invalidationClosure().arc_delays.end());
  for (const auto& [ref, net_name] : load_nets) {
    static_cast<void>(ref);
    const auto* group = arc_group_index.activeSinkNetGroup(net_name);
    if (group == nullptr)
      continue;
    for (const auto id : group->active_arc_ids) {
      const auto* slot = arc_group_index.slot(id);
      if (slot == nullptr || !slot->current_active_key.has_value())
        invalid("sink-net group contains an invalid active ArcId");
      requested.insert(slot->ref);
    }
  }
  for (const auto& assignment : batch.slotPlan().arcs)
    if (!assignment.tombstone)
      requested.insert(assignment.ref);

  std::set<csr::ArcKeyBase, ArcBaseLess> affected_bases;
  for (const auto& ref : requested) {
    const auto current = currentKey(
        ref, batch.slotPlan(), committed, staged, arc_group_index,
        view);
    if (deactivated.contains(ref) && !current.has_value())
      invalid("logical deactivation has no exact current active key");
    affected_bases.insert(postBase(ref, current, staged));
  }
  for (const auto& base : affected_bases)
    addCurrentGroup(base, arc_group_index, requested);

  std::map<csr::ArcKeyBase, std::uint64_t, ArcBaseLess>
      post_multiplicities;
  for (const auto& base : affected_bases)
    post_multiplicities.emplace(
        base, postMultiplicity(base, batch, staged, arc_group_index,
                               deactivated));

  std::vector<AffectedArcCapture> arcs;
  for (const auto& ref : requested) {
    const auto current = currentKey(
        ref, batch.slotPlan(), committed, staged, arc_group_index,
        view);
    const auto base = postBase(ref, current, staged);
    if (!affected_bases.contains(base))
      invalid("group expansion escaped its selected affected base");
    if (deactivated.contains(ref)) {
      arcs.push_back({ref, current, base, 0});
      continue;
    }
    const auto count = post_multiplicities.find(base);
    if (count == post_multiplicities.end() || count->second == 0)
      invalid("active affected base has no post-state peer group");
    arcs.push_back({ref, current, base, count->second});
  }

  std::map<timing_edit::StableNodeRef, AffectedNodeModelCapture>
      role_requests;
  for (const auto& materialization : batch.tokenMaterializations()) {
    for (const auto role : {timing_edit::CreatedNodeRole::kInput,
                            timing_edit::CreatedNodeRole::kOutput}) {
      const timing_edit::StableNodeRef ref{
          timing_edit::CreatedNodeRef{materialization.creation_token,
                                      role}};
      addNodeModelRequest(role_requests,
                          nodeModelRequest(ref, batch, staged, view));
    }
  }
  auto cap_requests = role_requests;
  for (const auto& [ref, key] : nodes)
    if (key.kind == csr::NodeKind::kInstancePin)
      addNodeModelRequest(cap_requests,
                          nodeModelRequest(ref, batch, staged, view));
  for (const auto& arc : arcs) {
    if (arc.expected_post_multiplicity == 0
        || arc.expected_post_base.kind != csr::ArcKind::kNet
        || arc.expected_post_base.sink.kind
               != csr::NodeKind::kInstancePin
        || std::ranges::none_of(load_nets, [&](const auto& net) {
             return net.second == arc.expected_post_base.owner_name;
           }))
      continue;
    const auto ref = stableArcEndpointRefs(arc.ref).sink;
    auto sink_request
        = nodeModelRequest(ref, batch, staged, view);
    if (sink_request.expected_key != arc.expected_post_base.sink)
      invalid("post net-load sink disagrees with its stable arc ref");
    addNodeModelRequest(cap_requests,
                        std::move(sink_request));
  }

  std::vector<AffectedNodeCapture> node_requests;
  for (auto& [ref, key] : nodes)
    node_requests.push_back({std::move(ref), std::move(key)});
  std::vector<AffectedNetCapture> net_requests;
  for (auto& [ref, name] : load_nets)
    net_requests.push_back({std::move(ref), std::move(name)});
  std::vector<AffectedNodeModelCapture> roles;
  for (auto& [ref, request] : role_requests) {
    static_cast<void>(ref);
    roles.push_back(std::move(request));
  }
  std::vector<AffectedNodeModelCapture> caps;
  for (auto& [ref, request] : cap_requests) {
    static_cast<void>(ref);
    caps.push_back(std::move(request));
  }
  std::ranges::sort(arcs, [](const auto& lhs, const auto& rhs) {
    return timing_edit::canonicalStableArcRefBytes(lhs.ref)
           < timing_edit::canonicalStableArcRefBytes(rhs.ref);
  });
  return makeAffectedCapturePlan(batch, std::move(node_requests),
                                 std::move(net_requests),
                                  std::move(roles), std::move(caps),
                                 std::move(arcs));
}

}  // namespace ista::timing_dual_csr::detail
