// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "MixedLevelizationTypes.hpp"

namespace ista::timing_incremental_levelization::detail {

// Borrow the successful owner's arrays; constructing mixed topology must not
// copy an entire previous permutation or inverse map.
struct PreviousLevels
{
  const std::vector<std::uint64_t>& level_offsets;
  const std::vector<csr::NodeId>& level_nodes;
  std::span<const std::uint64_t> node_to_level;
  std::span<const std::uint64_t> group_levels;
  bool complete;
  std::optional<csr::CsrViewError> view_error;

  PreviousLevels(const csr::CsrFifoLevelization& levels)
      : PreviousLevels(levels.level_offsets, levels.level_nodes, levels.node_to_level, levels.complete, levels.view_error)
  {
  }
  PreviousLevels(const std::vector<std::uint64_t>& offsets, const std::vector<csr::NodeId>& nodes, const std::vector<std::uint64_t>& inverse, bool done,
                 std::optional<csr::CsrViewError> error = std::nullopt)
      : level_offsets(offsets), level_nodes(nodes), node_to_level(inverse), complete(done), view_error(error)
  {
  }
  PreviousLevels(const std::vector<std::uint64_t>& offsets, const std::vector<csr::NodeId>& nodes,
                 std::span<const std::uint64_t> groups, std::span<const std::uint64_t> remap, bool done)
      : level_offsets(offsets), level_nodes(nodes), node_to_level(groups), group_levels(remap), complete(done)
  {
  }
  [[nodiscard]] std::uint64_t levelOf(std::size_t node) const noexcept
  {
    const auto value = node_to_level[node];
    return group_levels.empty() ? value : group_levels[value];
  }
};

[[nodiscard]] VirtualLevelMappingAttempt oldNodeLevel(csr::NodeId node, const PreviousLevels& previous, const MixedIdDomain& domain);

[[nodiscard]] inline bool checkedAdd(std::uint64_t lhs, std::uint64_t rhs,
                                     std::uint64_t& result) noexcept
{
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
    return false;
  result = lhs + rhs;
  return true;
}

[[nodiscard]] inline bool checkedIncrement(std::uint64_t& value) noexcept
{
  if (value == std::numeric_limits<std::uint64_t>::max())
    return false;
  ++value;
  return true;
}

[[nodiscard]] inline bool validatePreviousLevelMappingDimensions(const PreviousLevels& previous, std::uint64_t n_last_nodes, std::string& detail)
{
  if (previous.view_error.has_value()) {
    detail = "previous levelization carries a view error";
    return false;
  }
  if (!previous.complete) {
    detail = "previous levelization is incomplete";
    return false;
  }
  if (n_last_nodes > std::numeric_limits<std::size_t>::max()) {
    detail = "previous node high-water is not addressable";
    return false;
  }
  const std::size_t node_count = static_cast<std::size_t>(n_last_nodes);
  if (previous.level_offsets.empty() || previous.level_offsets.front() != 0
      || previous.level_offsets.back() != n_last_nodes) {
    detail = "previous level offsets do not cover the old-node domain";
    return false;
  }
  if (previous.node_to_level.size() != node_count
      || (!previous.group_levels.empty() && previous.group_levels.size() != previous.level_offsets.size())) {
    detail = "previous inverse has the wrong size";
    return false;
  }
  if (node_count == 0 && previous.level_offsets.size() != 1) {
    detail = "empty previous graph must use the unique offsets {0}";
    return false;
  }

  return true;
}

// A host reconstruction also needs the permutation. Owned CUDA topology
// construction consumes only the inverse/offsets; its permutation stays on device.
[[nodiscard]] inline bool validatePreviousLevelizationDimensions(const PreviousLevels& previous, std::uint64_t n_last_nodes, std::string& detail)
{
  if (!validatePreviousLevelMappingDimensions(previous, n_last_nodes, detail))
    return false;
  if (previous.level_nodes.size() != n_last_nodes) {
    detail = "previous permutation has the wrong size";
    return false;
  }
  return true;
}

[[nodiscard]] inline bool validatePreviousLevelizationShape(const PreviousLevels& previous, std::uint64_t n_last_nodes, std::string& detail)
{
  if (!validatePreviousLevelizationDimensions(previous, n_last_nodes, detail))
    return false;
  const std::size_t node_count = static_cast<std::size_t>(n_last_nodes);
  std::vector<std::uint8_t> seen(node_count, 0);
  for (std::size_t level = 0; level + 1 < previous.level_offsets.size();
       ++level) {
    const std::uint64_t begin = previous.level_offsets[level];
    const std::uint64_t end = previous.level_offsets[level + 1];
    if (begin >= end || end > n_last_nodes) {
      detail = "previous levels must be nonempty and in range";
      return false;
    }
    if (level == std::numeric_limits<std::uint64_t>::max()) {
      detail = "previous one-based level number overflows";
      return false;
    }
    const std::uint64_t expected_level =
        static_cast<std::uint64_t>(level) + 1;
    for (std::uint64_t position = begin; position < end; ++position) {
      const csr::NodeId node =
          previous.level_nodes[static_cast<std::size_t>(position)];
      if (node.value() >= n_last_nodes
          || seen[static_cast<std::size_t>(node.value())] != 0) {
        detail = "previous level_nodes is not an old-node permutation";
        return false;
      }
      seen[static_cast<std::size_t>(node.value())] = 1;
      if (previous.levelOf(static_cast<std::size_t>(node.value()))
          != expected_level) {
        detail = "previous inverse level map disagrees with offsets";
        return false;
      }
    }
  }
  for (std::uint8_t present : seen)
    if (present == 0) {
      detail = "previous levelization omits an old node";
      return false;
    }
  return true;
}

}  // namespace ista::timing_incremental_levelization::detail
