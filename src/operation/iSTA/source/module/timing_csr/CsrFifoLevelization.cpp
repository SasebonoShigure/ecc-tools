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
#include "CsrFifoLevelization.hpp"

#include <algorithm>
#include <queue>

namespace ista::csr {

CsrFifoLevelization replayFifoLevelization(const CsrReferenceInput& input)
{
  CsrFifoLevelization replay;
  replay.node_to_level.assign(input.nodes.size(), 0);

  std::vector<std::uint64_t> indegree(input.nodes.size(), 0);
  for (const auto& arc : input.arcs)
    if (arc.eligibility.levelization)
      ++indegree[arc.sink.value()];

  std::queue<NodeId> ready;
  for (NodeId root : input.ordered_root_ids) {
    replay.node_to_level[root.value()] = 1;
    ready.push(root);
  }

  while (!ready.empty()) {
    const NodeId node = ready.front();
    ready.pop();
    replay.level_nodes.push_back(node);
    for (std::uint64_t offset = input.out_row_offsets[node.value()]; offset < input.out_row_offsets[node.value() + 1]; ++offset) {
      const auto& arc = input.arcs[input.out_arc_ids[offset].value()];
      if (!arc.eligibility.levelization)
        continue;
      const std::uint64_t candidate = replay.node_to_level[node.value()] + 1;
      replay.node_to_level[arc.sink.value()] = std::max(replay.node_to_level[arc.sink.value()], candidate);
      if (--indegree[arc.sink.value()] == 0)
        ready.push(arc.sink);
    }
  }

  if (!replay.level_nodes.empty()) {
    std::uint64_t level = replay.node_to_level[replay.level_nodes.front().value()];
    for (std::size_t index = 1; index < replay.level_nodes.size(); ++index) {
      const std::uint64_t next = replay.node_to_level[replay.level_nodes[index].value()];
      if (next != level) {
        replay.level_offsets.push_back(index);
        level = next;
      }
    }
    replay.level_offsets.push_back(replay.level_nodes.size());
  }
  replay.complete = replay.level_nodes.size() == input.nodes.size();
  return replay;
}

}  // namespace ista::csr
