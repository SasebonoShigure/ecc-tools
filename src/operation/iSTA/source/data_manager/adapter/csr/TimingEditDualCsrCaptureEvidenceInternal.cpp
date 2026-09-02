// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "TimingEditDualCsrCaptureEvidenceInternal.hpp"

#include <algorithm>
#include <set>
#include <string>

#include "NarrowTimingCaptureInternal.hpp"
#include "StableArcEndpointRefsInternal.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

const csr::ReferencedLibraryCellModel* cellModel(
    const PendingStructuredBatch& pending,
    const NarrowTimingRecords& records,
    const std::string& cell_name)
{
  const csr::ReferencedLibraryCellModel* result = nullptr;
  for (const auto& captured : records.library_cells) {
    if (captured.model.cell_name != cell_name)
      continue;
    if (result != nullptr)
      return nullptr;
    result = &captured.model;
  }
  for (const auto& current : pending.expectedBefore()
                                 .graph()
                                 .reference_payload
                                 .referenced_library_cells) {
    if (current.cell_name != cell_name)
      continue;
    if (result != nullptr && *result != current)
      return nullptr;
    result = &current;
  }
  return result;
}

std::optional<csr::NodeId> boundNodeId(
    const PendingStructuredBatch& pending,
    const AffectedNodeModelCapture& request)
{
  const auto& staged = pending.stagedResolver();
  const auto found = staged.nodeBindings().find(request.ref);
  if (found == staged.nodeBindings().end() || found->second.tombstone
      || found->second.native_name != request.expected_key.full_name
      || found->second.slot.value()
             >= pending.preparedBatch().slotPlan().after.node_slots)
    return std::nullopt;
  return csr::NodeId{found->second.slot.value()};
}

const csr::NodeRecord* nodeRecord(
    const PendingStructuredBatch& pending,
    const NarrowTimingRecords& records, csr::NodeId id)
{
  const csr::NodeRecord* result = nullptr;
  for (const auto& captured : records.nodes) {
    if (captured.id != id)
      continue;
    if (result != nullptr)
      return nullptr;
    result = &captured.record;
  }
  if (result != nullptr)
    return result;
  const auto& before = pending.expectedBefore().graph();
  if (id.value() >= before.nodes.size())
    return nullptr;
  return &before.nodes[id.value()];
}

bool exactPlanSets(const PendingStructuredBatch& pending)
{
  const auto& plan = pending.capturePlan();
  std::set<timing_edit::StableNodeRef> roles;
  for (const auto& materialization : plan.token_materializations) {
    roles.insert(timing_edit::StableNodeRef{
        timing_edit::CreatedNodeRef{
            materialization.creation_token,
            timing_edit::CreatedNodeRole::kInput}});
    roles.insert(timing_edit::StableNodeRef{
        timing_edit::CreatedNodeRef{
            materialization.creation_token,
            timing_edit::CreatedNodeRole::kOutput}});
  }
  if (roles.size() != plan.node_roles.size()
      || std::ranges::any_of(plan.node_roles, [&](const auto& request) {
           return !roles.contains(request.ref);
         }))
    return false;

  std::set<timing_edit::StableNodeRef> caps = roles;
  for (const auto& node : plan.nodes)
    if (node.expected_key.kind == csr::NodeKind::kInstancePin)
      caps.insert(node.ref);
  for (const auto& arc : plan.arcs) {
    if (arc.expected_post_multiplicity == 0
        || arc.expected_post_base.kind != csr::ArcKind::kNet
        || arc.expected_post_base.sink.kind
               != csr::NodeKind::kInstancePin
        || std::ranges::none_of(plan.load_nets, [&](const auto& net) {
             return net.expected_name
                    == arc.expected_post_base.owner_name;
           }))
      continue;
    caps.insert(stableArcEndpointRefs(arc.ref).sink);
  }
  return caps.size() == plan.cell_pin_capacitances.size()
         && std::ranges::none_of(
             plan.cell_pin_capacitances, [&](const auto& request) {
               return !caps.contains(request.ref);
             });
}

std::optional<std::string> validateRoles(
    const PendingStructuredBatch& pending,
    const NarrowTimingRecords& records)
{
  const auto& requests = pending.capturePlan().node_roles;
  if (records.node_roles.size() != requests.size())
    return "captured node-role coverage is not exact";
  std::set<timing_edit::StableNodeRef> seen;
  for (const auto& request : requests) {
    if (!seen.insert(request.ref).second)
      return "node-role request repeats StableNodeRef";
    const CapturedNodeRole* captured = nullptr;
    for (const auto& value : records.node_roles) {
      if (value.requested != request)
        continue;
      if (captured != nullptr)
        return "node-role evidence repeats one typed request";
      captured = &value;
    }
    const auto id = boundNodeId(pending, request);
    const auto* cell = cellModel(pending, records,
                                 request.expected_cell);
    const auto* node = id ? nodeRecord(pending, records, *id) : nullptr;
    if (captured == nullptr || !id || cell == nullptr || node == nullptr
        || captured->id != *id || captured->record.id != *id
        || captured->record.key != request.expected_key
        || captured->record.instance_cell != request.expected_cell
        || node->key != request.expected_key
        || node->instance_cell != request.expected_cell)
      return "node-role evidence has a mismatched typed identity";
    if (captured->record.is_sequential
            != (cell->present && cell->is_sequential)
        || captured->record.is_clock_pin
               != (cell->present
                   && request.expected_port == cell->clock_pin_name)
        || captured->record.is_data_pin
               != (cell->present
                   && request.expected_port == cell->data_pin_name)
        || captured->record.is_output_pin
               != (cell->present
                   && request.expected_port == cell->output_pin_name))
      return "node-role evidence differs from the exact library model";
  }
  return std::nullopt;
}

std::optional<std::string> validateCapacitances(
    const PendingStructuredBatch& pending,
    const NarrowTimingRecords& records)
{
  const auto& requests = pending.capturePlan().cell_pin_capacitances;
  if (records.cell_pin_capacitances.size() != requests.size())
    return "captured pin-capacitance coverage is not exact";
  std::set<timing_edit::StableNodeRef> seen;
  for (const auto& request : requests) {
    if (!seen.insert(request.ref).second)
      return "pin-capacitance request repeats StableNodeRef";
    const CapturedCellPinCapacitance* captured = nullptr;
    for (const auto& value : records.cell_pin_capacitances) {
      if (value.requested != request)
        continue;
      if (captured != nullptr)
        return "pin-capacitance evidence repeats one typed request";
      captured = &value;
    }
    const auto id = boundNodeId(pending, request);
    const auto* node = id ? nodeRecord(pending, records, *id) : nullptr;
    const auto* cell = cellModel(pending, records,
                                 request.expected_cell);
    if (captured == nullptr || !id || node == nullptr
        || captured->model.node != *id
        || node->key != request.expected_key
        || node->instance_cell != request.expected_cell)
      return "pin-capacitance evidence has a mismatched typed identity";
    csr::CellPinCapacitanceModel expected;
    expected.node = *id;
    if (cell != nullptr && cell->present) {
      const auto port = std::ranges::find(
          cell->ports, request.expected_port,
          &csr::LibraryPortModel::name);
      if (port != cell->ports.end()) {
        expected.default_capacitance = port->default_capacitance;
        expected.transition_capacitance = port->transition_capacitance;
      }
    }
    if (captured->model != expected)
      return "pin-capacitance evidence differs from its library port";
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> validateStructuredNodeModelEvidence(
    const PendingStructuredBatch& pending,
    const NarrowTimingRecords& records)
{
  if (!exactPlanSets(pending))
    return "capture plan node-model dependency set is not derivable";
  if (const auto error = validateRoles(pending, records))
    return error;
  return validateCapacitances(pending, records);
}

}  // namespace ista::timing_dual_csr::detail
