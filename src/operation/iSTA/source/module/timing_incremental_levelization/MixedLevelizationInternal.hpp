// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "CsrFifoLevelization.hpp"

namespace ista::timing_incremental_levelization::detail {

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

[[nodiscard]] inline bool validatePreviousLevelizationShape(
    const csr::CsrFifoLevelization& previous, std::uint64_t n_last_nodes,
    std::string& detail)
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
  if (previous.level_nodes.size() != node_count
      || previous.node_to_level.size() != node_count) {
    detail = "previous permutation or inverse has the wrong size";
    return false;
  }
  if (node_count == 0 && previous.level_offsets.size() != 1) {
    detail = "empty previous graph must use the unique offsets {0}";
    return false;
  }

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
      if (previous.node_to_level[static_cast<std::size_t>(node.value())]
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
