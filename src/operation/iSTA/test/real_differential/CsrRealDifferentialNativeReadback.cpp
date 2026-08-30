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
#include "CsrRealDifferentialNativeReadback.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Database.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::real_diff {
namespace {

NativeReadbackKeys buildKeys(const CsrReferenceInput& input)
{
  NativeReadbackKeys keys;
  for (std::size_t index = 0; index < input.nodes.size(); ++index) {
    const NodeRecord& node = input.nodes[index];
    if (node.id.value() != index) {
      throw std::runtime_error("reference native readback: noncanonical NodeId record");
    }
    if (!keys.node_keys.emplace(node.key.full_name, node.key).second) {
      throw std::runtime_error("reference native readback: duplicate node full name");
    }
  }

  keys.arc_keys_by_rank.resize(input.arcs.size());
  std::vector<bool> seen(input.arcs.size(), false);
  for (std::size_t index = 0; index < input.arcs.size(); ++index) {
    const ArcRecord& arc = input.arcs[index];
    if (arc.id.value() != index || arc.native_traversal_rank >= input.arcs.size()) {
      throw std::runtime_error("reference native readback: invalid ArcId/native rank record");
    }
    const std::size_t rank = static_cast<std::size_t>(arc.native_traversal_rank);
    if (seen[rank]) {
      throw std::runtime_error("reference native readback: duplicate native Arc rank");
    }
    seen[rank] = true;
    keys.arc_keys_by_rank[rank] = arc.key;
  }
  if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
    throw std::runtime_error("reference native readback: incomplete native Arc ranks");
  }
  return keys;
}

NeutralAdjacencyRow readRow(const NodeKey& node, const std::map<std::string, std::vector<std::size_t>>& rows, const NativeReadbackKeys& keys)
{
  NeutralAdjacencyRow row;
  row.node = node;
  const auto found = rows.find(node.full_name);
  if (found == rows.end()) {
    return row;
  }
  row.arcs.reserve(found->second.size());
  for (std::size_t native_rank : found->second) {
    row.arcs.push_back(requireNativeArcKey(keys, native_rank));
  }
  return row;
}

}  // namespace

const NodeKey& requireNativeNodeKey(const NativeReadbackKeys& keys, std::string_view full_name)
{
  const auto found = keys.node_keys.find(full_name);
  if (found == keys.node_keys.end()) {
    throw std::runtime_error("reference native readback: stale/unknown node reference");
  }
  return found->second;
}

const ArcKey& requireNativeArcKey(const NativeReadbackKeys& keys, std::size_t native_rank)
{
  if (native_rank >= keys.arc_keys_by_rank.size()) {
    throw std::runtime_error("reference native readback: Arc native rank is out of range");
  }
  return keys.arc_keys_by_rank[native_rank];
}

NeutralWire readNativeTiming(const Database& database, const CsrReferenceInput& input)
{
  const NativeReadbackKeys keys = buildKeys(input);
  if (database.get_timing_point_map().size() != input.nodes.size() || database.get_arc_list().size() != input.arcs.size()) {
    throw std::runtime_error("reference native readback: Database/input cardinality mismatch");
  }

  NeutralWire wire;
  wire.timing_order.reserve(database.get_timing_order_list().size());
  for (const std::string& full_name : database.get_timing_order_list()) {
    wire.timing_order.push_back(requireNativeNodeKey(keys, full_name));
  }

  wire.out_rows.reserve(input.nodes.size());
  wire.in_rows.reserve(input.nodes.size());
  for (const NodeRecord& node : input.nodes) {
    wire.out_rows.push_back(readRow(node.key, database.get_outgoing_arc_list_map(), keys));
    wire.in_rows.push_back(readRow(node.key, database.get_incoming_arc_list_map(), keys));
  }

  wire.nodes.reserve(database.get_timing_point_map().size());
  for (const auto& [full_name, point] : database.get_timing_point_map()) {
    wire.nodes.push_back(readNativeTimingNode(full_name, point, keys));
  }
  std::sort(wire.nodes.begin(), wire.nodes.end(), [](const NeutralNode& lhs, const NeutralNode& rhs) { return nodeKeyLess(lhs.key, rhs.key); });

  wire.arcs.reserve(database.get_arc_list().size());
  for (std::size_t native_rank = 0; native_rank < database.get_arc_list().size(); ++native_rank) {
    wire.arcs.push_back(readNativeTimingArc(database.get_arc_list()[native_rank], native_rank, keys));
  }
  std::sort(wire.arcs.begin(), wire.arcs.end(), [](const NeutralArc& lhs, const NeutralArc& rhs) { return arcKeyLess(lhs.key, rhs.key); });
  return wire;
}

}  // namespace ista::csr::real_diff
