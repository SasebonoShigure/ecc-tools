// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrDescriptorValidation.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "DualCsrValidationInternal.hpp"
#include "TimingSemanticKey.hpp"
namespace ista::timing_dual_csr {
namespace {
using detail::reject;
using PhysicalKey = std::pair<std::uint64_t, std::uint64_t>;
using PhysicalRows
    = std::map<std::uint64_t, std::vector<csr::ArcId>>;
bool appended(csr::ArcId id, const DualCsrBatchDescriptor& descriptor) noexcept
{
  return id.value() >= descriptor.before.arc_slots
         && id.value() < descriptor.after.arc_slots;
}
const csr::ArcSemanticPayload* payloadFor(
    csr::ArcId id, const DualCsrState& committed,
    const DualCsrBatchDescriptor& descriptor) noexcept
{
  if (id.value() < committed.arc_slots.size())
    return &committed.arc_slots[id.value()].payload;
  const auto offset = id.value() - descriptor.before.arc_slots;
  if (offset < descriptor.arc_appends.size())
    return &descriptor.arc_appends[offset].payload;
  return nullptr;
}
std::vector<csr::ArcId> auxRow(const DualCsrState& state,
                               csr::NodeId source)
{
  if (source.value() >= state.aux_out.source_to_aux_row.size())
    return {};
  const auto mapped = state.aux_out.source_to_aux_row[source.value()];
  if (mapped < 0)
    return {};
  const auto row = static_cast<std::size_t>(mapped);
  if (row + 1 >= state.aux_out.row_offsets.size())
    return {};
  const auto begin = state.aux_out.row_offsets[row];
  const auto end = state.aux_out.row_offsets[row + 1];
  if (begin > end || end > state.aux_out.arc_ids.size())
    return {};
  return {state.aux_out.arc_ids.begin() + static_cast<std::ptrdiff_t>(begin),
          state.aux_out.arc_ids.begin() + static_cast<std::ptrdiff_t>(end)};
}
std::vector<csr::ArcId> committedOutgoingRow(
    const DualCsrState& state, csr::NodeId source)
{
  std::vector<csr::ArcId> result;
  if (source.value() + 1 < state.base_out.row_offsets.size()) {
    const auto begin = state.base_out.row_offsets[source.value()];
    const auto end = state.base_out.row_offsets[source.value() + 1];
    if (begin <= end && end <= state.base_out.physical_slots.size())
      for (auto slot = begin; slot < end; ++slot)
        if (state.base_out.physical_slots[slot])
          result.push_back(*state.base_out.physical_slots[slot]);
  }
  auto aux = auxRow(state, source);
  result.insert(result.end(), aux.begin(), aux.end());
  return result;
}
std::vector<csr::ArcId> committedIncomingRow(
    const DualCsrState& state, csr::NodeId sink)
{
  std::vector<csr::ArcId> result;
  if (sink.value() + 1 >= state.base_in.row_offsets.size())
    return result;
  const auto begin = state.base_in.row_offsets[sink.value()];
  const auto end = state.base_in.row_offsets[sink.value() + 1];
  if (begin > end || end > state.base_in.physical_slots.size())
    return result;
  for (auto slot = begin; slot < end; ++slot)
    if (state.base_in.physical_slots[slot])
      result.push_back(*state.base_in.physical_slots[slot]);
  return result;
}
std::set<std::uint64_t> postPhysicalOutgoingIds(
    const DualCsrState& committed,
    const DualCsrBatchDescriptor& descriptor,
    DualCsrViolations& violations, PhysicalRows& rows)
{
  auto base = committed.base_out.physical_slots;
  for (const auto& remove : descriptor.removes)
    if (remove.physical_slot < base.size())
      base[remove.physical_slot].reset();
  for (const auto& change : descriptor.changes)
    if (change.physical_slot < base.size())
      base[change.physical_slot] = change.replacement_arc;
  std::map<std::uint64_t, std::vector<csr::ArcId>> aux;
  for (const auto source : committed.aux_out.row_sources)
    aux[source.value()] = auxRow(committed, source);
  for (const auto& row : descriptor.aux_rows)
    if (row.replacement_arc_ids.empty())
      aux.erase(row.source.value());
    else
      aux[row.source.value()] = row.replacement_arc_ids;
  std::set<std::uint64_t> ids;
  const auto add = [&](csr::NodeId source, csr::ArcId id,
                       const std::string& field) {
    const auto* payload = payloadFor(id, committed, descriptor);
    if (!ids.insert(id.value()).second || payload == nullptr
        || payload->source != source)
      reject(violations, DualCsrViolationCode::kAdjacency, field,
             "post BaseOut/Aux must contain each source ArcId once");
    rows[source.value()].push_back(id);
  };
  if (committed.base_out.row_offsets.size()
      == committed.nodes.size() + 1) {
    for (std::size_t source = 0; source < committed.nodes.size(); ++source) {
      const auto begin = committed.base_out.row_offsets[source];
      const auto end = committed.base_out.row_offsets[source + 1];
      if (begin > end || end > base.size())
        continue;
      for (std::uint64_t slot = begin; slot < end; ++slot)
        if (base[slot])
          add(csr::NodeId{source}, *base[slot], "changes");
    }
  }
  for (const auto& row : descriptor.appended_base_out_rows)
    for (const auto id : row.arc_ids)
      add(row.source, id, "appended_base_out_rows");
  for (const auto& [source, row] : aux)
    for (const auto id : row)
      add(csr::NodeId{source}, id, "aux_rows");
  return ids;
}
std::set<std::uint64_t> postPhysicalIncomingIds(
    const DualCsrState& committed,
    const DualCsrBatchDescriptor& descriptor,
    DualCsrViolations& violations, PhysicalRows& rows)
{
  auto base = committed.base_in.physical_slots;
  for (const auto& change : descriptor.reverse_changes)
    if (change.physical_slot < base.size())
      base[change.physical_slot] = change.replacement_arc;
  std::set<std::uint64_t> ids;
  const auto add = [&](csr::NodeId sink, csr::ArcId id,
                       const std::string& field) {
    const auto* payload = payloadFor(id, committed, descriptor);
    if (!ids.insert(id.value()).second || payload == nullptr
        || payload->sink != sink)
      reject(violations, DualCsrViolationCode::kTranspose, field,
             "post BaseIn must contain each sink ArcId once");
    rows[sink.value()].push_back(id);
  };
  if (committed.base_in.row_offsets.size()
      == committed.nodes.size() + 1) {
    for (std::size_t sink = 0; sink < committed.nodes.size(); ++sink) {
      const auto begin = committed.base_in.row_offsets[sink];
      const auto end = committed.base_in.row_offsets[sink + 1];
      if (begin > end || end > base.size())
        continue;
      for (auto slot = begin; slot < end; ++slot)
        if (base[slot])
          add(csr::NodeId{sink}, *base[slot], "reverse_changes");
    }
  }
  for (const auto& row : descriptor.affected_base_in_rows)
    if (row.sink.value() >= descriptor.before.node_slots)
      for (const auto id : row.replacement_arc_ids)
        add(row.sink, id, "affected_base_in_rows");
  return ids;
}
template <typename T>
void validateExpected(const std::optional<ValueReplacement<T>>& change,
                      const T& current, const std::string& field,
                      DualCsrViolations& violations)
{
  if (change && !(change->expected == current))
    reject(violations, DualCsrViolationCode::kExpectedValue, field,
           "expected-before value does not match committed payload");
}
void validateReferenceDelta(const DualCsrState& committed,
                            const DualCsrBatchDescriptor& descriptor,
                            DualCsrViolations& violations)
{
  const auto& delta = descriptor.reference_payload;
  const auto& current = committed.reference_payload;
  validateExpected(delta.schema_version, current.schema_version,
                   "reference_payload.schema_version", violations);
  validateExpected(delta.source_identity, current.source_identity,
                   "reference_payload.source_identity", violations);
  validateExpected(delta.analysis_context, current.analysis_context,
                   "reference_payload.analysis_context", violations);
  validateExpected(delta.analysis_order, current.analysis_order,
                   "reference_payload.analysis_order", violations);
  validateExpected(delta.path_source_order, current.path_source_order,
                   "reference_payload.path_source_order", violations);
  validateExpected(delta.transition_order, current.transition_order,
                   "reference_payload.transition_order", violations);
  validateExpected(delta.instances, current.instances,
                   "reference_payload.instances", violations);
  validateExpected(delta.ordered_root_ids, current.ordered_root_ids,
                   "reference_payload.ordered_root_ids", violations);
  validateExpected(delta.ordered_start_ids, current.ordered_start_ids,
                   "reference_payload.ordered_start_ids", violations);
  validateExpected(delta.ordered_end_ids, current.ordered_end_ids,
                   "reference_payload.ordered_end_ids", violations);
  validateExpected(delta.clock_tags, current.clock_tags,
                   "reference_payload.clock_tags", violations);
  validateExpected(delta.clocks, current.clocks,
                   "reference_payload.clocks", violations);
  validateExpected(delta.clock_owner_by_node, current.clock_owner_by_node,
                   "reference_payload.clock_owner_by_node", violations);
  validateExpected(delta.port_constraints, current.port_constraints,
                   "reference_payload.port_constraints", violations);
  validateExpected(delta.case_analysis_constraints,
                   current.case_analysis_constraints,
                   "reference_payload.case_analysis_constraints", violations);
  validateExpected(delta.timing_check_endpoints,
                   current.timing_check_endpoints,
                   "reference_payload.timing_check_endpoints", violations);
  validateExpected(delta.referenced_library_cells,
                   current.referenced_library_cells,
                   "reference_payload.referenced_library_cells", violations);
  validateExpected(delta.parasitics, current.parasitics,
                   "reference_payload.parasitics", violations);
  validateExpected(delta.start_seeds, current.start_seeds,
                   "reference_payload.start_seeds", violations);
  validateExpected(delta.endpoint_required, current.endpoint_required,
                   "reference_payload.endpoint_required", violations);
  validateExpected(delta.cell_pin_capacitances,
                   current.cell_pin_capacitances,
                   "reference_payload.cell_pin_capacitances", violations);
  validateExpected(delta.unsupported_reasons, current.unsupported_reasons,
                   "reference_payload.unsupported_reasons", violations);
  if (!delta.source_identity) {
    reject(violations, DualCsrViolationCode::kGeneration,
           "reference_payload.source_identity",
           "every batch must declare the expected and replacement identity");
  } else {
    const auto& replacement = delta.source_identity->replacement;
    if (replacement.session_token != descriptor.session_token.value()
        || replacement.graph_generation
               != descriptor.target_generation.value())
      reject(violations, DualCsrViolationCode::kGeneration,
             "reference_payload.source_identity.replacement",
             "replacement identity must bind target session/generation");
  }
}

void validateAppends(const DualCsrState& committed,
                     const DualCsrBatchDescriptor& descriptor,
                     DualCsrViolations& violations)
{
  const auto expected_nodes
      = descriptor.after.node_slots - descriptor.before.node_slots;
  const auto expected_arcs
      = descriptor.after.arc_slots - descriptor.before.arc_slots;
  if (descriptor.node_appends.size() != expected_nodes)
    reject(violations, DualCsrViolationCode::kHighWater, "node_appends",
           "appends must exactly cover the node high-water range");
  if (descriptor.arc_appends.size() != expected_arcs)
    reject(violations, DualCsrViolationCode::kHighWater, "arc_appends",
           "appends must exactly cover the arc high-water range");

  for (std::size_t index = 0; index < descriptor.node_appends.size(); ++index) {
    const auto& append = descriptor.node_appends[index];
    const auto expected = descriptor.before.node_slots + index;
    if (append.id.value() != expected || append.payload.id != append.id
        || !(append.payload.key == append.key))
      reject(violations, DualCsrViolationCode::kId, "node_appends",
             "node append ID/key/payload must agree with contiguous slot");
  }

  const auto node_key = [&](csr::NodeId id) -> const csr::NodeKey* {
    if (id.value() < committed.nodes.size())
      return &committed.nodes[id.value()].key;
    const auto offset = id.value() - descriptor.before.node_slots;
    if (offset < descriptor.node_appends.size())
      return &descriptor.node_appends[offset].key;
    return nullptr;
  };
  for (std::size_t index = 0; index < descriptor.arc_appends.size(); ++index) {
    const auto& append = descriptor.arc_appends[index];
    const auto expected = descriptor.before.arc_slots + index;
    const auto* source_key = node_key(append.source);
    const auto* sink_key = node_key(append.sink);
    if (append.id.value() != expected || append.payload.key != append.key
        || append.payload.source != append.source
        || append.payload.sink != append.sink
        || append.payload.kind != append.key.base.kind || source_key == nullptr
        || sink_key == nullptr || append.key.base.source != *source_key
        || append.key.base.sink != *sink_key)
      reject(violations, DualCsrViolationCode::kId, "arc_appends",
             "arc append must match contiguous slot, endpoints, and keys");
  }

  if (descriptor.appended_base_out_rows.size()
      != descriptor.node_appends.size())
    reject(violations, DualCsrViolationCode::kAdjacency,
           "appended_base_out_rows",
           "every appended node must declare one candidate BaseOut row");
  std::set<std::uint64_t> row_arcs;
  for (std::size_t index = 0;
       index < descriptor.appended_base_out_rows.size(); ++index) {
    const auto& row = descriptor.appended_base_out_rows[index];
    const csr::NodeId expected{
        descriptor.before.node_slots + index};
    if (row.source != expected)
      reject(violations, DualCsrViolationCode::kId,
             "appended_base_out_rows",
             "new-node rows must be complete and ordered by NodeId");
    std::set<std::uint64_t> local;
    for (const auto id : row.arc_ids) {
      const auto* payload = payloadFor(id, committed, descriptor);
      if (!appended(id, descriptor)
          || !local.insert(id.value()).second
          || !row_arcs.insert(id.value()).second
          || payload == nullptr || payload->source != row.source)
        reject(violations, DualCsrViolationCode::kAdjacency,
               "appended_base_out_rows",
               "new-node row must contain unique appended source arcs");
    }
  }
  for (const auto& append : descriptor.arc_appends) {
    if (append.source.value() >= descriptor.before.node_slots
        && !row_arcs.contains(append.id.value()))
      reject(violations, DualCsrViolationCode::kAdjacency,
             "appended_base_out_rows",
             "new-source arc is absent from its candidate row");
  }
}

void validatePostActiveArcKeys(
    const DualCsrState& committed,
    const DualCsrBatchDescriptor& descriptor,
    const std::set<std::uint64_t>& deactivated,
    DualCsrViolations& violations)
{
  std::vector<const csr::ArcSemanticPayload*> active;
  active.reserve(committed.arc_slots.size()
                 + descriptor.arc_appends.size());
  for (std::size_t id = 0; id < committed.arc_slots.size(); ++id) {
    const auto& slot = committed.arc_slots[id];
    if (!slot.active || deactivated.contains(id))
      continue;
    const auto change = std::ranges::find(
        descriptor.payload_changes,
        csr::ArcId{static_cast<std::uint64_t>(id)},
        &ArcPayloadChangeDescriptor::id);
    active.push_back(change == descriptor.payload_changes.end()
                         ? &slot.payload
                         : &change->replacement);
  }
  for (const auto& append : descriptor.arc_appends)
    active.push_back(&append.payload);

  bool bases_valid = true;
  for (const auto* payload : active)
    if (const auto error = csr::validateArcKeyBase(payload->key.base)) {
      bases_valid = false;
      reject(violations, DualCsrViolationCode::kKey,
             "post_active_arcs.key", *error);
    }
  if (!bases_valid)
    return;
  std::ranges::sort(
      active, [](const csr::ArcSemanticPayload* lhs,
                 const csr::ArcSemanticPayload* rhs) {
        return csr::arcKeyLess(lhs->key, rhs->key);
      });
  std::uint64_t expected_ordinal = 0;
  for (std::size_t index = 0; index < active.size(); ++index) {
    if (index == 0 || active[index - 1]->key.base != active[index]->key.base)
      expected_ordinal = 0;
    if (active[index]->key.parallel_ordinal != expected_ordinal)
      reject(violations, DualCsrViolationCode::kKey,
             "post_active_arcs.key.parallel_ordinal",
             "post active parallel ordinals must be continuous from zero");
    ++expected_ordinal;
  }
}
}  // namespace
DualCsrViolations validateDualCsrDescriptor(
    const DualCsrState& committed, const DualCsrBatchDescriptor& descriptor)
{
  DualCsrViolations violations;
  if (descriptor.schema != kDualCsrDescriptorSchema)
    reject(violations, DualCsrViolationCode::kSchema, "schema",
           "unsupported descriptor schema");
  if (descriptor.session_token != committed.session_token)
    reject(violations, DualCsrViolationCode::kSession, "session_token",
           "descriptor belongs to a different session");
  if (descriptor.expected_generation != committed.generation
      || descriptor.expected_generation.value()
             == std::numeric_limits<std::uint64_t>::max()
      || descriptor.target_generation.value()
             != descriptor.expected_generation.value() + 1)
    reject(violations, DualCsrViolationCode::kGeneration, "generation",
           "descriptor must advance committed generation by one");
  if (!(descriptor.before == committed.high_water))
    reject(violations, DualCsrViolationCode::kHighWater, "before",
           "descriptor before high-water does not match committed state");
  if (descriptor.after.node_slots < descriptor.before.node_slots
      || descriptor.after.net_slots < descriptor.before.net_slots
      || descriptor.after.arc_slots < descriptor.before.arc_slots)
    reject(violations, DualCsrViolationCode::kHighWater, "after",
           "append-only high-water cannot move backwards");
  validateAppends(committed, descriptor, violations);
  validateReferenceDelta(committed, descriptor, violations);
  std::set<PhysicalKey> out_writes;
  std::set<PhysicalKey> in_writes;
  std::set<std::uint64_t> required_deactivations;
  const auto record_out = [&](csr::NodeId row, std::uint64_t slot,
                              const std::string& field) {
    if (!out_writes.emplace(row.value(), slot).second)
      reject(violations, DualCsrViolationCode::kConflict, field,
             "BaseOut physical slot is written more than once");
  };
  for (const auto& remove : descriptor.removes) {
    record_out(remove.source, remove.physical_slot, "removes");
    if (!detail::rowContainsSlot(committed.base_out.row_offsets,
                                 remove.source, remove.physical_slot)
        || remove.physical_slot
               >= committed.base_out.physical_slots.size()
        || committed.base_out.physical_slots[remove.physical_slot]
               != std::optional<csr::ArcId>{remove.expected_arc})
      reject(violations, DualCsrViolationCode::kExpectedValue, "removes",
             "BaseOut remove does not match committed row/slot");
    required_deactivations.insert(remove.expected_arc.value());
  }
  for (const auto& change : descriptor.changes) {
    record_out(change.source, change.physical_slot, "changes");
    const auto* replacement = payloadFor(
        change.replacement_arc, committed, descriptor);
    const bool replacement_active
        = appended(change.replacement_arc, descriptor)
          || (change.replacement_arc.value()
                    < committed.arc_slots.size()
              && committed.arc_slots[change.replacement_arc.value()]
                     .active);
    const bool replacement_deactivated = std::ranges::any_of(
        descriptor.logical_deactivations, [&](const auto& item) {
          return item.id == change.replacement_arc;
        });
    if (!detail::rowContainsSlot(committed.base_out.row_offsets,
                                 change.source, change.physical_slot)
        || change.physical_slot
               >= committed.base_out.physical_slots.size()
        || committed.base_out.physical_slots[change.physical_slot]
               != change.expected_arc
        || replacement == nullptr || !replacement_active
        || replacement_deactivated
        || replacement->source != change.source)
      reject(violations, DualCsrViolationCode::kExpectedValue, "changes",
             "BaseOut change does not match slot or active replacement");
    if (change.expected_arc
        && change.expected_arc != change.replacement_arc)
      required_deactivations.insert(change.expected_arc->value());
  }
  for (const auto& change : descriptor.reverse_changes) {
    if (!in_writes.emplace(change.sink.value(), change.physical_slot).second)
      reject(violations, DualCsrViolationCode::kConflict,
             "reverse_changes",
             "BaseIn physical slot is written more than once");
    const auto* replacement = payloadFor(
        change.replacement_arc, committed, descriptor);
    const bool replacement_active
        = appended(change.replacement_arc, descriptor)
          || (change.replacement_arc.value()
                    < committed.arc_slots.size()
              && committed.arc_slots[change.replacement_arc.value()]
                     .active);
    const bool replacement_deactivated = std::ranges::any_of(
        descriptor.logical_deactivations, [&](const auto& item) {
          return item.id == change.replacement_arc;
        });
    if (!detail::rowContainsSlot(committed.base_in.row_offsets, change.sink,
                                 change.physical_slot)
        || change.physical_slot >= committed.base_in.physical_slots.size()
        || committed.base_in.physical_slots[change.physical_slot]
               != std::optional<csr::ArcId>{change.expected_arc}
        || replacement == nullptr || !replacement_active
        || replacement_deactivated || replacement->sink != change.sink)
      reject(violations, DualCsrViolationCode::kExpectedValue,
             "reverse_changes",
             "BaseIn change does not match slot or active replacement");
    if (change.expected_arc != change.replacement_arc)
      required_deactivations.insert(change.expected_arc.value());
  }
  std::set<std::uint64_t> aux_sources;
  for (const auto& row : descriptor.aux_rows) {
    if (!aux_sources.insert(row.source.value()).second)
      reject(violations, DualCsrViolationCode::kConflict, "aux_rows",
             "source row appears more than once");
    if (row.source.value() >= descriptor.after.node_slots
        || (row.source.value() < descriptor.before.node_slots
            && auxRow(committed, row.source) != row.expected_arc_ids)
        || (row.source.value() >= descriptor.before.node_slots
            && !row.expected_arc_ids.empty()))
      reject(violations, DualCsrViolationCode::kExpectedValue, "aux_rows",
             "Aux expected row does not match committed state");
    std::set<std::uint64_t> replacement_ids;
    for (const auto arc : row.replacement_arc_ids) {
      const auto* payload = payloadFor(arc, committed, descriptor);
      if (!replacement_ids.insert(arc.value()).second || payload == nullptr
          || payload->source != row.source)
        reject(violations, DualCsrViolationCode::kAdjacency, "aux_rows",
               "Aux replacement must contain unique arcs from its source");
    }
    for (const auto old_arc : row.expected_arc_ids) {
      if (std::ranges::find(row.replacement_arc_ids, old_arc)
          == row.replacement_arc_ids.end())
        required_deactivations.insert(old_arc.value());
    }
  }
  std::set<std::uint64_t> deactivated;
  for (const auto& item : descriptor.logical_deactivations) {
    if (item.reason != LogicalArcDeactivateReason::kRemoved
        && item.reason != LogicalArcDeactivateReason::kReplaced
        && item.reason != LogicalArcDeactivateReason::kAuxCleared) {
      reject(violations, DualCsrViolationCode::kSchema,
             "logical_deactivations.reason",
             "deactivation reason is outside the declared enum domain");
      continue;
    }
    if (!deactivated.insert(item.id.value()).second
        || item.id.value() >= committed.arc_slots.size()) {
      reject(violations, DualCsrViolationCode::kId,
             "logical_deactivations",
             "deactivation ArcId must be unique and pre-existing");
      continue;
    }
    const auto& slot = committed.arc_slots[item.id.value()];
    if (slot.active != item.expected_active || !item.expected_active
        || slot.payload.key != item.key
        || slot.payload.source != item.source
        || slot.payload.sink != item.sink)
      reject(violations, DualCsrViolationCode::kExpectedValue,
             "logical_deactivations",
             "deactivation does not match active committed arc");
  }
  PhysicalRows post_outgoing_rows;
  const auto post_physical = postPhysicalOutgoingIds(
      committed, descriptor, violations, post_outgoing_rows);
  std::set<std::uint64_t> expected_post_active;
  for (std::size_t id = 0; id < committed.arc_slots.size(); ++id)
    if (committed.arc_slots[id].active
        && !deactivated.contains(id))
      expected_post_active.insert(id);
  for (const auto& append : descriptor.arc_appends)
    expected_post_active.insert(append.id.value());
  if (post_physical != expected_post_active)
    reject(violations, DualCsrViolationCode::kAdjacency,
           "post_outgoing_rows",
           "BaseOut union Aux must exactly cover post-active ArcIds");
  PhysicalRows post_incoming_rows;
  const auto post_incoming = postPhysicalIncomingIds(
      committed, descriptor, violations, post_incoming_rows);
  if (post_incoming != expected_post_active)
    reject(violations, DualCsrViolationCode::kTranspose,
           "post_incoming_rows",
           "BaseIn must exactly cover post-active ArcIds");
  std::set<std::uint64_t> affected_outgoing;
  std::optional<std::uint64_t> last_outgoing;
  for (const auto& row : descriptor.affected_base_out_rows) {
    const bool ordered = !last_outgoing
                         || *last_outgoing < row.source.value();
    last_outgoing = row.source.value();
    if (row.source.value() >= descriptor.before.node_slots
        || !ordered
        || !affected_outgoing.insert(row.source.value()).second
        || row.expected_arc_ids
               != committedOutgoingRow(committed, row.source))
      reject(violations, DualCsrViolationCode::kExpectedValue,
             "affected_base_out_rows",
             "affected existing BaseOut row must bind exact committed order");
    const auto found = post_outgoing_rows.find(row.source.value());
    const std::vector<csr::ArcId> actual
        = found == post_outgoing_rows.end()
              ? std::vector<csr::ArcId>{}
              : found->second;
    if (actual != row.replacement_arc_ids)
      reject(violations, DualCsrViolationCode::kAdjacency,
             "affected_base_out_rows",
             "physical BaseOut/Aux result differs from candidate order");
  }
  const auto require_outgoing = [&](csr::NodeId source,
                                    const std::string& field) {
    if (!affected_outgoing.contains(source.value()))
      reject(violations, DualCsrViolationCode::kAdjacency, field,
             "existing outgoing row write lacks typed order evidence");
  };
  for (const auto& item : descriptor.removes)
    require_outgoing(item.source, "removes");
  for (const auto& item : descriptor.changes)
    require_outgoing(item.source, "changes");
  for (const auto& item : descriptor.aux_rows)
    require_outgoing(item.source, "aux_rows");
  std::set<std::uint64_t> affected_incoming;
  std::optional<std::uint64_t> last_incoming;
  for (const auto& row : descriptor.affected_base_in_rows) {
    const bool ordered = !last_incoming
                         || *last_incoming < row.sink.value();
    last_incoming = row.sink.value();
    const bool existing = row.sink.value() < descriptor.before.node_slots;
    if (row.sink.value() >= descriptor.after.node_slots
        || !ordered
        || !affected_incoming.insert(row.sink.value()).second
        || (existing
                ? row.expected_arc_ids
                      != committedIncomingRow(committed, row.sink)
                : !row.expected_arc_ids.empty()))
      reject(violations, DualCsrViolationCode::kExpectedValue,
             "affected_base_in_rows",
             "affected BaseIn row must bind exact committed order");
    const auto found = post_incoming_rows.find(row.sink.value());
    const std::vector<csr::ArcId> actual
        = found == post_incoming_rows.end()
              ? std::vector<csr::ArcId>{}
              : found->second;
    if (actual != row.replacement_arc_ids)
      reject(violations, DualCsrViolationCode::kTranspose,
             "affected_base_in_rows",
             "physical BaseIn result differs from candidate order");
  }
  for (const auto& item : descriptor.reverse_changes)
    if (!affected_incoming.contains(item.sink.value()))
      reject(violations, DualCsrViolationCode::kTranspose,
             "reverse_changes",
             "incoming row write lacks typed order evidence");
  for (std::uint64_t node = descriptor.before.node_slots;
       node < descriptor.after.node_slots; ++node)
    if (!affected_incoming.contains(node))
      reject(violations, DualCsrViolationCode::kTranspose,
             "affected_base_in_rows",
             "every appended node must declare candidate BaseIn order");
  for (const auto id : post_physical)
    required_deactivations.erase(id);
  for (const auto id : required_deactivations)
    if (!deactivated.contains(id))
      reject(violations, DualCsrViolationCode::kConflict,
             "logical_deactivations",
             "physical/Aux removal lacks typed logical deactivation");
  std::set<std::uint64_t> node_changes;
  for (const auto& change : descriptor.node_payload_changes) {
    if (!node_changes.insert(change.id.value()).second
        || change.id.value() >= committed.nodes.size()) {
      reject(violations, DualCsrViolationCode::kId,
             "node_payload_changes",
             "node payload change must target one pre-existing NodeId");
      continue;
    }
    const auto& node = committed.nodes[change.id.value()];
    if (node.key != change.expected_key
        || node.net_name != change.expected_net_name)
      reject(violations, DualCsrViolationCode::kExpectedValue,
             "node_payload_changes",
             "node expected key/net does not match committed state");
  }
  std::set<std::uint64_t> payload_changes;
  for (const auto& change : descriptor.payload_changes) {
    if (!payload_changes.insert(change.id.value()).second
        || change.id.value() >= committed.arc_slots.size()) {
      reject(violations, DualCsrViolationCode::kId, "payload_changes",
             "payload change must target one pre-existing ArcId");
      continue;
    }
    const auto& slot = committed.arc_slots[change.id.value()];
    const bool stable_fields
        = change.replacement.key.base == change.expected.key.base
          && change.replacement.source == change.expected.source
          && change.replacement.sink == change.expected.sink
          && change.replacement.kind == change.expected.kind
          && change.replacement.is_clock_arc == change.expected.is_clock_arc
          && change.replacement.is_disabled == change.expected.is_disabled
          && change.replacement.is_loop_disabled
                 == change.expected.is_loop_disabled;
    if (!slot.active || slot.payload != change.expected || !stable_fields)
      reject(violations, DualCsrViolationCode::kExpectedValue,
             "payload_changes",
             "payload change may only replace timing/load/eligibility or the parallel ordinal");
    if (deactivated.contains(change.id.value()))
      reject(violations, DualCsrViolationCode::kConflict,
             "payload_changes",
             "one ArcId cannot be changed and deactivated in the same batch");
  }
  validatePostActiveArcKeys(committed, descriptor, deactivated,
                            violations);
  return violations;
}
}  // namespace ista::timing_dual_csr
