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

std::optional<NarrowCaptureError> validateModelBinding(
    const AffectedNodeModelCapture* request,
    timing_edit::TimingEditResolverView staged,
    const timing_edit::AppendOnlyHighWater& after,
    const csr::NodeRecord& node)
{
  if (request == nullptr)
    return std::nullopt;
  const auto* binding = staged.findNodeBinding(request->ref);
  if (request->expected_key != node.key || binding == nullptr
      || binding->tombstone
      || binding->native_name != request->expected_key.full_name
      || binding->slot.value() >= after.node_slots
      || csr::NodeId{binding->slot.value()} != node.id
      || request->expected_cell != node.instance_cell
      || request->expected_port != node.key.pin_name)
    return NarrowCaptureError::kUnplannedDependency;
  return std::nullopt;
}

}  // namespace

std::variant<PlannedNodeCaptureIndex, NarrowCaptureError>
indexPlannedNodeCapture(const AffectedCapturePlan& plan)
{
  PlannedNodeCaptureIndex index;
  const auto add = [&](const timing_edit::StableNodeRef& ref,
                       const csr::NodeKey& key) -> PlannedNodeCapture* {
    auto [it, inserted] = index.try_emplace(
        key.full_name, PlannedNodeCapture{&key, ref});
    if (!inserted && (*it->second.key != key || it->second.ref != ref))
      return nullptr;
    return &it->second;
  };
  for (const auto& request : plan.nodes) {
    auto* entry = add(request.ref, request.expected_key);
    if (entry == nullptr || ++entry->declarations != 1)
      return NarrowCaptureError::kDuplicateRequest;
  }
  for (const auto& request : plan.node_roles) {
    auto* entry = add(request.ref, request.expected_key);
    if (entry == nullptr || entry->role != nullptr)
      return NarrowCaptureError::kDuplicateRequest;
    entry->role = &request;
  }
  for (const auto& request : plan.cell_pin_capacitances) {
    auto* entry = add(request.ref, request.expected_key);
    if (entry == nullptr || entry->capacitance != nullptr)
      return NarrowCaptureError::kDuplicateRequest;
    entry->capacitance = &request;
  }
  for (const auto& request : plan.arcs) {
    const auto endpoints = stableArcEndpointRefs(request.ref);
    if (add(endpoints.source, request.expected_post_base.source) == nullptr
        || add(endpoints.sink, request.expected_post_base.sink) == nullptr)
      return NarrowCaptureError::kDuplicateRequest;
  }
  return index;
}

PlannedNodeId resolvePlannedNodeId(
    const DualSemanticReadView& committed,
    timing_edit::TimingEditResolverView staged,
    const AffectedCapturePlan& plan,
    const PlannedNodeCaptureIndex& index,
    const csr::NodeKey& key)
{
  const auto found = index.find(key.full_name);
  const auto* request = found == index.end() ? nullptr : &found->second;
  if (request != nullptr && *request->key != key)
    return NarrowCaptureError::kBatchBindingMismatch;
  const auto* entry = request == nullptr ? staged.findNodeEntryForKey(key) : nullptr;
  const auto* binding = request == nullptr
      ? (entry ? &entry->second : nullptr)
      : staged.findNodeBinding(request->ref);
  if (binding == nullptr)
    return request == nullptr ? NarrowCaptureError::kUnplannedDependency
                              : NarrowCaptureError::kBatchBindingMismatch;
  if (binding->tombstone || binding->native_name != key.full_name
      || binding->slot.value() >= plan.after.node_slots)
    return NarrowCaptureError::kBatchBindingMismatch;
  const csr::NodeId id{binding->slot.value()};
  if (id.value() < plan.before.node_slots) {
    const auto node = committed.nodeById(id);
    if (!node || node.value().key != key)
      return NarrowCaptureError::kBatchBindingMismatch;
  } else if (request == nullptr || request->declarations != 1) {
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
            || arc.transfer.children().size()
                   != requested.ordered_arc_signatures.size())
          return false;
        for (std::size_t index = 0;
             index < arc.transfer.children().size(); ++index)
          if (arc.transfer.children()[index].signature
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
    const AffectedNodeModelCapture* request,
    timing_edit::TimingEditResolverView staged,
    const timing_edit::AppendOnlyHighWater& after,
    const std::map<std::string, const csr::ReferencedLibraryCellModel*>& cells,
    csr::NodeRecord& node)
{
  if (const auto error = validateModelBinding(request, staged, after, node))
    return *error;
  if (request == nullptr)
    return std::optional<CapturedNodeRole>{};
  const auto cell = cells.find(request->expected_cell);
  if (cell == cells.end())
    return NarrowCaptureError::kUnplannedDependency;
  csr::detail::applyCanonicalNodeLibraryRoles(*cell->second, node);
  return std::optional<CapturedNodeRole>{
      CapturedNodeRole{*request, node.id, node}};
}

CapacitanceProjection projectPlannedCapacitance(
    const ista::Database& database,
    const AffectedNodeModelCapture* request,
    timing_edit::TimingEditResolverView staged,
    const timing_edit::AppendOnlyHighWater& after,
    const csr::NodeRecord& node)
{
  if (const auto error = validateModelBinding(request, staged, after, node))
    return *error;
  if (request == nullptr)
    return NarrowCaptureError::kUnplannedDependency;
  auto model = csr::detail::projectCanonicalCellPinCapacitance(
      database, node.id, node.key);
  return CapturedCellPinCapacitance{*request, std::move(model)};
}

}  // namespace ista::timing_dual_csr::detail
