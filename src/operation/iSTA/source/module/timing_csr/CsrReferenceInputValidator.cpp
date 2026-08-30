// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <numeric>

#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceInputValidatorInternal.hpp"
#include "CsrReferenceNativeModelValidatorInternal.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrReferenceValidatorDetail.hpp"

namespace ista::csr {

using namespace detail;

std::vector<CsrContractViolation> validateReferenceInputIntrinsic(const CsrReferenceInput& input)
{
  Violations errors;
  if (input.schema_version != kCsrSchemaVersion)
    reject(errors, "schema_version", "unsupported schema version");
  appendViolations(errors, validateAnalysisContextIntrinsic(input.analysis_context));
  appendViolations(errors, detail::validateParasiticContextCrossField(input));
  if (input.analysis_order != std::array{AnalysisMode::kMax, AnalysisMode::kMin})
    reject(errors, "analysis_order", "lane order must be max, min");
  if (input.path_source_order != std::array{PathSource::kInput, PathSource::kRegister})
    reject(errors, "path_source_order", "lane order must be input, register");
  if (input.transition_order != std::array{Transition::kRise, Transition::kFall})
    reject(errors, "transition_order", "lane order must be rise, fall");

  const std::size_t node_count = input.nodes.size();
  const std::size_t arc_count = input.arcs.size();
  bool node_keys_valid = true;
  std::vector<std::size_t> full_name_order(node_count);
  std::iota(full_name_order.begin(), full_name_order.end(), 0);
  std::vector<std::uint8_t> start_roles(node_count, 0);
  std::vector<std::uint8_t> end_roles(node_count, 0);
  for (std::size_t index = 0; index < input.nodes.size(); ++index) {
    const auto& node = input.nodes[index];
    if (node.id.value() != index)
      reject(errors, "nodes", "NodeId must be dense and equal vector position");
    if (auto error = validateNodeKey(node.key)) {
      reject(errors, "nodes.key", *error);
      node_keys_valid = false;
    }
    if (!knownEnum(node.direction))
      reject(errors, "nodes.direction", "PinDirection enum is unknown");
    if (node.is_port != (node.key.kind == NodeKind::kPort))
      reject(errors, "nodes.is_port", "port role differs from NodeKey kind");
    if (node.is_port && !node.instance_cell.empty())
      reject(errors, "nodes.instance_cell", "port must not name an instance cell");
    if (node.is_port && (node.is_sequential || node.is_clock_pin || node.is_data_pin || node.is_output_pin))
      reject(errors, "nodes.roles", "ports must not carry instance-only roles");
    if (!node.is_port && node.instance_cell.empty())
      reject(errors, "nodes.instance_cell", "instance pin requires an instance cell");
    start_roles[index] = node.is_startpoint;
    end_roles[index] = node.is_endpoint;
  }
  if (node_keys_valid) {
    for (std::size_t index = 1; index < node_count; ++index)
      if (!nodeKeyLess(input.nodes[index - 1].key, input.nodes[index].key))
        reject(errors, "nodes.key", "NodeIds must follow strict canonical NodeKey order");
  }
  std::ranges::sort(full_name_order, [&](std::size_t lhs, std::size_t rhs) {
    const auto& left = input.nodes[lhs].key.full_name;
    const auto& right = input.nodes[rhs].key.full_name;
    return left == right ? lhs < rhs : byteStringLess(left, right);
  });
  for (std::size_t index = 1; index < full_name_order.size(); ++index)
    if (input.nodes[full_name_order[index - 1]].key.full_name == input.nodes[full_name_order[index]].key.full_name)
      reject(errors, "nodes.key.full_name", "full names must be unique");
  if (input.node_ids_by_key.size() != node_count) {
    reject(errors, "node_ids_by_key", "reverse lookup must contain exactly N entries");
  } else {
    for (std::size_t index = 0; index < node_count; ++index)
      if (input.node_ids_by_key[index].id.value() != index || !(input.node_ids_by_key[index].key == input.nodes[index].key))
        reject(errors, "node_ids_by_key", "reverse lookup must exactly invert canonical NodeIds");
  }

  std::vector<std::uint8_t> pin_owners(node_count, 0);
  std::vector<std::string> instance_names;
  instance_names.reserve(input.instances.size());
  for (const auto& instance : input.instances) {
    if (instance.instance_name.empty() || instance.cell_name.empty())
      reject(errors, "instances", "instance and cell names must be nonempty");
    instance_names.push_back(instance.instance_name);
    std::vector<std::uint8_t> local_pins(node_count, 0);
    bool pins_valid = true;
    std::size_t clock_pin_count = 0;
    std::size_t data_pin_count = 0;
    std::size_t output_pin_count = 0;
    for (NodeId pin : instance.ordered_pins) {
      if (pin.value() >= node_count) {
        reject(errors, "instances.ordered_pins", "NodeId is out of range");
        pins_valid = false;
        continue;
      }
      const auto& node = input.nodes[pin.value()];
      if (local_pins[pin.value()]++ != 0 || pin_owners[pin.value()]++ != 0)
        reject(errors, "instances.ordered_pins", "instance pin is duplicated");
      if (node.is_port || node.key.instance_name != instance.instance_name || node.instance_cell != instance.cell_name)
        reject(errors, "instances.ordered_pins", "pin does not resolve to this instance and cell");
      if (node.is_sequential != instance.is_sequential)
        reject(errors, "instances.is_sequential", "instance and pin sequential roles differ");
      clock_pin_count += node.is_clock_pin;
      data_pin_count += node.is_data_pin;
      output_pin_count += node.is_output_pin;
    }
    if (clock_pin_count > 1 || data_pin_count > 1 || output_pin_count > 1)
      reject(errors, "instances.roles", "clock, data, and output roles may each identify at most one pin per instance");
    if (pins_valid) {
      auto expected_pins = instance.ordered_pins;
      std::ranges::sort(expected_pins,
                        [&](NodeId lhs, NodeId rhs) { return byteStringLess(input.nodes[lhs.value()].key.full_name, input.nodes[rhs.value()].key.full_name); });
      if (instance.ordered_pins != expected_pins)
        reject(errors, "instances.ordered_pins", "pins must follow frozen full-name order");
    }
  }
  for (std::size_t index = 1; index < instance_names.size(); ++index)
    if (!byteStringLess(instance_names[index - 1], instance_names[index]))
      reject(errors, "instances.instance_name", "instances must follow strict frozen map-name order");
  for (std::size_t node = 0; node < node_count; ++node) {
    const std::uint8_t expected = input.nodes[node].is_port ? 0 : 1;
    if (pin_owners[node] != expected)
      reject(errors, "instances.ordered_pins", "every instance pin must resolve exactly once");
  }

  bool arc_keys_valid = true;
  bool ranks_valid = true;
  std::vector<std::size_t> arc_at_rank(arc_count, 0);
  std::vector<std::uint8_t> rank_seen(arc_count, 0);
  std::vector<std::uint64_t> eligible_indegree(node_count, 0);
  for (std::size_t index = 0; index < input.arcs.size(); ++index) {
    const auto& arc = input.arcs[index];
    if (arc.id.value() != index)
      reject(errors, "arcs", "ArcId must be dense and equal vector position");
    const bool endpoints_valid = arc.source.value() < node_count && arc.sink.value() < node_count;
    if (!endpoints_valid) {
      reject(errors, "arcs.endpoints", "arc endpoint is out of range");
    } else if (!(arc.key.base.source == input.nodes[arc.source.value()].key) || !(arc.key.base.sink == input.nodes[arc.sink.value()].key)) {
      reject(errors, "arcs.key", "ArcKey endpoints do not match dense endpoints");
    }
    if (!knownEnum(arc.kind))
      reject(errors, "arcs.kind", "ArcKind enum is unknown");
    if (arc.kind != arc.key.base.kind)
      reject(errors, "arcs.kind", "ArcRecord and ArcKey kinds differ");
    if (knownEnum(arc.kind) && arc.kind == ArcKind::kCell) {
      if (endpoints_valid) {
        const auto& source = input.nodes[arc.source.value()];
        const auto& sink = input.nodes[arc.sink.value()];
        if (source.is_port || sink.is_port || source.key.instance_name != sink.key.instance_name || source.instance_cell != sink.instance_cell
            || arc.key.base.owner_name != source.key.instance_name || arc.key.base.library_source_port != source.key.pin_name
            || arc.key.base.library_sink_port != sink.key.pin_name)
          reject(errors, "arcs.key", "cell arc ownership and library ports do not match its same-instance endpoints");
      }
    } else if (knownEnum(arc.kind) && arc.kind == ArcKind::kNet) {
      if (endpoints_valid) {
        const auto& source = input.nodes[arc.source.value()];
        const auto& sink = input.nodes[arc.sink.value()];
        if (source.net_name.empty() || source.net_name != sink.net_name || arc.key.base.owner_name != source.net_name)
          reject(errors, "arcs.key", "net arc owner and endpoint net names must be the same nonempty net");
      }
    }
    const bool any_eligible = arc.eligibility.levelization || arc.eligibility.data_arrival || arc.eligibility.data_slew || arc.eligibility.clock;
    if ((arc.is_disabled || arc.is_loop_disabled) && any_eligible)
      reject(errors, "arcs.eligibility", "disabled and loop-disabled arcs must be ineligible");
    if ((arc.eligibility.data_arrival || arc.eligibility.data_slew || arc.eligibility.clock) && !arc.eligibility.levelization)
      reject(errors, "arcs.eligibility", "propagation eligibility requires levelization eligibility");
    if (auto error = validateArcKeyBase(arc.key.base)) {
      reject(errors, "arcs.key", *error);
      arc_keys_valid = false;
    }
    if (arc.native_traversal_rank >= arc_count || rank_seen[arc.native_traversal_rank]++ != 0) {
      reject(errors, "arcs.native_traversal_rank", "ranks must be a permutation of [0,E)");
      ranks_valid = false;
    } else {
      arc_at_rank[arc.native_traversal_rank] = index;
    }
    if (endpoints_valid && !arc.is_disabled && !arc.is_loop_disabled)
      ++eligible_indegree[arc.sink.value()];
  }

  if (arc_keys_valid) {
    for (std::size_t index = 1; index < arc_count; ++index)
      if (!arcKeyLess(input.arcs[index - 1].key, input.arcs[index].key))
        reject(errors, "arcs.key", "ArcIds must follow strict canonical ArcKey order");
  }
  if (input.arc_ids_by_key.size() != arc_count) {
    reject(errors, "arc_ids_by_key", "reverse lookup must contain exactly E entries");
  } else {
    for (std::size_t index = 0; index < arc_count; ++index)
      if (input.arc_ids_by_key[index].id.value() != index || !(input.arc_ids_by_key[index].key == input.arcs[index].key))
        reject(errors, "arc_ids_by_key", "reverse lookup must exactly invert canonical ArcIds");
  }
  if (arc_keys_valid && ranks_valid) {
    std::vector<ArcKeyBase> bases;
    bases.reserve(arc_count);
    for (std::size_t rank = 0; rank < arc_count; ++rank)
      bases.push_back(input.arcs[arc_at_rank[rank]].key.base);
    try {
      const auto assigned = assignParallelOrdinals(bases);
      for (std::size_t rank = 0; rank < arc_count; ++rank)
        if (assigned[rank].parallel_ordinal != input.arcs[arc_at_rank[rank]].key.parallel_ordinal)
          reject(errors, "arcs.key.parallel_ordinal", "ordinals must be continuous in native traversal order");
    } catch (const std::invalid_argument& error) {
      reject(errors, "arcs.key.parallel_ordinal", error.what());
    }
  }
  validateAdjacency(input.out_row_offsets, input.out_arc_ids, input.arcs, node_count, true, errors, "base_out");
  validateAdjacency(input.in_row_offsets, input.in_arc_ids, input.arcs, node_count, false, errors, "base_in");

  std::vector<std::uint8_t> root_roles(node_count, 0);
  std::vector<NodeId> expected_roots;
  std::vector<NodeId> expected_starts;
  std::vector<NodeId> expected_ends;
  for (std::size_t node : full_name_order) {
    if (start_roles[node])
      expected_starts.emplace_back(node);
    if (end_roles[node])
      expected_ends.emplace_back(node);
    if (eligible_indegree[node] == 0) {
      root_roles[node] = 1;
      expected_roots.emplace_back(node);
    }
  }
  validateExactNodeSet(input.ordered_root_ids, root_roles, errors, "ordered_root_ids");
  if (input.ordered_root_ids != expected_roots)
    reject(errors, "ordered_root_ids", "roots must follow frozen full-name order");
  validateExactNodeSet(input.ordered_start_ids, start_roles, errors, "ordered_start_ids");
  validateExactNodeSet(input.ordered_end_ids, end_roles, errors, "ordered_end_ids");
  if (input.ordered_start_ids != expected_starts)
    reject(errors, "ordered_start_ids", "startpoints must follow frozen full-name order");
  if (input.ordered_end_ids != expected_ends)
    reject(errors, "ordered_end_ids", "endpoints must follow frozen full-name order");

  validateReferenceNativeModels(input, errors);
  validateReferenceConstraints(input, errors);
  return errors;
}

std::vector<CsrContractViolation> validateReferenceInput(const CsrReferenceInput& input)
{
  Violations errors = validateReferenceInputIntrinsic(input);
  if (!errors.empty())
    return errors;
  appendViolations(errors, validateStage2CommittedIdentity(input.source_identity, "source_identity"));
  if (!errors.empty())
    return errors;
  const CsrSnapshotIdentity expected_identity = makeReferenceInputIdentity(input, input.source_identity.session_token, input.source_identity.graph_generation);
  if (!(input.source_identity == expected_identity))
    reject(errors, "source_identity", "identity does not match the candidate-only fingerprint payloads");
  return errors;
}

std::vector<CsrContractViolation> validateReferenceInputForSession(const CsrReferenceInput& input, std::uint64_t expected_session_token)
{
  Violations errors = validateReferenceInput(input);
  if (expected_session_token == 0) {
    reject(errors, "expected_session_token", "expected session token must be nonzero");
  } else if (input.source_identity.session_token != expected_session_token) {
    reject(errors, "source_identity.session_token", "identity is not bound to the expected session token");
  }
  return errors;
}

}  // namespace ista::csr
