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
#include <map>
#include <string>
#include <vector>

#include "TimingGraphCsrAdapterInternal.hpp"

namespace ista::csr::detail {
namespace {

void validateAdjacencyKeys(const GraphBuildState& state, const std::map<std::string, std::vector<std::size_t>>& native_rows)
{
  for (const auto& [name, row] : native_rows) {
    static_cast<void>(row);
    if (state.node_ids_by_full_name.find(name) == state.node_ids_by_full_name.end())
      malformed();
  }
}

void exportAdjacencyFamily(const GraphBuildState& state, const std::map<std::string, std::vector<std::size_t>>& native_rows, bool outgoing,
                           std::vector<std::uint64_t>& offsets, std::vector<ArcId>& ids)
{
  validateAdjacencyKeys(state, native_rows);
  const auto& native_arcs = state.database.get_arc_list();
  offsets.clear();
  ids.clear();
  offsets.reserve(state.snapshot.nodes.size() + 1);
  offsets.push_back(0);
  std::vector<std::uint64_t> occurrences(native_arcs.size(), 0);

  for (const auto& node : state.snapshot.nodes) {
    const auto row = native_rows.find(node.key.full_name);
    if (row != native_rows.end()) {
      for (std::size_t native_arc : row->second) {
        if (native_arc >= native_arcs.size() || native_arc >= state.arc_ids_by_native_rank.size())
          malformed();
        const auto& arc = native_arcs.at(native_arc);
        const std::string& endpoint = outgoing ? arc.get_source_pin() : arc.get_sink_pin();
        if (endpoint != node.key.full_name)
          malformed();
        ++occurrences[native_arc];
        ids.push_back(state.arc_ids_by_native_rank[native_arc]);
      }
    }
    offsets.push_back(static_cast<std::uint64_t>(ids.size()));
  }

  for (std::uint64_t count : occurrences) {
    if (count != 1)
      malformed();
  }
}

NodeId requireOrderedNode(const GraphBuildState& state, const std::string& name)
{
  const auto node = state.node_ids_by_full_name.find(name);
  if (node == state.node_ids_by_full_name.end())
    malformed();
  return node->second;
}

void exportOrderedCollections(GraphBuildState& state)
{
  std::vector<std::uint64_t> active_indegree(state.snapshot.nodes.size(), 0);
  for (const auto& arc : state.snapshot.arcs) {
    if (!arc.is_disabled && !arc.is_loop_disabled) {
      if (arc.sink.value() >= active_indegree.size())
        malformed();
      ++active_indegree[static_cast<std::size_t>(arc.sink.value())];
    }
  }

  for (const auto& [name, point] : state.database.get_timing_point_map()) {
    static_cast<void>(point);
    const NodeId node = requireOrderedNode(state, name);
    if (active_indegree[static_cast<std::size_t>(node.value())] == 0)
      state.snapshot.ordered_root_ids.push_back(node);
  }
  for (const auto& name : state.database.get_start_point_list())
    state.snapshot.ordered_start_ids.push_back(requireOrderedNode(state, name));
  for (const auto& name : state.database.get_end_point_list())
    state.snapshot.ordered_end_ids.push_back(requireOrderedNode(state, name));
}

}  // namespace

void exportAdjacencyAndOrder(GraphBuildState& state)
{
  exportAdjacencyFamily(state, state.database.get_outgoing_arc_list_map(), true, state.snapshot.out_row_offsets, state.snapshot.out_arc_ids);
  exportAdjacencyFamily(state, state.database.get_incoming_arc_list_map(), false, state.snapshot.in_row_offsets, state.snapshot.in_arc_ids);
  exportOrderedCollections(state);
}

}  // namespace ista::csr::detail
