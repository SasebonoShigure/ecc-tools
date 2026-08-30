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
#include "CsrGraphValidator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "TimingSemanticKey.hpp"

namespace ista::csr {
namespace {

class Reporter
{
 public:
  void malformed(std::string field, std::string message) { _violations.push_back({CsrCaptureCode::kMalformedTopology, std::move(field), std::move(message)}); }

  void collision(std::string field, std::string message)
  {
    _violations.push_back({CsrCaptureCode::kSemanticKeyCollision, std::move(field), std::move(message)});
  }

  [[nodiscard]] std::vector<CsrGraphContractViolation> take() { return std::move(_violations); }

 private:
  std::vector<CsrGraphContractViolation> _violations;
};

template <typename Enum>
bool hasKnownToken(Enum value)
{
  try {
    static_cast<void>(token(value));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

std::string indexed(std::string_view family, std::size_t index, std::string_view member)
{
  return std::string(family) + "[" + std::to_string(index) + "]." + std::string(member);
}

bool validNodeId(NodeId id, std::size_t count)
{
  return id.value() < count;
}

bool validArcId(ArcId id, std::size_t count)
{
  return id.value() < count;
}

void validateNodeStage(const CsrGraphSnapshot& snapshot, Reporter& reporter)
{
  bool all_keys_valid = true;
  for (std::size_t index = 0; index < snapshot.nodes.size(); ++index) {
    const auto& node = snapshot.nodes[index];
    if (node.id != NodeId{index})
      reporter.malformed(indexed("nodes", index, "id"), "node ID must equal its canonical row index");
    if (auto error = validateNodeKey(node.key)) {
      reporter.malformed(indexed("nodes", index, "key"), *error);
      all_keys_valid = false;
    }
    if (!hasKnownToken(node.direction))
      reporter.malformed(indexed("nodes", index, "direction"), "pin direction is unknown");
    const bool key_is_port = node.key.kind == NodeKind::kPort;
    if (node.is_port != key_is_port)
      reporter.malformed(indexed("nodes", index, "is_port"), "is_port must agree with NodeKey kind");
    if (node.is_port && (!node.instance_cell.empty() || node.is_sequential || node.is_clock_pin || node.is_data_pin || node.is_output_pin))
      reporter.malformed(indexed("nodes", index, "roles"), "port contains instance-only cell or role state");
  }

  if (all_keys_valid) {
    for (std::size_t index = 1; index < snapshot.nodes.size(); ++index) {
      const auto& previous = snapshot.nodes[index - 1].key;
      const auto& current = snapshot.nodes[index].key;
      if (previous == current) {
        reporter.collision(indexed("nodes", index, "key"), "canonical NodeKey collides with the previous row");
      } else if (!nodeKeyLess(previous, current)) {
        reporter.malformed(indexed("nodes", index, "key"), "nodes are not in canonical NodeKey order");
      }
    }
  }

  if (snapshot.node_ids_by_key.size() != snapshot.nodes.size())
    reporter.malformed("node_ids_by_key", "lookup size must equal node count");
  const std::size_t comparable = std::min(snapshot.node_ids_by_key.size(), snapshot.nodes.size());
  for (std::size_t index = 0; index < comparable; ++index) {
    const auto& lookup = snapshot.node_ids_by_key[index];
    if (lookup.id != NodeId{index} || lookup.key != snapshot.nodes[index].key)
      reporter.malformed(indexed("node_ids_by_key", index, "entry"), "lookup must be the exact canonical key-to-ID inverse");
  }
}

struct InstanceOwnerInfo
{
  std::size_t first_row;
  std::size_t count{0};
};

std::unordered_map<std::string, InstanceOwnerInfo> indexInstanceOwners(const CsrGraphSnapshot& snapshot)
{
  std::unordered_map<std::string, InstanceOwnerInfo> owners;
  owners.reserve(snapshot.instances.size());
  for (std::size_t row = 0; row < snapshot.instances.size(); ++row) {
    const auto [owner, inserted] = owners.try_emplace(snapshot.instances[row].instance_name, InstanceOwnerInfo{row});
    static_cast<void>(inserted);
    ++owner->second.count;
  }
  return owners;
}

std::vector<std::vector<std::size_t>> indexNodeRowsByInstance(const CsrGraphSnapshot& snapshot,
                                                              const std::unordered_map<std::string, InstanceOwnerInfo>& owners)
{
  std::vector<std::vector<std::size_t>> rows(snapshot.instances.size());
  for (std::size_t row = 0; row < snapshot.nodes.size(); ++row) {
    const auto& key = snapshot.nodes[row].key;
    if (key.kind != NodeKind::kInstancePin)
      continue;
    const auto owner = owners.find(key.instance_name);
    if (owner != owners.end() && owner->second.count == 1)
      rows[owner->second.first_row].push_back(row);
  }
  return rows;
}

void validateInstanceStage(const CsrGraphSnapshot& snapshot, Reporter& reporter)
{
  const auto owners = indexInstanceOwners(snapshot);
  const auto node_rows_by_instance = indexNodeRowsByInstance(snapshot, owners);
  std::vector<std::size_t> seen_in_instance(snapshot.nodes.size(), snapshot.instances.size());
  for (std::size_t index = 0; index < snapshot.instances.size(); ++index) {
    const auto& instance = snapshot.instances[index];
    if (instance.instance_name.empty())
      reporter.malformed(indexed("instances", index, "instance_name"), "instance name is empty");
    if (instance.cell_name.empty())
      reporter.malformed(indexed("instances", index, "cell_name"), "cell name is empty");
    if (index > 0 && snapshot.instances[index - 1].instance_name >= instance.instance_name)
      reporter.malformed(indexed("instances", index, "instance_name"), "instances must preserve unique native map-name order");

    std::size_t clock_roles = 0;
    std::size_t data_roles = 0;
    std::size_t output_roles = 0;
    for (std::size_t pin_index = 0; pin_index < instance.ordered_pins.size(); ++pin_index) {
      const NodeId node_id = instance.ordered_pins[pin_index];
      if (!validNodeId(node_id, snapshot.nodes.size())) {
        reporter.malformed(indexed("instances", index, "ordered_pins"), "ordered pin contains an unknown NodeId");
        continue;
      }
      const std::size_t row = static_cast<std::size_t>(node_id.value());
      if (seen_in_instance[row] == index)
        reporter.malformed(indexed("instances", index, "ordered_pins"), "ordered pin list contains a duplicate NodeId");
      seen_in_instance[row] = index;
      const auto& node = snapshot.nodes[row];
      if (node.key.kind != NodeKind::kInstancePin || node.key.instance_name != instance.instance_name)
        reporter.malformed(indexed("instances", index, "ordered_pins"), "ordered pin does not belong to this instance");
      if (node.instance_cell != instance.cell_name)
        reporter.malformed(indexed("instances", index, "cell_name"), "instance pin cell name disagrees with its instance");
      if (node.is_sequential != instance.is_sequential)
        reporter.malformed(indexed("nodes", row, "is_sequential"), "instance-pin sequential role disagrees with its instance");
      clock_roles += node.is_clock_pin ? 1 : 0;
      data_roles += node.is_data_pin ? 1 : 0;
      output_roles += node.is_output_pin ? 1 : 0;
    }
    if (clock_roles > 1 || data_roles > 1 || output_roles > 1)
      reporter.malformed(indexed("instances", index, "ordered_pins"), "clock, data, and output roles must each identify at most one ordered pin");

    for (std::size_t row : node_rows_by_instance[index]) {
      if (seen_in_instance[row] != index)
        reporter.malformed(indexed("instances", index, "ordered_pins"), "instance pin is missing from the ordered pin list");
    }
  }

  for (std::size_t row = 0; row < snapshot.nodes.size(); ++row) {
    const auto& key = snapshot.nodes[row].key;
    if (key.kind != NodeKind::kInstancePin)
      continue;
    const auto owner = owners.find(key.instance_name);
    if (owner == owners.end() || owner->second.count != 1)
      reporter.malformed(indexed("nodes", row, "key.instance_name"), "instance pin must reference exactly one instance");
  }
}

void validateArcStage(const CsrGraphSnapshot& snapshot, Reporter& reporter)
{
  bool all_keys_valid = true;
  bool ranks_valid = true;
  std::vector<bool> ranks(snapshot.arcs.size(), false);
  std::vector<std::size_t> arc_indices_by_rank(snapshot.arcs.size(), 0);
  for (std::size_t index = 0; index < snapshot.arcs.size(); ++index) {
    const auto& arc = snapshot.arcs[index];
    if (arc.id != ArcId{index})
      reporter.malformed(indexed("arcs", index, "id"), "arc ID must equal its canonical row index");
    if (auto error = validateArcKeyBase(arc.key.base)) {
      reporter.malformed(indexed("arcs", index, "key"), *error);
      all_keys_valid = false;
    }
    if (!hasKnownToken(arc.kind))
      reporter.malformed(indexed("arcs", index, "kind"), "arc kind is unknown");
    if (arc.kind != arc.key.base.kind)
      reporter.malformed(indexed("arcs", index, "kind"), "record kind must agree with ArcKey kind");
    if (!validNodeId(arc.source, snapshot.nodes.size()) || !validNodeId(arc.sink, snapshot.nodes.size())) {
      reporter.malformed(indexed("arcs", index, "endpoints"), "arc endpoint contains an unknown NodeId");
    } else {
      if (snapshot.nodes[arc.source.value()].key != arc.key.base.source)
        reporter.malformed(indexed("arcs", index, "source"), "source NodeId does not resolve to ArcKey source");
      if (snapshot.nodes[arc.sink.value()].key != arc.key.base.sink)
        reporter.malformed(indexed("arcs", index, "sink"), "sink NodeId does not resolve to ArcKey sink");
      const auto& source = snapshot.nodes[arc.source.value()];
      const auto& sink = snapshot.nodes[arc.sink.value()];
      if (arc.kind == ArcKind::kCell
          && (source.key.kind != NodeKind::kInstancePin || sink.key.kind != NodeKind::kInstancePin || source.key.instance_name != sink.key.instance_name
              || arc.key.base.owner_name != source.key.instance_name || arc.key.base.library_source_port != source.key.pin_name
              || arc.key.base.library_sink_port != sink.key.pin_name)) {
        reporter.malformed(indexed("arcs", index, "key"), "cell ArcKey owner and library ports must match same-instance endpoint pins");
      }
      if (arc.kind == ArcKind::kNet && (source.net_name.empty() || source.net_name != sink.net_name || arc.key.base.owner_name != source.net_name)) {
        reporter.malformed(indexed("arcs", index, "key"), "net ArcKey owner must match the endpoints' common nonempty net");
      }
    }
    if (arc.native_traversal_rank >= snapshot.arcs.size()) {
      reporter.malformed(indexed("arcs", index, "native_traversal_rank"), "native traversal rank is out of range");
      ranks_valid = false;
    } else {
      const std::size_t rank = static_cast<std::size_t>(arc.native_traversal_rank);
      if (ranks[rank]) {
        reporter.malformed(indexed("arcs", index, "native_traversal_rank"), "native traversal rank is duplicated");
        ranks_valid = false;
      }
      ranks[rank] = true;
      arc_indices_by_rank[rank] = index;
    }
  }

  if (all_keys_valid && ranks_valid) {
    std::vector<ArcKeyBase> bases;
    bases.reserve(arc_indices_by_rank.size());
    for (std::size_t index : arc_indices_by_rank)
      bases.push_back(snapshot.arcs[index].key.base);
    const auto expected = assignParallelOrdinals(bases);
    for (std::size_t rank = 0; rank < expected.size(); ++rank) {
      const std::size_t index = arc_indices_by_rank[rank];
      if (snapshot.arcs[index].key.parallel_ordinal != expected[rank].parallel_ordinal) {
        reporter.malformed(indexed("arcs", index, "key.parallel_ordinal"),
                           "parallel ordinal must be assigned within its ArcKeyBase class in native traversal order");
      }
    }
  }

  if (all_keys_valid) {
    for (std::size_t index = 1; index < snapshot.arcs.size(); ++index) {
      const auto& previous = snapshot.arcs[index - 1].key;
      const auto& current = snapshot.arcs[index].key;
      if (previous == current) {
        reporter.collision(indexed("arcs", index, "key"), "canonical ArcKey collides with the previous row");
      } else if (!arcKeyLess(previous, current)) {
        reporter.malformed(indexed("arcs", index, "key"), "arcs are not in canonical ArcKey order");
      }
    }
  }

  if (snapshot.arc_ids_by_key.size() != snapshot.arcs.size())
    reporter.malformed("arc_ids_by_key", "lookup size must equal arc count");
  const std::size_t comparable = std::min(snapshot.arc_ids_by_key.size(), snapshot.arcs.size());
  for (std::size_t index = 0; index < comparable; ++index) {
    const auto& lookup = snapshot.arc_ids_by_key[index];
    if (lookup.id != ArcId{index} || lookup.key != snapshot.arcs[index].key)
      reporter.malformed(indexed("arc_ids_by_key", index, "entry"), "lookup must be the exact canonical key-to-ID inverse");
  }
}

void validateAdjacencyFamily(const CsrGraphSnapshot& snapshot, const std::vector<std::uint64_t>& offsets, const std::vector<ArcId>& ids, bool outgoing,
                             std::string_view family, Reporter& reporter)
{
  const std::size_t node_count = snapshot.nodes.size();
  const std::size_t arc_count = snapshot.arcs.size();
  if (offsets.size() != node_count + 1) {
    reporter.malformed(std::string(family) + "_row_offsets", "row-offset size must be node count plus one");
    return;
  }
  bool shape_valid = true;
  if (offsets.front() != 0) {
    reporter.malformed(std::string(family) + "_row_offsets[0]", "first row offset must be zero");
    shape_valid = false;
  }
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    if (offsets[index] > static_cast<std::uint64_t>(ids.size())) {
      reporter.malformed(indexed(std::string(family) + "_row_offsets", index, "value"), "row offset exceeds adjacency entry count");
      shape_valid = false;
    }
    if (index > 0 && offsets[index - 1] > offsets[index]) {
      reporter.malformed(indexed(std::string(family) + "_row_offsets", index, "value"), "row offsets must be nondecreasing");
      shape_valid = false;
    }
  }
  if (offsets.back() != ids.size()) {
    reporter.malformed(std::string(family) + "_row_offsets", "terminal row offset must equal adjacency entry count");
    shape_valid = false;
  }
  if (!shape_valid)
    return;

  std::vector<std::uint64_t> occurrences(arc_count, 0);
  for (std::size_t row = 0; row < node_count; ++row) {
    for (std::uint64_t position = offsets[row]; position < offsets[row + 1]; ++position) {
      const ArcId arc_id = ids[static_cast<std::size_t>(position)];
      if (!validArcId(arc_id, arc_count)) {
        reporter.malformed(indexed(std::string(family) + "_arc_ids", static_cast<std::size_t>(position), "id"), "adjacency contains an unknown ArcId");
        continue;
      }
      const std::size_t arc_row = static_cast<std::size_t>(arc_id.value());
      ++occurrences[arc_row];
      const NodeId endpoint = outgoing ? snapshot.arcs[arc_row].source : snapshot.arcs[arc_row].sink;
      if (endpoint != NodeId{row})
        reporter.malformed(indexed(std::string(family) + "_arc_ids", static_cast<std::size_t>(position), "row"), "adjacency arc is in the wrong endpoint row");
    }
  }
  for (std::size_t arc = 0; arc < arc_count; ++arc) {
    if (occurrences[arc] != 1)
      reporter.malformed(indexed(std::string(family) + "_arc_ids", arc, "occurrences"), "every arc must occur exactly once");
  }
}

void validateOrderedList(const CsrGraphSnapshot& snapshot, const std::vector<NodeId>& ids, std::string_view field, Reporter& reporter)
{
  std::vector<bool> seen(snapshot.nodes.size(), false);
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (!validNodeId(ids[index], snapshot.nodes.size())) {
      reporter.malformed(indexed(field, index, "id"), "ordered collection contains an unknown NodeId");
      continue;
    }
    const std::size_t row = static_cast<std::size_t>(ids[index].value());
    if (seen[row])
      reporter.malformed(indexed(field, index, "id"), "ordered collection contains a duplicate NodeId");
    seen[row] = true;
  }
}

void validateRoleCollection(const CsrGraphSnapshot& snapshot, const std::vector<NodeId>& ids, bool NodeRecord::* role, std::string_view field,
                            Reporter& reporter)
{
  std::vector<bool> listed(snapshot.nodes.size(), false);
  for (NodeId id : ids) {
    if (validNodeId(id, snapshot.nodes.size()))
      listed[static_cast<std::size_t>(id.value())] = true;
  }
  for (std::size_t row = 0; row < snapshot.nodes.size(); ++row) {
    if (listed[row] != snapshot.nodes[row].*role)
      reporter.malformed(std::string(field), "ordered collection must exactly match node role membership");
  }
}

void validateOrderedStage(const CsrGraphSnapshot& snapshot, Reporter& reporter)
{
  validateOrderedList(snapshot, snapshot.ordered_root_ids, "ordered_root_ids", reporter);
  validateOrderedList(snapshot, snapshot.ordered_start_ids, "ordered_start_ids", reporter);
  validateOrderedList(snapshot, snapshot.ordered_end_ids, "ordered_end_ids", reporter);
  validateRoleCollection(snapshot, snapshot.ordered_start_ids, &NodeRecord::is_startpoint, "ordered_start_ids", reporter);
  validateRoleCollection(snapshot, snapshot.ordered_end_ids, &NodeRecord::is_endpoint, "ordered_end_ids", reporter);

  std::vector<std::uint64_t> active_indegree(snapshot.nodes.size(), 0);
  for (const auto& arc : snapshot.arcs) {
    if (!arc.is_disabled && !arc.is_loop_disabled && validNodeId(arc.sink, snapshot.nodes.size()))
      ++active_indegree[static_cast<std::size_t>(arc.sink.value())];
  }
  std::vector<NodeId> expected_roots;
  for (std::size_t row = 0; row < snapshot.nodes.size(); ++row) {
    if (active_indegree[row] == 0)
      expected_roots.push_back(NodeId{row});
  }
  std::ranges::sort(expected_roots,
                    [&](NodeId lhs, NodeId rhs) { return snapshot.nodes[lhs.value()].key.full_name < snapshot.nodes[rhs.value()].key.full_name; });
  if (snapshot.ordered_root_ids != expected_roots)
    reporter.malformed("ordered_root_ids", "roots must be active-indegree-zero nodes in native timing-point map-name order");
}

}  // namespace

std::vector<CsrGraphContractViolation> validateCsrGraphSnapshot(const CsrGraphSnapshot& snapshot)
{
  Reporter reporter;
  if (snapshot.schema != kCsrGraphSnapshotSchema)
    reporter.malformed("schema", "schema must equal ista-csr-graph-snapshot/1.0.0");
  validateNodeStage(snapshot, reporter);
  validateInstanceStage(snapshot, reporter);
  validateArcStage(snapshot, reporter);
  validateAdjacencyFamily(snapshot, snapshot.out_row_offsets, snapshot.out_arc_ids, true, "out", reporter);
  validateAdjacencyFamily(snapshot, snapshot.in_row_offsets, snapshot.in_arc_ids, false, "in", reporter);
  validateOrderedStage(snapshot, reporter);
  return reporter.take();
}

}  // namespace ista::csr
