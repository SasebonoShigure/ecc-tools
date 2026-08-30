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
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "CsrGraphValidator.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::csr {
namespace {

std::vector<NativeGraphAdjacencyRow> decodeAdjacency(const CsrGraphSnapshot& snapshot, const std::vector<std::uint64_t>& offsets, const std::vector<ArcId>& ids)
{
  std::vector<NativeGraphAdjacencyRow> result;
  result.reserve(snapshot.nodes.size());
  for (std::size_t row = 0; row < snapshot.nodes.size(); ++row) {
    NativeGraphAdjacencyRow decoded;
    decoded.node = snapshot.nodes[row].key;
    for (std::uint64_t position = offsets[row]; position < offsets[row + 1]; ++position)
      decoded.ordered_arcs.push_back(snapshot.arcs[ids[static_cast<std::size_t>(position)].value()].key);
    result.push_back(std::move(decoded));
  }
  return result;
}

std::vector<NodeKey> decodeOrderedNodes(const CsrGraphSnapshot& snapshot, const std::vector<NodeId>& ids)
{
  std::vector<NodeKey> result;
  result.reserve(ids.size());
  for (NodeId id : ids)
    result.push_back(snapshot.nodes[id.value()].key);
  return result;
}

}  // namespace

NativeGraphProjectionResult decodeNativeGraphProjection(const CsrGraphSnapshot& snapshot)
{
  const auto violations = validateCsrGraphSnapshot(snapshot);
  if (!violations.empty())
    return CsrGraphExportFailure{violations.front().code};

  NativeGraphProjection result;
  result.nodes.reserve(snapshot.nodes.size());
  for (const auto& node : snapshot.nodes) {
    result.nodes.push_back({node.key, node.direction, node.instance_cell, node.net_name, node.is_port, node.is_startpoint, node.is_endpoint, node.is_sequential,
                            node.is_clock_pin, node.is_data_pin, node.is_output_pin});
  }

  result.instances.reserve(snapshot.instances.size());
  for (const auto& instance : snapshot.instances) {
    NativeGraphInstanceProjection decoded;
    decoded.instance_name = instance.instance_name;
    decoded.cell_name = instance.cell_name;
    decoded.is_sequential = instance.is_sequential;
    decoded.has_timing_checks = instance.has_timing_checks;
    decoded.is_clock_gating = instance.is_clock_gating;
    decoded.is_macro = instance.is_macro;
    decoded.ordered_pins.reserve(instance.ordered_pins.size());
    for (NodeId id : instance.ordered_pins)
      decoded.ordered_pins.push_back(snapshot.nodes[id.value()].key);
    result.instances.push_back(std::move(decoded));
  }

  result.arcs.reserve(snapshot.arcs.size());
  for (const auto& arc : snapshot.arcs) {
    result.arcs.push_back({arc.key, arc.native_traversal_rank, arc.is_clock_arc, arc.is_disabled, arc.is_loop_disabled});
  }
  result.base_out = decodeAdjacency(snapshot, snapshot.out_row_offsets, snapshot.out_arc_ids);
  result.base_in = decodeAdjacency(snapshot, snapshot.in_row_offsets, snapshot.in_arc_ids);
  result.ordered_roots = decodeOrderedNodes(snapshot, snapshot.ordered_root_ids);
  result.ordered_starts = decodeOrderedNodes(snapshot, snapshot.ordered_start_ids);
  result.ordered_ends = decodeOrderedNodes(snapshot, snapshot.ordered_end_ids);
  return result;
}

}  // namespace ista::csr
