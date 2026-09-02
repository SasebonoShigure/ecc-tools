// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDualCsrOutgoingOrderInternal.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>

#include "Database.hpp"
#include "StableArcEndpointRefsInternal.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

using NodeNames = std::map<std::uint64_t, std::string>;

struct AffectedNames
{
  NodeNames sources;
  NodeNames sinks;
};

std::optional<NarrowCaptureError> addNode(
    NodeNames& nodes,
    const timing_edit::StableNodeRef& ref,
    const csr::NodeKey& key,
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan)
{
  const auto binding = staged.nodeBindings().find(ref);
  if (binding == staged.nodeBindings().end()
      || binding->second.tombstone
      || binding->second.native_name != key.full_name
      || binding->second.slot.value() >= plan.after.node_slots)
    return NarrowCaptureError::kBatchBindingMismatch;
  const auto [found, inserted] = nodes.emplace(
      binding->second.slot.value(), binding->second.native_name);
  if (!inserted && found->second != binding->second.native_name)
    return NarrowCaptureError::kDuplicateRequest;
  return std::nullopt;
}

std::variant<AffectedNames, NarrowCaptureError> affectedNames(
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan)
{
  AffectedNames result;
  for (const auto& assignment : plan.slot_plan.nodes) {
    if (assignment.tombstone)
      continue;
    const auto request = std::ranges::find(
        plan.nodes, assignment.ref, &AffectedNodeCapture::ref);
    if (request == plan.nodes.end())
      return NarrowCaptureError::kUnplannedDependency;
    if (const auto error = addNode(
            result.sources, request->ref, request->expected_key,
            staged, plan))
      return *error;
    if (const auto error = addNode(
            result.sinks, request->ref, request->expected_key,
            staged, plan))
      return *error;
  }
  for (const auto& assignment : plan.slot_plan.arcs) {
    const auto request = std::ranges::find(
        plan.arcs, assignment.ref, &AffectedArcCapture::ref);
    if (request == plan.arcs.end())
      return NarrowCaptureError::kUnplannedDependency;
    const auto endpoints = stableArcEndpointRefs(request->ref);
    if (const auto error = addNode(
            result.sources, endpoints.source,
            request->expected_post_base.source,
            staged, plan))
      return *error;
    if (const auto error = addNode(
            result.sinks, endpoints.sink,
            request->expected_post_base.sink,
            staged, plan))
      return *error;
  }
  return result;
}

std::optional<csr::ArcId> committedId(
    const NativeCandidateArcOrderFact& fact,
    const DualSemanticReadView& committed,
    const DualCsrArcGroupIndex& index)
{
  const auto* group = index.activeGroup(fact.key.base);
  if (group == nullptr)
    return std::nullopt;
  std::optional<csr::ArcId> result;
  for (const auto id : group->active_arc_ids) {
    const auto* slot = index.slot(id);
    if (slot == nullptr || slot->current_active_key != fact.key)
      continue;
    if (result)
      return std::nullopt;
    result = id;
  }
  if (!result)
    return std::nullopt;
  const auto payload = committed.arcById(*result);
  if (!payload || payload.value().key != fact.key
      || payload.value().source != fact.source
      || payload.value().sink != fact.sink
      || payload.value().kind != fact.key.base.kind
      || payload.value().is_clock_arc != fact.is_clock_arc
      || payload.value().is_disabled != fact.is_disabled
      || payload.value().is_loop_disabled != fact.is_loop_disabled
      || payload.value().transfer != fact.transfer)
    return std::nullopt;
  return result;
}

}  // namespace

NativeArcOrderResult captureNativeAffectedArcRows(
    const ista::Database& database,
    const DualSemanticReadView& committed,
    const DualCsrArcGroupIndex& committed_index,
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan,
    std::span<const NativeCandidateArcOrderFact> candidates)
{
  const auto selected = affectedNames(staged, plan);
  if (const auto* error = std::get_if<NarrowCaptureError>(&selected))
    return *error;
  const auto& names = std::get<AffectedNames>(selected);

  std::map<std::size_t, std::size_t> candidate_by_native;
  std::vector<csr::ArcId> ids;
  ids.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto& candidate = candidates[index];
    if (!candidate_by_native.emplace(candidate.native_index, index).second)
      return NarrowCaptureError::kDuplicateRequest;
    const auto id = candidate.assigned_id
                        ? candidate.assigned_id
                        : committedId(candidate, committed,
                                      committed_index);
    if (!id)
      return NarrowCaptureError::kUnplannedDependency;
    ids.push_back(*id);
  }

  NativeAffectedArcRows result;
  result.outgoing.reserve(names.sources.size());
  for (const auto& [source_value, source_name] : names.sources) {
    CapturedOutgoingRow row{csr::NodeId{source_value}, {}};
    const auto native_row = database.get_outgoing_arc_list_map().find(
        source_name);
    if (native_row != database.get_outgoing_arc_list_map().end()) {
      std::set<std::uint64_t> row_ids;
      for (const auto native_index : native_row->second) {
        const auto candidate = candidate_by_native.find(native_index);
        if (candidate == candidate_by_native.end())
          return NarrowCaptureError::kUnplannedDependency;
        const auto position = candidate->second;
        if (candidates[position].source != row.source
            || !row_ids.insert(ids[position].value()).second)
          return NarrowCaptureError::kMalformedNativeRecord;
        row.active_arc_ids.push_back(ids[position]);
      }
    }
    result.outgoing.push_back(std::move(row));
  }
  result.incoming.reserve(names.sinks.size());
  for (const auto& [sink_value, sink_name] : names.sinks) {
    CapturedIncomingRow row{csr::NodeId{sink_value}, {}};
    const auto native_row = database.get_incoming_arc_list_map().find(
        sink_name);
    if (native_row != database.get_incoming_arc_list_map().end()) {
      std::set<std::uint64_t> row_ids;
      for (const auto native_index : native_row->second) {
        const auto candidate = candidate_by_native.find(native_index);
        if (candidate == candidate_by_native.end())
          return NarrowCaptureError::kUnplannedDependency;
        const auto position = candidate->second;
        if (candidates[position].sink != row.sink
            || !row_ids.insert(ids[position].value()).second)
          return NarrowCaptureError::kMalformedNativeRecord;
        row.active_arc_ids.push_back(ids[position]);
      }
    }
    result.incoming.push_back(std::move(row));
  }
  return result;
}

}  // namespace ista::timing_dual_csr::detail
