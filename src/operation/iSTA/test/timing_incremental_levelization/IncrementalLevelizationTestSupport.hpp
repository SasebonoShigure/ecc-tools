// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CpuIncrementalLevelizationPipeline.hpp"
#include "MixedLevelizationGraphBuilder.hpp"

namespace ista::timing_incremental_levelization::test {

inline void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

struct ArcSpec
{
  std::uint64_t source{0};
  std::uint64_t sink{0};
  bool levelization{true};
  csr::EdgeStorageKind storage{csr::EdgeStorageKind::kBaseOut};
  bool active{true};
};

class TestEdgeView final : public csr::ActiveCsrEdgeView
{
 public:
  explicit TestEdgeView(std::size_t node_count) : _node_count(node_count) {}

  csr::ArcId addArc(const ArcSpec& spec)
  {
    csr::ArcSemanticPayload payload;
    payload.source = csr::NodeId{spec.source};
    payload.sink = csr::NodeId{spec.sink};
    payload.eligibility.levelization = spec.levelization;
    _payloads.push_back(std::move(payload));
    _storage.push_back(spec.storage);
    _active.push_back(spec.active);
    return csr::ArcId{static_cast<std::uint64_t>(_payloads.size() - 1)};
  }

  void failActiveTraversal(csr::CsrViewError error)
  {
    _active_error = error;
  }

  [[nodiscard]] std::size_t nodeCount() const noexcept override
  {
    return _node_count;
  }
  [[nodiscard]] std::size_t arcSlotCount() const noexcept override
  {
    return _payloads.size();
  }
  [[nodiscard]] std::size_t activeArcCount() const noexcept override
  {
    return static_cast<std::size_t>(
        std::count(_active.begin(), _active.end(), true));
  }
  [[nodiscard]] bool isActive(csr::ArcId id) const noexcept override
  {
    return id.value() < _active.size()
           && _active[static_cast<std::size_t>(id.value())];
  }
  [[nodiscard]] csr::ExpectedRef<const csr::ArcSemanticPayload,
                                 csr::CsrViewError>
  arc(csr::ArcId id) const noexcept override
  {
    if (id.value() >= _payloads.size())
      return csr::ExpectedRef<const csr::ArcSemanticPayload,
                              csr::CsrViewError>::failure(
          csr::CsrViewError::kArcOutOfRange);
    if (!isActive(id))
      return csr::ExpectedRef<const csr::ArcSemanticPayload,
                              csr::CsrViewError>::failure(
          csr::CsrViewError::kInactiveArc);
    return csr::ExpectedRef<const csr::ArcSemanticPayload,
                            csr::CsrViewError>::success(
        _payloads[static_cast<std::size_t>(id.value())]);
  }
  [[nodiscard]] csr::EdgeVisitResult forEachActiveArc(
      const csr::EdgeVisitor& visitor) const override
  {
    if (_active_error.has_value())
      return csr::EdgeVisitResult::fail(*_active_error);
    for (std::size_t index = 0; index < _payloads.size(); ++index) {
      if (!_active[index])
        continue;
      const csr::EdgeVisitResult result = visitor(
          {csr::ArcId{static_cast<std::uint64_t>(index)}, _storage[index]});
      if (result.control != csr::EdgeVisitControl::kContinue)
        return result;
    }
    return csr::EdgeVisitResult::continueVisit();
  }
  [[nodiscard]] csr::EdgeVisitResult forEachOutgoing(
      csr::NodeId node, const csr::EdgeVisitor& visitor) const override
  {
    if (node.value() >= _node_count)
      return csr::EdgeVisitResult::fail(csr::CsrViewError::kNodeOutOfRange);
    for (std::size_t index = 0; index < _payloads.size(); ++index) {
      if (!_active[index] || _payloads[index].source != node)
        continue;
      const csr::EdgeVisitResult result = visitor(
          {csr::ArcId{static_cast<std::uint64_t>(index)}, _storage[index]});
      if (result.control != csr::EdgeVisitControl::kContinue)
        return result;
    }
    return csr::EdgeVisitResult::continueVisit();
  }
  [[nodiscard]] csr::EdgeVisitResult forEachIncoming(
      csr::NodeId node, const csr::EdgeVisitor& visitor) const override
  {
    if (node.value() >= _node_count)
      return csr::EdgeVisitResult::fail(csr::CsrViewError::kNodeOutOfRange);
    for (std::size_t index = 0; index < _payloads.size(); ++index) {
      if (!_active[index] || _payloads[index].sink != node)
        continue;
      const csr::EdgeVisitResult result = visitor(
          {csr::ArcId{static_cast<std::uint64_t>(index)},
           csr::EdgeStorageKind::kBaseIn});
      if (result.control != csr::EdgeVisitControl::kContinue)
        return result;
    }
    return csr::EdgeVisitResult::continueVisit();
  }

 private:
  std::size_t _node_count{0};
  std::vector<csr::ArcSemanticPayload> _payloads;
  std::vector<csr::EdgeStorageKind> _storage;
  std::vector<bool> _active;
  std::optional<csr::CsrViewError> _active_error;
};

inline csr::CsrFifoLevelization makeLevels(
    std::size_t node_count,
    const std::vector<std::vector<std::uint64_t>>& by_level)
{
  csr::CsrFifoLevelization levels;
  levels.level_offsets.clear();
  levels.level_offsets.push_back(0);
  levels.node_to_level.assign(node_count, 0);
  for (std::size_t level = 0; level < by_level.size(); ++level) {
    for (std::uint64_t node : by_level[level]) {
      levels.level_nodes.emplace_back(node);
      if (node < node_count)
        levels.node_to_level[static_cast<std::size_t>(node)] =
            static_cast<std::uint64_t>(level) + 1;
    }
    levels.level_offsets.push_back(
        static_cast<std::uint64_t>(levels.level_nodes.size()));
  }
  levels.complete = levels.level_nodes.size() == node_count;
  return levels;
}

inline void requireIndependentLevelContract(
    const TestEdgeView& edges, const csr::CsrFifoLevelization& levels)
{
  require(levels.complete && !levels.view_error,
          "levelization is not a sealed success");
  require(!levels.level_offsets.empty()
              && levels.level_offsets.front() == 0
              && levels.level_offsets.back() == edges.nodeCount(),
          "level offsets do not cover actual nodes");
  require(levels.level_nodes.size() == edges.nodeCount()
              && levels.node_to_level.size() == edges.nodeCount(),
          "permutation or inverse has the wrong size");
  std::vector<std::uint8_t> seen(edges.nodeCount(), 0);
  for (std::size_t level = 0; level + 1 < levels.level_offsets.size();
       ++level) {
    const std::uint64_t begin = levels.level_offsets[level];
    const std::uint64_t end = levels.level_offsets[level + 1];
    require(begin < end, "successful levelization contains an empty level");
    for (std::uint64_t position = begin; position < end; ++position) {
      const csr::NodeId node =
          levels.level_nodes[static_cast<std::size_t>(position)];
      require(node.value() < edges.nodeCount(), "level node is out of range");
      require(seen[static_cast<std::size_t>(node.value())]++ == 0,
              "level node is duplicated");
      require(levels.node_to_level[static_cast<std::size_t>(node.value())]
                  == static_cast<std::uint64_t>(level) + 1,
              "inverse level map disagrees with offsets");
    }
  }
  const csr::EdgeVisitResult visit =
      edges.forEachActiveArc([&](const csr::EdgeEntry& entry) {
        const auto payload = edges.arc(entry.arc_id);
        if (!payload)
          return csr::EdgeVisitResult::fail(payload.error());
        const auto& arc = payload.value();
        if (arc.eligibility.levelization
            && levels.node_to_level[arc.source.value()]
                   >= levels.node_to_level[arc.sink.value()])
          return csr::EdgeVisitResult::fail(
              csr::CsrViewError::kMalformedAdjacency);
        return csr::EdgeVisitResult::continueVisit();
      });
  require(visit.control == csr::EdgeVisitControl::kContinue,
          "eligible actual edge does not strictly cross levels");
}

inline MixedLevelizationSuccess requirePipelineSuccess(
    const TestEdgeView& edges, const csr::CsrFifoLevelization& previous,
    std::uint64_t n_last_nodes, std::uint64_t generation = 1)
{
  const auto attempt = CpuIncrementalLevelizationPipeline::run(
      edges, previous,
      {n_last_nodes, generation - 1, generation});
  const auto* success = std::get_if<MixedLevelizationSuccess>(&attempt);
  require(success != nullptr, "mixed pipeline unexpectedly failed");
  requireIndependentLevelContract(edges, success->levels);
  return *success;
}

}  // namespace ista::timing_incremental_levelization::test
