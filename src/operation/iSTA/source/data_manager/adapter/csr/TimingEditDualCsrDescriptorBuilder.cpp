// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "TimingEditDualCsrDescriptorBuilder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "DualCsrStructuredCapturePlanInternal.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NarrowCaptureAuditValidation.hpp"
#include "NarrowTimingCaptureInternal.hpp"
#include "TimingEditDualCsrCaptureEvidenceInternal.hpp"
#include "TimingEditDualCsrDescriptorStructureInternal.hpp"

namespace ista::timing_dual_csr {
namespace {
using detail::CapturedArc;
using detail::CapturedNetLoad;
using detail::CapturedNode;
using detail::NarrowTimingRecords;

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("dual csr descriptor builder: "
                              + std::move(message));
}

template <typename T, typename Predicate>
const T& requireUnique(const std::vector<T>& values, Predicate predicate,
                       std::string_view field)
{
  const T* result = nullptr;
  for (const auto& value : values) {
    if (!predicate(value))
      continue;
    if (result != nullptr)
      invalid(std::string(field) + " has duplicate records");
    result = &value;
  }
  if (result == nullptr)
    invalid(std::string(field) + " is missing a planned record");
  return *result;
}

const timing_edit::NativeSlotBinding& stagedNodeBinding(
    const timing_edit::TimingEditResolverState& resolver,
    const timing_edit::StableNodeRef& ref)
{
  const auto found = resolver.nodeBindings().find(ref);
  if (found == resolver.nodeBindings().end() || found->second.tombstone)
    invalid("captured node has no live staged resolver binding");
  return found->second;
}

const timing_edit::NativeSlotBinding& stagedArcBinding(
    const timing_edit::TimingEditResolverState& resolver,
    const timing_edit::StableArcRef& ref)
{
  const auto found = resolver.arcBindings().find(ref);
  if (found == resolver.arcBindings().end())
    invalid("captured arc has no staged resolver binding");
  return found->second;
}

void requireExactAudit(const AffectedCapturePlan& plan,
                       const detail::NativeCaptureResult& result,
                       const NarrowTimingRecords& records)
{
  if (!result.success())
    invalid("native capture failed");
  if constexpr (!incregpusta_debug_checks_enabled)
    return;
  const auto active_arcs = static_cast<std::uint64_t>(
      std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity != 0;
      }));
  const auto deactivated_arcs = static_cast<std::uint64_t>(
      std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity == 0;
      }));
  if (!result.success() || records.audit != result.audit
      || result.audit.requested_nodes != plan.nodes.size()
      || result.audit.requested_instances
             != plan.token_materializations.size()
      || result.audit.requested_arcs != active_arcs
      || result.audit.requested_deactivations != deactivated_arcs
      || result.audit.requested_load_nets != plan.load_nets.size()
      || !validateNarrowCaptureAudit(
          result.audit,
          NarrowCaptureAuditExpectation{plan.after,
                                        deactivated_arcs}))
    invalid("capture success/audit does not prove a guarded narrow read");
  std::set<std::string> cells;
  for (const auto& edit : plan.normalized_edits)
    cells.insert(edit.library_cell.cell_name);
  if (result.audit.requested_library_cells != cells.size())
    invalid("capture audit library request count is not exact");
}

void requirePlanBinding(const PendingStructuredBatch& pending)
{
  const auto& before = pending.expectedBefore();
  const auto& batch = pending.preparedBatch();
  if (batch.sealed() || before.graph().session_token != batch.sessionToken() || before.graph().generation != batch.expectedGeneration()
      || before.graph().high_water != batch.slotPlan().before || before.arcGroupIndex().session_token != batch.sessionToken()
      || before.arcGroupIndex().generation != batch.expectedGeneration() || before.arcGroupIndex().arc_high_water != batch.slotPlan().before.arc_slots
      || batch.expectedPreCommittedIdentity().generation != batch.expectedGeneration() || pending.stagedResolver().highWater() != batch.slotPlan().after)
    invalid("pending capability no longer matches its exact before-state");

  // The preparer checked the original resolver boundary and copied the exact
  // identity into PreparedBatch. Replaying that controlled construction is a
  // diagnostic proof; capture binding and record coverage remain mandatory.
  if constexpr (incregpusta_debug_checks_enabled) {
    if (before.resolver().highWater() != batch.slotPlan().before || before.identity() != batch.expectedPreCommittedIdentity()
        || before.resolver().stage(batch) != pending.stagedResolver())
      invalid("pending capability no longer matches its exact before-state");
    const DualSemanticReadView view(before.graph());
    const auto rebuilt = detail::makeStructuredCapturePlan(batch, before.resolver(), pending.stagedResolver(), before.arcGroupIndex(), view);
    if (rebuilt != pending.capturePlan())
      invalid("capture plan is not the exact deterministic batch plan");
  }
}

void requireNodeCoverage(const PendingStructuredBatch& pending,
                         const NarrowTimingRecords& records)
{
  const auto& plan = pending.capturePlan();
  const auto& staged = pending.stagedResolver();
  if (records.nodes.size() != plan.nodes.size())
    invalid("captured node coverage is not exact");
  std::set<std::uint64_t> ids;
  for (const auto& request : plan.nodes) {
    const auto& captured = requireUnique(
        records.nodes,
        [&](const auto& value) { return value.ref == request.ref; },
        "nodes");
    const auto id = stagedNodeBinding(staged, request.ref).slot.value();
    if (!ids.insert(id).second || captured.id.value() != id
        || captured.record.id != captured.id
        || captured.record.key != request.expected_key)
      invalid("captured node ref/id/key does not match staged resolver");
  }
}

void requireInstanceCoverage(const PendingStructuredBatch& pending,
                             const NarrowTimingRecords& records)
{
  const auto& batch = pending.preparedBatch();
  const auto& staged = pending.stagedResolver();
  if (records.instances.size() != batch.tokenMaterializations().size())
    invalid("captured instance coverage is not exact");
  std::set<std::string> names;
  for (const auto& materialization : batch.tokenMaterializations()) {
    const auto& captured = requireUnique(
        records.instances,
        [&](const auto& value) {
          return value.creation_token == materialization.creation_token;
        },
        "instances");
    const auto& edit = requireUnique(
        batch.normalizedEdits(),
        [&](const auto& value) {
          return value.creation_token == materialization.creation_token;
        },
        "normalized_edits");
    std::vector<std::pair<std::string, csr::NodeId>> pins{
        {materialization.input_pin_name,
         csr::NodeId{stagedNodeBinding(
                         staged,
                         timing_edit::StableNodeRef{
                             timing_edit::CreatedNodeRef{
                                 materialization.creation_token,
                                 timing_edit::CreatedNodeRole::kInput}})
                         .slot.value()}},
        {materialization.output_pin_name,
         csr::NodeId{stagedNodeBinding(
                         staged,
                         timing_edit::StableNodeRef{
                             timing_edit::CreatedNodeRef{
                                 materialization.creation_token,
                                 timing_edit::CreatedNodeRole::kOutput}})
                         .slot.value()}}};
    std::ranges::sort(pins, {}, &decltype(pins)::value_type::first);
    const std::vector<csr::NodeId> expected_pins{pins[0].second,
                                                 pins[1].second};
    if (!names.insert(captured.record.instance_name).second
        || captured.record.instance_name != materialization.instance_name
        || captured.record.cell_name != edit.library_cell.cell_name
        || captured.record.ordered_pins != expected_pins
        || captured.record.is_sequential
        || captured.record.has_timing_checks
        || captured.record.is_clock_gating
        || captured.record.is_macro)
      invalid("captured instance identity differs from materialization");
  }
}

void requireArcCoverage(const PendingStructuredBatch& pending,
                        const NarrowTimingRecords& records)
{
  const auto& plan = pending.capturePlan();
  const auto& staged = pending.stagedResolver();
  const auto& before = pending.expectedBefore().graph();
  const auto& before_index
      = pending.expectedBefore().arcGroupIndex();
  if (records.arcs.size() != plan.arcs.size())
    invalid("captured arc coverage is not exact");
  std::set<std::uint64_t> ids;
  std::vector<csr::ArcKey> active_keys;
  for (const auto& request : plan.arcs) {
    const auto& captured = requireUnique(
        records.arcs,
        [&](const auto& value) { return value.ref == request.ref; },
        "arcs");
    const auto& binding = stagedArcBinding(staged, request.ref);
    const std::uint64_t id = binding.slot.value();
    if (!ids.insert(id).second || captured.id.value() != id
        || binding.tombstone != !captured.active
        || captured.active != (request.expected_post_multiplicity != 0))
      invalid("captured arc ref/id/activity differs from staged resolver");
    if (id < plan.before.arc_slots) {
      const auto* slot = before_index.slot(captured.id);
      if (slot == nullptr || slot->ref != request.ref
          || slot->current_active_key
                 != request.expected_current_key)
        invalid("capture request differs from exact committed arc index");
    } else if (request.expected_current_key.has_value()) {
      invalid("appended captured arc claims a committed current key");
    }
    if (!captured.active) {
      if (id >= before.arc_slots.size()
          || before.arc_slots[id].payload != captured.payload
          || !request.expected_current_key.has_value()
          || captured.payload.key
                 != *request.expected_current_key
          || request.expected_current_key->base
                 != request.expected_post_base)
        invalid("deactivation did not retain exact committed-before payload");
      continue;
    }
    if (captured.payload.key.base != request.expected_post_base)
      invalid("active captured arc base differs from post request");
    if (id < before.arc_slots.size()) {
      const auto& expected = before.arc_slots[id].payload;
      const auto delay_changes = std::ranges::count(
          plan.invalidation_closure.arc_delays, request.ref);
      if (delay_changes > 1)
        invalid("arc-delay field policy repeats one StableArcRef");
      const auto captured_sink = std::ranges::find(
          records.nodes, captured.payload.sink, &CapturedNode::id);
      const csr::NodeRecord* sink = nullptr;
      if (captured_sink != records.nodes.end()) {
        sink = &captured_sink->record;
      } else if (captured.payload.sink.value() < before.nodes.size()) {
        sink = &before.nodes[captured.payload.sink.value()];
      }
      if (sink == nullptr)
        invalid("active captured arc has an unbound sink NodeId");
      const auto load = std::ranges::find(
          records.net_loads, sink->net_name,
          &CapturedNetLoad::net_name);
      const bool base_unchanged
          = expected.key.base == request.expected_post_base;
      if (captured.payload.kind != expected.kind
          || captured.payload.is_clock_arc != expected.is_clock_arc
          || captured.payload.is_disabled != expected.is_disabled
          || captured.payload.is_loop_disabled
                 != expected.is_loop_disabled
          || captured.payload.eligibility != expected.eligibility
          || (base_unchanged
              && (captured.payload.source != expected.source
                  || captured.payload.sink != expected.sink))
          || (delay_changes == 0
              && captured.payload.transfer != expected.transfer)
          || (load == records.net_loads.end()
              && captured.payload.output_load != expected.output_load)
          || (load != records.net_loads.end()
              && captured.payload.output_load
                     != std::optional<csr::ArcOutputLoadMatrix>{
                         load->output_load}))
        invalid("captured existing arc changed outside its field policy");
    }
    if (std::ranges::find(active_keys, captured.payload.key)
        != active_keys.end())
      invalid("active candidate canonical key is not unique");
    active_keys.push_back(captured.payload.key);
    std::vector<std::uint64_t> ordinals;
    for (const auto& peer : records.arcs)
      if (peer.active
          && peer.payload.key.base == captured.payload.key.base)
        ordinals.push_back(peer.payload.key.parallel_ordinal);
    std::ranges::sort(ordinals);
    if (ordinals.size() != request.expected_post_multiplicity)
      invalid("captured ArcKeyBase multiplicity differs from plan");
    for (std::size_t index = 0; index < ordinals.size(); ++index)
      if (ordinals[index] != index)
        invalid("captured ArcKeyBase ordinals are not continuous");
  }
}

void requireLibraryRef(
    const timing_edit::LibraryCellRef& requested,
    const csr::ReferencedLibraryCellModel& model)
{
  const auto portExists = [&](const std::string& name) {
    return std::ranges::any_of(model.ports, [&](const auto& port) {
      return port.name == name;
    });
  };
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
  if (!model.present || model.cell_name != requested.cell_name
      || model.library_name != requested.library_name
      || !portExists(requested.input_port)
      || !portExists(requested.output_port) || !arc_matches
      || !model.timing_checks.empty() || model.is_sequential
      || model.is_clock_gating || model.is_macro)
    invalid("captured library model violates its semantic request");
}

void requireNetAndLibraryCoverage(const PendingStructuredBatch& pending,
                                  const NarrowTimingRecords& records)
{
  const auto& plan = pending.capturePlan();
  if (records.net_loads.size() != plan.load_nets.size())
    invalid("captured net-load coverage is not exact");
  std::set<std::string> names;
  for (const auto& request : plan.load_nets) {
    const auto& captured = requireUnique(
        records.net_loads,
        [&](const auto& value) { return value.ref == request.ref; },
        "net_loads");
    if (!names.insert(captured.net_name).second
        || captured.net_name != request.expected_name)
      invalid("captured net-load identity differs from plan");
  }

  std::map<std::string, timing_edit::LibraryCellRef> requested_cells;
  for (const auto& edit : plan.normalized_edits) {
    const auto [found, inserted]
        = requested_cells.emplace(edit.library_cell.cell_name,
                                  edit.library_cell);
    if (!inserted && found->second != edit.library_cell)
      invalid("one library cell name has conflicting semantic refs");
  }
  if (records.library_cells.size() != requested_cells.size())
    invalid("captured library-cell coverage is not exact");
  for (const auto& [name, requested] : requested_cells) {
    const auto& captured = requireUnique(
        records.library_cells,
        [&](const auto& value) {
          return value.model.cell_name == name;
        },
        "library_cells");
    if (captured.requested != requested
        || captured.model.cell_name != name) {
      invalid("captured library-cell identity differs from request");
    }
    requireLibraryRef(requested, captured.model);
  }
  std::set<std::uint64_t> cap_nodes;
  for (const auto& captured : records.cell_pin_capacitances)
    if (!cap_nodes.insert(captured.model.node.value()).second)
      invalid("captured pin capacitance repeats one NodeId");
}

template <typename T>
void requireDelta(const std::optional<ValueReplacement<T>>& actual,
                  const T& expected, const T& replacement,
                  std::string_view field)
{
  if (expected == replacement) {
    if (actual)
      invalid(std::string(field) + " changes an unmodified field");
    return;
  }
  if (!actual || actual->expected != expected
      || actual->replacement != replacement)
    invalid(std::string(field) + " is not the exact typed replacement");
}

void requireForbiddenReferenceFieldsEmpty(
    const DualCsrReferencePayloadDelta& delta)
{
  if (delta.schema_version || delta.source_identity
      || delta.analysis_context || delta.analysis_order
      || delta.path_source_order || delta.transition_order
      || delta.ordered_root_ids || delta.ordered_start_ids
      || delta.ordered_end_ids || delta.clock_tags || delta.clocks
      || delta.port_constraints || delta.case_analysis_constraints
      || delta.timing_check_endpoints || delta.parasitics
      || delta.start_seeds || delta.endpoint_required
      || delta.unsupported_reasons)
    invalid("capture attempted to replace an unrequested reference field");
}

void requireReferenceDelta(const PendingStructuredBatch& pending,
                           const NarrowTimingRecords& records)
{
  const auto& current = pending.expectedBefore().graph().reference_payload;
  const auto& delta = records.reference_payload;
  requireForbiddenReferenceFieldsEmpty(delta);

  auto instances = current.instances;
  for (const auto& captured : records.instances) {
    if (std::ranges::any_of(instances, [&](const auto& value) {
          return value.instance_name == captured.record.instance_name;
        }))
      invalid("captured instance already exists in committed reference");
    instances.push_back(captured.record);
  }
  std::ranges::sort(instances, [](const auto& lhs, const auto& rhs) {
    return csr::detail::referenceByteStringLess(lhs.instance_name,
                                                rhs.instance_name);
  });
  requireDelta(delta.instances, current.instances, instances,
               "reference.instances");

  auto cells = current.referenced_library_cells;
  for (const auto& captured : records.library_cells) {
    const auto found = std::ranges::find_if(cells, [&](const auto& value) {
      return value.cell_name == captured.model.cell_name;
    });
    if (found == cells.end())
      cells.push_back(captured.model);
    else if (*found != captured.model)
      invalid("captured library model changes a committed cell");
  }
  std::ranges::sort(cells, [](const auto& lhs, const auto& rhs) {
    return csr::detail::referenceByteStringLess(lhs.cell_name,
                                                rhs.cell_name);
  });
  requireDelta(delta.referenced_library_cells,
               current.referenced_library_cells, cells,
               "reference.referenced_library_cells");

  const auto keyForId = [&](csr::NodeId id) -> const csr::NodeKey& {
    const auto captured = std::ranges::find(
        records.nodes, id, &CapturedNode::id);
    if (captured != records.nodes.end())
      return captured->record.key;
    const auto& graph = pending.expectedBefore().graph();
    if (id.value() >= graph.nodes.size())
      invalid("capacitance refers to an uncaptured appended node");
    return graph.nodes[id.value()].key;
  };
  auto caps = current.cell_pin_capacitances;
  for (const auto& captured : records.cell_pin_capacitances) {
    if (captured.requested.expected_key
        != keyForId(captured.model.node))
      invalid("captured capacitance key differs from its logical NodeId");
    const auto found = std::ranges::find(
        caps, captured.model.node, &csr::CellPinCapacitanceModel::node);
    if (found == caps.end())
      caps.push_back(captured.model);
    else if (*found != captured.model)
      invalid("captured capacitance changes a committed NodeId");
  }
  std::ranges::sort(caps, [&](const auto& lhs, const auto& rhs) {
    return csr::detail::referenceByteStringLess(
        keyForId(lhs.node).full_name,
        keyForId(rhs.node).full_name);
  });
  requireDelta(delta.cell_pin_capacitances,
               current.cell_pin_capacitances, caps,
               "reference.cell_pin_capacitances");

  auto owners = current.clock_owner_by_node;
  owners.resize(pending.preparedBatch().slotPlan().after.node_slots,
                std::nullopt);
  requireDelta(delta.clock_owner_by_node,
               current.clock_owner_by_node, owners,
               "reference.clock_owner_by_node");
}

DualCsrBatchDescriptor basicDescriptor(
    const PendingStructuredBatch& pending,
    const NarrowTimingRecords& records)
{
  const auto& batch = pending.preparedBatch();
  const auto& before = pending.expectedBefore().graph();
  DualCsrBatchDescriptor descriptor;
  descriptor.session_token = batch.sessionToken();
  descriptor.expected_generation = batch.expectedGeneration();
  descriptor.target_generation = batch.targetGeneration();
  descriptor.before = batch.slotPlan().before;
  descriptor.after = batch.slotPlan().after;

  for (const auto& captured : records.nodes) {
    if (captured.id.value() >= descriptor.before.node_slots) {
      descriptor.node_appends.push_back(
          {captured.id, captured.record.key, captured.record});
      continue;
    }
    const auto& expected = before.nodes[captured.id.value()];
    auto only_net_changed = expected;
    only_net_changed.net_name = captured.record.net_name;
    if (only_net_changed != captured.record)
      invalid("existing node changed outside the reconnect net field");
    if (expected.net_name != captured.record.net_name)
      descriptor.node_payload_changes.push_back(
          {captured.id, expected.key, expected.net_name,
           captured.record.net_name});
  }
  std::ranges::sort(descriptor.node_appends,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.id.value() < rhs.id.value();
                    });
  std::ranges::sort(descriptor.node_payload_changes,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.id.value() < rhs.id.value();
                    });

  for (const auto& captured : records.arcs) {
    if (!captured.active) {
      descriptor.logical_deactivations.push_back(
          {captured.id, captured.payload.key, captured.payload.source,
           captured.payload.sink, LogicalArcDeactivateReason::kRemoved,
           true});
      continue;
    }
    if (captured.id.value() >= descriptor.before.arc_slots) {
      descriptor.arc_appends.push_back(
          {captured.id, captured.payload.key, captured.payload.source,
           captured.payload.sink, captured.payload});
      continue;
    }
    const auto& expected = before.arc_slots[captured.id.value()];
    if (!expected.active)
      invalid("capture attempts to reactivate a logical ArcId");
    if (expected.payload != captured.payload)
      descriptor.payload_changes.push_back(
          {captured.id, expected.payload, captured.payload});
  }
  for (const auto& captured : records.arcs) {
    const auto& request = requireUnique(
        pending.capturePlan().arcs,
        [&](const auto& value) { return value.ref == captured.ref; },
        "arc_index_requests");
    const std::optional<csr::ArcKey> replacement
        = captured.active
              ? std::optional<csr::ArcKey>{captured.payload.key}
              : std::nullopt;
    if (request.expected_current_key != replacement)
      descriptor.arc_group_index_delta.push_back(
          {captured.id, captured.ref, request.expected_current_key,
           replacement});
  }
  std::ranges::sort(descriptor.arc_appends,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.id.value() < rhs.id.value();
                    });
  std::ranges::sort(descriptor.payload_changes,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.id.value() < rhs.id.value();
                    });
  std::ranges::sort(descriptor.logical_deactivations,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.id.value() < rhs.id.value();
                    });
  std::ranges::sort(descriptor.arc_group_index_delta,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.id.value() < rhs.id.value();
                    });

  descriptor.reference_payload = records.reference_payload;
  auto identity = before.reference_payload.source_identity;
  identity.session_token = descriptor.session_token.value();
  identity.graph_generation = descriptor.target_generation.value();
  descriptor.reference_payload.source_identity
      = ValueReplacement<csr::CsrSnapshotIdentity>{
          before.reference_payload.source_identity,
          std::move(identity)};
  return descriptor;
}

void requireCapturedLoadsApplied(const PendingStructuredBatch& pending,
                                 const NarrowTimingRecords& records)
{
  const auto& graph = pending.expectedBefore().graph();
  const auto nodeFor = [&](csr::NodeId id) -> const csr::NodeRecord& {
    const auto captured = std::ranges::find(records.nodes, id,
                                            &CapturedNode::id);
    if (captured != records.nodes.end())
      return captured->record;
    if (id.value() >= graph.nodes.size())
      invalid("captured arc sink is an uncaptured appended node");
    return graph.nodes[id.value()];
  };
  for (const auto& arc : records.arcs) {
    if (!arc.active)
      continue;
    const auto& sink = nodeFor(arc.payload.sink);
    const auto load = std::ranges::find(
        records.net_loads, sink.net_name, &CapturedNetLoad::net_name);
    if (load != records.net_loads.end()
        && arc.payload.output_load
               != std::optional<csr::ArcOutputLoadMatrix>{
                   load->output_load})
      invalid("captured arc did not apply its invalidated net load");
  }
}

}  // namespace

VerifiedStructuredBatch TimingEditDualCsrDescriptorBuilder::build(
    PendingStructuredBatch pending,
    CandidateBoundCapture&& capture)
{
  requirePlanBinding(pending);
  auto storage = std::move(capture._storage);
  if (storage == nullptr
      || storage->bound_plan != pending.capturePlan())
    invalid("candidate capture is moved-from or bound to another plan");
  auto capture_result = std::move(storage->result);
  if (!capture_result.records)
    invalid("capture result has no typed records");
  detail::NarrowTimingRecords records
      = std::move(*capture_result.records);
  requireExactAudit(pending.capturePlan(), capture_result, records);
  requireNodeCoverage(pending, records);
  requireInstanceCoverage(pending, records);
  requireArcCoverage(pending, records);
  requireNetAndLibraryCoverage(pending, records);
  if (const auto error = detail::validateStructuredNodeModelEvidence(
          pending, records))
    invalid(*error);
  requireReferenceDelta(pending, records);
  requireCapturedLoadsApplied(pending, records);

  auto descriptor = basicDescriptor(pending, records);
  detail::buildStructuredPhysicalDelta(pending, records, descriptor);
  const auto violations = validateDualCsrDescriptor(
      pending.expectedBefore().graph(), descriptor);
  if (!violations.empty())
    invalid(violations.front().field + ": "
            + violations.front().message);
  const auto index_violations = validateDualCsrArcGroupIndexDelta(
      pending.expectedBefore().graph(),
      pending.expectedBefore().arcGroupIndex(), descriptor);
  if (!index_violations.empty())
    invalid(index_violations.front().field + ": "
            + index_violations.front().message);

  return VerifiedStructuredBatch{
      std::move(pending._prepared_batch),
      std::move(pending._staged_resolver), std::move(descriptor),
      std::move(pending._expected_before), std::move(records.audit)};
}

}  // namespace ista::timing_dual_csr
