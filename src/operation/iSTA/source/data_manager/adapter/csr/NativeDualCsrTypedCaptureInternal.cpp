// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDualCsrTypedCaptureInternal.hpp"

#include <algorithm>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "Database.hpp"
#include "StableArcEndpointRefsInternal.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

std::variant<const AffectedNodeModelCapture*, NarrowCaptureError>
matchRequest(
    const std::vector<AffectedNodeModelCapture>& requests,
    const timing_edit::TimingEditResolverState& staged,
    const timing_edit::AppendOnlyHighWater& after,
    const csr::NodeRecord& node)
{
  const AffectedNodeModelCapture* result = nullptr;
  for (const auto& request : requests) {
    if (request.expected_key != node.key)
      continue;
    if (result != nullptr)
      return NarrowCaptureError::kDuplicateRequest;
    result = &request;
  }
  if (result == nullptr)
    return result;
  const auto binding = staged.nodeBindings().find(result->ref);
  if (binding == staged.nodeBindings().end()
      || binding->second.tombstone
      || binding->second.native_name != result->expected_key.full_name
      || binding->second.slot.value() >= after.node_slots
      || csr::NodeId{binding->second.slot.value()} != node.id
      || result->expected_cell != node.instance_cell
      || result->expected_port != node.key.pin_name)
    return NarrowCaptureError::kUnplannedDependency;
  return result;
}

}  // namespace

PlannedNodeId resolvePlannedNodeId(
    const DualSemanticReadView& committed,
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan,
    const csr::NodeKey& key)
{
  std::optional<timing_edit::StableNodeRef> matched;
  const auto consider = [&](const timing_edit::StableNodeRef& ref,
                            const csr::NodeKey& expected) {
    if (expected != key)
      return true;
    if (matched && *matched != ref)
      return false;
    matched = ref;
    return true;
  };
  for (const auto& request : plan.nodes)
    if (!consider(request.ref, request.expected_key))
      return NarrowCaptureError::kDuplicateRequest;
  for (const auto& request : plan.node_roles)
    if (!consider(request.ref, request.expected_key))
      return NarrowCaptureError::kDuplicateRequest;
  for (const auto& request : plan.cell_pin_capacitances)
    if (!consider(request.ref, request.expected_key))
      return NarrowCaptureError::kDuplicateRequest;
  for (const auto& request : plan.arcs) {
    const auto endpoints = stableArcEndpointRefs(request.ref);
    if (!consider(endpoints.source,
                  request.expected_post_base.source)
        || !consider(endpoints.sink,
                     request.expected_post_base.sink))
      return NarrowCaptureError::kDuplicateRequest;
  }
  if (!matched) {
    const std::optional<csr::NodeId> committed_id
        = committed.nodeIdByKey(key);
    if (!committed_id
        || committed_id->value() >= plan.before.node_slots)
      return NarrowCaptureError::kUnplannedDependency;
    const auto committed_node = committed.nodeById(*committed_id);
    if (!committed_node || committed_node.value().key != key)
      return NarrowCaptureError::kBatchBindingMismatch;

    const timing_edit::StableNodeRef existing_ref{
        timing_edit::ExistingNodeRef{key}};
    const auto binding = staged.nodeBindings().find(existing_ref);
    if (binding == staged.nodeBindings().end()
        || binding->second.tombstone
        || binding->second.native_name != key.full_name
        || binding->second.slot.value() != committed_id->value()
        || binding->second.slot.value() >= plan.after.node_slots)
      return NarrowCaptureError::kBatchBindingMismatch;
    return *committed_id;
  }
  const auto binding = staged.nodeBindings().find(*matched);
  if (binding == staged.nodeBindings().end()
      || binding->second.tombstone
      || binding->second.native_name != key.full_name
      || binding->second.slot.value() >= plan.after.node_slots)
    return NarrowCaptureError::kBatchBindingMismatch;
  const csr::NodeId id{binding->second.slot.value()};
  if (id.value() < plan.before.node_slots) {
    const auto node = committed.nodeById(id);
    if (!node || node.value().key != key)
      return NarrowCaptureError::kBatchBindingMismatch;
  } else if (std::ranges::count_if(
                 plan.nodes, [&](const auto& request) {
                   return request.ref == *matched
                          && request.expected_key == key;
                 })
             != 1) {
    return NarrowCaptureError::kUnplannedDependency;
  }
  return id;
}

std::optional<NarrowCaptureError> validateCapturedLibraryCell(
    const timing_edit::LibraryCellRef& requested,
    const csr::ReferencedLibraryCellModel& model)
{
  if (!model.present || model.cell_name != requested.cell_name
      || model.library_name != requested.library_name)
    return NarrowCaptureError::kMalformedNativeRecord;
  const auto portExists = [&](const std::string& name) {
    return std::ranges::any_of(model.ports, [&](const auto& port) {
      return port.name == name;
    });
  };
  if (!portExists(requested.input_port)
      || !portExists(requested.output_port))
    return NarrowCaptureError::kMalformedNativeRecord;
  const bool arc_matches = std::ranges::any_of(
      model.cell_arcs, [&](const auto& arc) {
        if (arc.source_port != requested.input_port
            || arc.sink_port != requested.output_port
            || arc.transfer.children.size()
                   != requested.ordered_arc_signatures.size())
          return false;
        for (std::size_t index = 0;
             index < arc.transfer.children.size(); ++index)
          if (arc.transfer.children[index].signature
              != requested.ordered_arc_signatures[index])
            return false;
        return true;
      });
  if (!arc_matches || !model.timing_checks.empty()
      || model.is_sequential || model.is_clock_gating || model.is_macro)
    return NarrowCaptureError::kUnsupportedPolicy;
  return std::nullopt;
}

NodeRoleProjection projectPlannedNodeRole(
    const std::vector<AffectedNodeModelCapture>& requests,
    const timing_edit::TimingEditResolverState& staged,
    const timing_edit::AppendOnlyHighWater& after,
    const std::map<std::string, csr::ReferencedLibraryCellModel>& cells,
    csr::NodeRecord& node)
{
  const auto matched = matchRequest(requests, staged, after, node);
  if (const auto* error = std::get_if<NarrowCaptureError>(&matched))
    return *error;
  const auto* request
      = std::get<const AffectedNodeModelCapture*>(matched);
  if (request == nullptr)
    return std::optional<CapturedNodeRole>{};
  const auto cell = cells.find(request->expected_cell);
  if (cell == cells.end())
    return NarrowCaptureError::kUnplannedDependency;
  csr::detail::applyCanonicalNodeLibraryRoles(cell->second, node);
  return std::optional<CapturedNodeRole>{
      CapturedNodeRole{*request, node.id, node}};
}

CapacitanceProjection projectPlannedCapacitance(
    const ista::Database& database,
    const std::vector<AffectedNodeModelCapture>& requests,
    const timing_edit::TimingEditResolverState& staged,
    const timing_edit::AppendOnlyHighWater& after,
    const csr::NodeRecord& node)
{
  const auto matched = matchRequest(requests, staged, after, node);
  if (const auto* error = std::get_if<NarrowCaptureError>(&matched))
    return *error;
  const auto* request
      = std::get<const AffectedNodeModelCapture*>(matched);
  if (request == nullptr)
    return NarrowCaptureError::kUnplannedDependency;
  auto model = csr::detail::projectCanonicalCellPinCapacitance(
      database, node.id, node.key);
  return CapturedCellPinCapacitance{*request, std::move(model)};
}

}  // namespace ista::timing_dual_csr::detail
