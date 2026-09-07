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
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::csr {
namespace {
#if ECC_INCREGPUSTA_DEBUG_CHECKS
thread_local std::uint64_t full_levelizer_entry_count = 0;

void recordFullLevelizerEntry()
{
  if (full_levelizer_entry_count == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("full levelizer entry counter overflow");
  ++full_levelizer_entry_count;
}
#endif

}  // namespace

CsrFullLevelizerEntryDelta::CsrFullLevelizerEntryDelta(CsrFullLevelizerEntryDelta&& other) noexcept
    : _value(std::exchange(other._value, 0))
{
}

CsrFullLevelizerOperationGuard::CsrFullLevelizerOperationGuard(CsrFullLevelizerOperationGuard&& other) noexcept
    : _counter(std::exchange(other._counter, nullptr)), _start_count(other._start_count)
{
}

#if ECC_INCREGPUSTA_DEBUG_CHECKS
CsrFullLevelizerEntryDelta CsrFullLevelizerOperationGuard::finish() &&
{
  if (_counter == nullptr || _counter != &full_levelizer_entry_count)
    throw std::logic_error("full levelizer operation guard finished outside its originating thread or more than once");
  if (*_counter < _start_count)
    throw std::logic_error("full levelizer entry counter regressed");
  const std::uint64_t delta = *_counter - _start_count;
  _counter = nullptr;
  return CsrFullLevelizerEntryDelta(delta);
}

CsrFullLevelizerOperationGuard beginCsrFullLevelizerOperation() noexcept
{
  return CsrFullLevelizerOperationGuard(&full_levelizer_entry_count, full_levelizer_entry_count);
}

#endif

CsrFifoLevelization replayFifoLevelization(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads, CsrTraversalTelemetry* telemetry)
{
#if ECC_INCREGPUSTA_DEBUG_CHECKS
  recordFullLevelizerEntry();
#endif
  CsrFifoLevelization replay;
  if (edges.nodeCount() != payloads.nodeCount()) {
    replay.view_error = CsrViewError::kMalformedAdjacency;
    return replay;
  }
  replay.node_to_level.assign(edges.nodeCount(), 0);

  std::vector<std::uint64_t> indegree(edges.nodeCount(), 0);
  for (std::size_t index = 0; index < edges.nodeCount(); ++index) {
    const EdgeVisitResult visit = edges.forEachIncoming(NodeId{index}, [&](const EdgeEntry& entry) {
      if (telemetry != nullptr)
        recordStorageVisit(telemetry->levelization, entry.storage_kind);
      const auto payload = edges.arc(entry.arc_id);
      if (!payload || payload.value().sink != NodeId{index})
        return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
      if (payload.value().eligibility.levelization)
        ++indegree[index];
      return EdgeVisitResult::continueVisit();
    });
    if (visit.control == EdgeVisitControl::kError) {
      replay.view_error = visit.error.value_or(CsrViewError::kCallbackError);
      return replay;
    }
  }

  std::queue<NodeId> ready;
  std::vector<std::uint8_t> declared_root(edges.nodeCount(), 0);
  for (NodeId root : payloads.orderedRootIds()) {
    if (root.value() >= edges.nodeCount()) {
      replay.view_error = CsrViewError::kNodeOutOfRange;
      return replay;
    }
    if (indegree[root.value()] != 0
        || declared_root[root.value()]++ != 0) {
      replay.view_error = CsrViewError::kMalformedAdjacency;
      return replay;
    }
    replay.node_to_level[root.value()] = 1;
    ready.push(root);
  }
  for (std::size_t node = 0; node < edges.nodeCount(); ++node)
    if ((indegree[node] == 0) != (declared_root[node] != 0)) {
      replay.view_error = CsrViewError::kMalformedAdjacency;
      return replay;
    }

  while (!ready.empty()) {
    const NodeId node = ready.front();
    ready.pop();
    replay.level_nodes.push_back(node);
    const EdgeVisitResult visit = edges.forEachOutgoing(node, [&](const EdgeEntry& entry) {
      if (telemetry != nullptr)
        recordStorageVisit(telemetry->levelization, entry.storage_kind);
      const auto payload = edges.arc(entry.arc_id);
      if (!payload || payload.value().source != node || payload.value().sink.value() >= edges.nodeCount())
        return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
      const ArcSemanticPayload& arc = payload.value();
      if (!arc.eligibility.levelization)
        return EdgeVisitResult::continueVisit();
      const std::uint64_t candidate = replay.node_to_level[node.value()] + 1;
      replay.node_to_level[arc.sink.value()] = std::max(replay.node_to_level[arc.sink.value()], candidate);
      if (indegree[arc.sink.value()] == 0)
        return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
      if (--indegree[arc.sink.value()] == 0)
        ready.push(arc.sink);
      return EdgeVisitResult::continueVisit();
    });
    if (visit.control == EdgeVisitControl::kError) {
      replay.view_error = visit.error.value_or(CsrViewError::kCallbackError);
      return replay;
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
  replay.complete = replay.level_nodes.size() == edges.nodeCount();
  return replay;
}

CsrFifoLevelization replayFifoLevelization(const CsrReferenceInput& input)
{
  FlatCsrEdgeView edges(input);
  FlatTimingPayloadView payloads(input);
  return replayFifoLevelization(edges, payloads, nullptr);
}

}  // namespace ista::csr
