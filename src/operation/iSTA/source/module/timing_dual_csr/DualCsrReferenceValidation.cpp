// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrReferenceValidationInternal.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <map>
#include <set>
#include <string_view>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CsrReferenceValidator.hpp"
#include "CsrReferenceNativeModelValidatorInternal.hpp"
#include "TimingSemanticKey.hpp"

#include "DualCsrValidationInternal.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

bool byteStringLess(const std::string& lhs, const std::string& rhs)
{
  return std::lexicographical_compare(
      lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
      [](char left, char right) {
        return static_cast<unsigned char>(left)
               < static_cast<unsigned char>(right);
      });
}

DualCsrViolationCode referenceCode(const std::string& field)
{
  if (field.find("schema") != std::string::npos
      || field.find("_order") != std::string::npos
      || field.find("unsupported_reasons") != std::string::npos)
    return DualCsrViolationCode::kSchema;
  if (field.find("id") != std::string::npos
      || field.find("node") != std::string::npos
      || field.find("pin") != std::string::npos)
    return DualCsrViolationCode::kId;
  if (field.find("key") != std::string::npos
      || field.find("name") != std::string::npos)
    return DualCsrViolationCode::kKey;
  return DualCsrViolationCode::kExpectedValue;
}

void appendReferenceViolations(
    const std::vector<csr::CsrContractViolation>& errors,
    const std::string& prefix, DualCsrViolations& violations)
{
  for (const auto& error : errors)
    reject(violations, referenceCode(error.field),
           prefix + error.field, error.message);
}

csr::NodeId remap(csr::NodeId id,
                  const std::vector<std::size_t>& old_to_new) noexcept
{
  if (id.value() >= old_to_new.size())
    return csr::NodeId{old_to_new.size()};
  return csr::NodeId{old_to_new[id.value()]};
}

std::optional<csr::NodeId> remap(
    const std::optional<csr::NodeId>& id,
    const std::vector<std::size_t>& old_to_new) noexcept
{
  return id ? std::optional{remap(*id, old_to_new)} : std::nullopt;
}

void remapNodeList(std::vector<csr::NodeId>& ids,
                   const std::vector<std::size_t>& old_to_new)
{
  for (auto& id : ids)
    id = remap(id, old_to_new);
}

std::vector<std::size_t> canonicalNodeOrder(const DualCsrState& state)
{
  std::vector<std::size_t> order(state.nodes.size());
  std::iota(order.begin(), order.end(), 0);
  std::ranges::sort(order, [&](std::size_t lhs, std::size_t rhs) {
    const auto& left = state.nodes[lhs].key;
    const auto& right = state.nodes[rhs].key;
    if (csr::nodeKeyLess(left, right))
      return true;
    if (csr::nodeKeyLess(right, left))
      return false;
    return lhs < rhs;
  });
  return order;
}

void projectNodes(const DualCsrState& state,
                  const std::vector<std::size_t>& order,
                  std::vector<std::size_t>& old_to_new,
                  csr::CsrReferenceInput& input)
{
  input.nodes.reserve(order.size());
  input.node_ids_by_key.reserve(order.size());
  for (std::size_t new_id = 0; new_id < order.size(); ++new_id) {
    old_to_new[order[new_id]] = new_id;
    auto node = state.nodes[order[new_id]];
    node.id = csr::NodeId{new_id};
    input.node_ids_by_key.push_back({node.key, node.id});
    input.nodes.push_back(std::move(node));
  }
}

void projectInstances(const DualCsrReferencePayload& payload,
                      const std::vector<std::size_t>& old_to_new,
                      csr::CsrReferenceInput& input)
{
  input.instances = payload.instances;
  std::ranges::sort(input.instances, [](const auto& a, const auto& b) { return byteStringLess(a.instance_name, b.instance_name); });
  for (auto& instance : input.instances)
    remapNodeList(instance.ordered_pins, old_to_new);
}

void projectArcs(const DualCsrState& state,
                 const std::vector<std::size_t>& old_to_new,
                 csr::CsrReferenceInput& input)
{
  std::vector<std::pair<csr::ArcId, const csr::ArcSemanticPayload*>> active;
  for (std::size_t id = 0; id < state.arc_slots.size(); ++id)
    if (state.arc_slots[id].active)
      active.emplace_back(csr::ArcId{id}, &state.arc_slots[id].payload);
  std::ranges::sort(active, [](const auto& lhs, const auto& rhs) {
    if (csr::arcKeyLess(lhs.second->key, rhs.second->key))
      return true;
    if (csr::arcKeyLess(rhs.second->key, lhs.second->key))
      return false;
    return lhs.first.value() < rhs.first.value();
  });

  input.arcs.reserve(active.size());
  input.arc_ids_by_key.reserve(active.size());
  std::optional<csr::ArcKeyBase> previous_base;
  std::uint64_t ordinal = 0;
  for (std::size_t index = 0; index < active.size(); ++index) {
    const auto& payload = *active[index].second;
    if (!previous_base || !(*previous_base == payload.key.base)) {
      previous_base = payload.key.base;
      ordinal = 0;
    } else {
      ++ordinal;
    }
    csr::ArcRecord arc;
    arc.id = csr::ArcId{index};
    arc.key = payload.key;
    arc.key.parallel_ordinal = ordinal;
    arc.source = remap(payload.source, old_to_new);
    arc.sink = remap(payload.sink, old_to_new);
    arc.kind = payload.kind;
    arc.native_traversal_rank = index;
    arc.is_clock_arc = payload.is_clock_arc;
    arc.is_disabled = payload.is_disabled;
    arc.is_loop_disabled = payload.is_loop_disabled;
    arc.eligibility = payload.eligibility;
    arc.transfer = payload.transfer;
    arc.output_load = payload.output_load;
    input.arc_ids_by_key.push_back({arc.key, arc.id});
    input.arcs.push_back(std::move(arc));
  }
}

void projectAdjacency(csr::CsrReferenceInput& input)
{
  std::vector<std::vector<csr::ArcId>> outgoing(input.nodes.size());
  std::vector<std::vector<csr::ArcId>> incoming(input.nodes.size());
  for (const auto& arc : input.arcs) {
    if (arc.source.value() < outgoing.size())
      outgoing[arc.source.value()].push_back(arc.id);
    if (arc.sink.value() < incoming.size())
      incoming[arc.sink.value()].push_back(arc.id);
  }
  input.out_row_offsets.push_back(0);
  input.in_row_offsets.push_back(0);
  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    input.out_arc_ids.insert(input.out_arc_ids.end(), outgoing[node].begin(),
                             outgoing[node].end());
    input.out_row_offsets.push_back(input.out_arc_ids.size());
    input.in_arc_ids.insert(input.in_arc_ids.end(), incoming[node].begin(),
                            incoming[node].end());
    input.in_row_offsets.push_back(input.in_arc_ids.size());
  }
}

std::vector<csr::NodeId> projectedNativeRoots(
    const csr::CsrReferenceInput& input)
{
  std::vector<std::uint64_t> indegree(input.nodes.size(), 0);
  for (const auto& arc : input.arcs)
    if (arc.sink.value() < indegree.size() && !arc.is_disabled
        && !arc.is_loop_disabled)
      ++indegree[arc.sink.value()];
  std::vector<std::size_t> order(input.nodes.size());
  std::iota(order.begin(), order.end(), 0);
  std::ranges::sort(order, [&](std::size_t lhs, std::size_t rhs) {
    return byteStringLess(input.nodes[lhs].key.full_name,
                          input.nodes[rhs].key.full_name);
  });
  std::vector<csr::NodeId> roots;
  for (const auto node : order)
    if (indegree[node] == 0)
      roots.emplace_back(node);
  return roots;
}

void projectReferencePayload(const DualCsrState& state,
                             const std::vector<std::size_t>& old_to_new,
                             csr::CsrReferenceInput& input)
{
  const auto& payload = state.reference_payload;
  input.schema_version = payload.schema_version;
  input.source_identity = payload.source_identity;
  input.analysis_context = payload.analysis_context;
  input.analysis_order = payload.analysis_order;
  input.path_source_order = payload.path_source_order;
  input.transition_order = payload.transition_order;
  projectInstances(payload, old_to_new, input);
  input.ordered_start_ids = payload.ordered_start_ids;
  input.ordered_end_ids = payload.ordered_end_ids;
  remapNodeList(input.ordered_start_ids, old_to_new);
  remapNodeList(input.ordered_end_ids, old_to_new);
  input.clock_tags = payload.clock_tags;
  input.clocks = payload.clocks;
  for (auto& clock : input.clocks)
    for (auto& source : clock.ordered_sources)
      source.resolved_node = remap(source.resolved_node, old_to_new);
  if (payload.clock_owner_by_node.size() == old_to_new.size()) {
    input.clock_owner_by_node.resize(old_to_new.size());
    for (std::size_t old_id = 0; old_id < old_to_new.size(); ++old_id)
      input.clock_owner_by_node[old_to_new[old_id]]
          = payload.clock_owner_by_node[old_id];
  } else {
    input.clock_owner_by_node = payload.clock_owner_by_node;
  }
  input.port_constraints = payload.port_constraints;
  for (auto& constraint : input.port_constraints)
    constraint.resolved_node = remap(constraint.resolved_node, old_to_new);
  input.case_analysis_constraints = payload.case_analysis_constraints;
  for (auto& constraint : input.case_analysis_constraints)
    constraint.resolved_node = remap(constraint.resolved_node, old_to_new);
  input.timing_check_endpoints = payload.timing_check_endpoints;
  for (auto& endpoint : input.timing_check_endpoints) {
    endpoint.resolved_clock_node
        = remap(endpoint.resolved_clock_node, old_to_new);
    endpoint.resolved_data_node
        = remap(endpoint.resolved_data_node, old_to_new);
  }
  input.referenced_library_cells = payload.referenced_library_cells;
  std::ranges::sort(input.referenced_library_cells, [](const auto& a, const auto& b) { return byteStringLess(a.cell_name, b.cell_name); });
  input.parasitics = payload.parasitics;
  input.start_seeds = payload.start_seeds;
  for (auto& seed : input.start_seeds) {
    seed.seed_node = remap(seed.seed_node, old_to_new);
    seed.path_start_node = remap(seed.path_start_node, old_to_new);
    seed.crpr_node = remap(seed.crpr_node, old_to_new);
  }
  input.endpoint_required = payload.endpoint_required;
  for (auto& endpoint : input.endpoint_required)
    endpoint.node = remap(endpoint.node, old_to_new);
  input.cell_pin_capacitances = payload.cell_pin_capacitances;
  for (auto& capacitance : input.cell_pin_capacitances)
    capacitance.node = remap(capacitance.node, old_to_new);
  std::ranges::sort(input.cell_pin_capacitances, [&](const auto& a, const auto& b) {
    if (a.node.value() >= input.nodes.size() || b.node.value() >= input.nodes.size())
      return a.node.value() < b.node.value();
    return byteStringLess(input.nodes[a.node.value()].key.full_name, input.nodes[b.node.value()].key.full_name);
  });
  input.unsupported_reasons = payload.unsupported_reasons;
  // Stage 2 derives roots from non-disabled native arcs. The Stage 5 root
  // contract is checked separately with exact levelization eligibility.
  input.ordered_root_ids = projectedNativeRoots(input);
}

csr::CsrReferenceInput canonicalValidationInput(const DualCsrState& state)
{
  csr::CsrReferenceInput input;
  const auto order = canonicalNodeOrder(state);
  std::vector<std::size_t> old_to_new(state.nodes.size(), 0);
  projectNodes(state, order, old_to_new, input);
  projectArcs(state, old_to_new, input);
  projectAdjacency(input);
  projectReferencePayload(state, old_to_new, input);
  return input;
}

void validateEligibleRoots(const DualCsrState& state,
                           DualCsrViolations& violations)
{
  std::vector<std::uint64_t> indegree(state.nodes.size(), 0);
  for (const auto& slot : state.arc_slots)
    if (slot.active && slot.payload.eligibility.levelization
        && slot.payload.sink.value() < indegree.size())
      ++indegree[slot.payload.sink.value()];
  std::vector<std::size_t> order(state.nodes.size());
  std::iota(order.begin(), order.end(), 0);
  std::ranges::sort(order, [&](std::size_t lhs, std::size_t rhs) {
    return byteStringLess(state.nodes[lhs].key.full_name,
                          state.nodes[rhs].key.full_name);
  });
  std::vector<csr::NodeId> expected;
  for (const auto node : order)
    if (indegree[node] == 0)
      expected.emplace_back(node);
  if (state.reference_payload.ordered_root_ids != expected)
    reject(violations, DualCsrViolationCode::kAdjacency,
           "reference_payload.ordered_root_ids",
           "ordered roots must exactly equal the levelization-eligible indegree-zero set in frozen full-name order");
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
template <typename T>
void validateAppend(const std::optional<RecordAppend<T>>& change, const std::vector<T>& current,
                    const std::string& field, DualCsrViolations& violations)
{
  if (!change)
    return;
  if (change->expected_size != current.size())
    reject(violations, DualCsrViolationCode::kExpectedValue, field, "append size differs from committed payload");
  if (change->values.size() > current.max_size() - current.size())
    reject(violations, DualCsrViolationCode::kCapacity, field, "append exceeds container capacity");
}
void validateReferenceAppends(const DualCsrState& committed, const DualCsrBatchDescriptor& descriptor,
                             DualCsrViolations& violations)
{
  const auto& delta = descriptor.reference_payload;
  const auto& current = committed.reference_payload;
  const auto rejectField = [&](const char* field, const char* message) {
    reject(violations, DualCsrViolationCode::kExpectedValue, std::string{"reference_payload."} + field, message);
  };
  if (delta.referenced_library_cells) {
    std::set<std::string_view> names;
    for (const auto& cell : delta.referenced_library_cells->values) {
      if (cell.cell_name.empty() || !names.insert(cell.cell_name).second
          || std::ranges::find(current.referenced_library_cells, cell.cell_name, &csr::ReferencedLibraryCellModel::cell_name)
                 != current.referenced_library_cells.end())
        rejectField("referenced_library_cells", "appended cell names must be nonempty and new");
      std::vector<csr::CsrContractViolation> errors;
      csr::detail::validateLibraryCell(cell, errors);
      for (const auto& error : errors)
        reject(violations, DualCsrViolationCode::kExpectedValue, error.field, error.message);
    }
  }
  std::set<std::uint64_t> expected_pins;
  for (const auto& append : descriptor.node_appends)
    if (!append.payload.is_port)
      expected_pins.insert(append.id.value());
  std::map<std::uint64_t, const csr::LibraryPortModel*> pin_models;
  if (delta.instances) {
    std::set<std::string_view> names;
    for (const auto& instance : delta.instances->values) {
      if (instance.instance_name.empty() || instance.cell_name.empty() || !names.insert(instance.instance_name).second
          || std::ranges::find(current.instances, instance.instance_name, &csr::InstanceRecord::instance_name) != current.instances.end())
        rejectField("instances", "appended instance names must be nonempty and new");
      const auto old_cell = std::ranges::find(current.referenced_library_cells, instance.cell_name, &csr::ReferencedLibraryCellModel::cell_name);
      const csr::ReferencedLibraryCellModel* cell = old_cell == current.referenced_library_cells.end() ? nullptr : &*old_cell;
      if (!cell && delta.referenced_library_cells) {
        const auto found = std::ranges::find(delta.referenced_library_cells->values, instance.cell_name, &csr::ReferencedLibraryCellModel::cell_name);
        if (found != delta.referenced_library_cells->values.end())
          cell = &*found;
      }
      if (!cell || instance.is_sequential != (cell->present && cell->is_sequential)
          || instance.is_clock_gating != (cell->present && cell->is_clock_gating) || instance.is_macro != (cell->present && cell->is_macro))
        rejectField("instances", "appended instance requires matching model roles");
      std::optional<std::string_view> previous_pin;
      for (const auto id : instance.ordered_pins) {
        if (id.value() < descriptor.before.node_slots || id.value() - descriptor.before.node_slots >= descriptor.node_appends.size()
            || pin_models.contains(id.value())) {
          rejectField("instances", "appended pins must be distinct new NodeIds");
          continue;
        }
        const auto& node = descriptor.node_appends[id.value() - descriptor.before.node_slots].payload;
        if (node.is_port || node.key.instance_name != instance.instance_name || node.instance_cell != instance.cell_name
            || node.is_sequential != instance.is_sequential)
          rejectField("instances", "appended pin identity differs from instance");
        if (previous_pin && !std::lexicographical_compare(previous_pin->begin(), previous_pin->end(), node.key.full_name.begin(), node.key.full_name.end(),
                                                        [](unsigned char a, unsigned char b) { return a < b; }))
          rejectField("instances", "appended pins must follow strict full-name order");
        previous_pin = node.key.full_name;
        const csr::LibraryPortModel* port = nullptr;
        if (cell && cell->present) {
          const auto found = std::ranges::find(cell->ports, node.key.pin_name, &csr::LibraryPortModel::name);
          if (found != cell->ports.end())
            port = &*found;
        }
        pin_models.emplace(id.value(), port);
        if (cell && (node.is_clock_pin != (cell->present && node.key.pin_name == cell->clock_pin_name)
                     || node.is_data_pin != (cell->present && node.key.pin_name == cell->data_pin_name)
                     || node.is_output_pin != (cell->present && node.key.pin_name == cell->output_pin_name)
                     || (port && node.direction != port->direction)))
          rejectField("instances", "appended pin roles differ from its model");
      }
    }
  }
  if (pin_models.size() != expected_pins.size()
      || !std::ranges::all_of(expected_pins, [&](auto id) { return pin_models.contains(id); }))
    rejectField("instances", "instances must cover every appended non-port node exactly once");
  if (delta.referenced_library_cells)
    for (const auto& cell : delta.referenced_library_cells->values)
      if (!delta.instances || std::ranges::none_of(delta.instances->values, [&](const auto& instance) { return instance.cell_name == cell.cell_name; }))
        rejectField("referenced_library_cells", "appended model must be referenced by an appended instance");
  std::set<std::uint64_t> capacitance_pins;
  if (delta.cell_pin_capacitances) {
    for (const auto& cap : delta.cell_pin_capacitances->values) {
      const bool valid_node = cap.node.value() >= descriptor.before.node_slots
                              && cap.node.value() - descriptor.before.node_slots < descriptor.node_appends.size();
      if (!valid_node || !capacitance_pins.insert(cap.node.value()).second
          || (valid_node && descriptor.node_appends[cap.node.value() - descriptor.before.node_slots].payload.is_port))
        rejectField("cell_pin_capacitances", "appended capacitance must name one new instance pin");
      const auto model = pin_models.find(cap.node.value());
      const auto* port = model == pin_models.end() ? nullptr : model->second;
      if (cap.default_capacitance != (port ? port->default_capacitance : 0.0)
          || cap.transition_capacitance != (port ? port->transition_capacitance : csr::OptionalLaneMatrix{}))
        rejectField("cell_pin_capacitances", "appended capacitance differs from the referenced Liberty port");
      if (!std::isfinite(cap.default_capacitance) || cap.default_capacitance < 0)
        rejectField("cell_pin_capacitances", "capacitance must be finite and nonnegative");
      for (const auto& row : cap.transition_capacitance)
        for (const auto& value : row)
          if (value && (!std::isfinite(*value) || *value < 0))
            rejectField("cell_pin_capacitances", "transition capacitance must be finite and nonnegative");
    }
  }
  if (capacitance_pins != expected_pins)
    rejectField("cell_pin_capacitances", "capacitance append must cover every new instance pin exactly once");
  if (delta.clock_owner_by_node) {
    if (delta.clock_owner_by_node->values.size() != descriptor.node_appends.size())
      rejectField("clock_owner_by_node", "owner append must cover the new nodes");
    for (const auto& owner : delta.clock_owner_by_node->values)
      if (owner && std::ranges::find(current.clocks, *owner, &csr::ClockConstraintModel::map_key) == current.clocks.end())
        rejectField("clock_owner_by_node", "clock owner must resolve in the session");
  }
}

}  // namespace

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
  validateAppend(delta.instances, current.instances,
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
  validateAppend(delta.clock_owner_by_node, current.clock_owner_by_node,
                   "reference_payload.clock_owner_by_node", violations);
  validateExpected(delta.port_constraints, current.port_constraints,
                   "reference_payload.port_constraints", violations);
  validateExpected(delta.case_analysis_constraints,
                   current.case_analysis_constraints,
                   "reference_payload.case_analysis_constraints", violations);
  validateExpected(delta.timing_check_endpoints,
                   current.timing_check_endpoints,
                   "reference_payload.timing_check_endpoints", violations);
  validateAppend(delta.referenced_library_cells,
                   current.referenced_library_cells,
                   "reference_payload.referenced_library_cells", violations);
  validateExpected(delta.parasitics, current.parasitics,
                   "reference_payload.parasitics", violations);
  validateExpected(delta.start_seeds, current.start_seeds,
                   "reference_payload.start_seeds", violations);
  validateExpected(delta.endpoint_required, current.endpoint_required,
                   "reference_payload.endpoint_required", violations);
  validateAppend(delta.cell_pin_capacitances,
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
  validateReferenceAppends(committed, descriptor, violations);
}

void validateReferencePayload(const DualCsrState& state,
                              DualCsrViolations& violations)
{
  validateEligibleRoots(state, violations);
  appendReferenceViolations(
      csr::validateSnapshotIdentitySyntax(
          state.reference_payload.source_identity,
          "reference_payload.source_identity"),
      "", violations);
  const auto input = canonicalValidationInput(state);
  appendReferenceViolations(csr::validateReferenceInputIntrinsic(input),
                            "reference_payload.", violations);
}

}  // namespace ista::timing_dual_csr::detail
