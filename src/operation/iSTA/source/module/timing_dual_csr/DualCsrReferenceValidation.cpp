// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrReferenceValidationInternal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CsrReferenceValidator.hpp"
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

}  // namespace

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
