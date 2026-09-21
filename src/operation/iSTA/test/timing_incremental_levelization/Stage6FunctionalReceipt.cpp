// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "Stage6FunctionalReceipt.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ista::timing_dual_csr::test {
namespace {

using Violations = std::vector<Stage6LevelizationViolation>;

void reject(Violations& errors, Stage6LevelizationViolationCode code,
            std::string field, std::string message)
{
  errors.push_back(
      {code, std::move(field), std::move(message)});
}

std::optional<std::size_t> nodeIndex(
    const DualCsrFunctionalProjection& graph, const csr::NodeKey& key)
{
  const auto found = std::find_if(
      graph.nodes.begin(), graph.nodes.end(),
      [&](const DualCsrProjectedNode& node) { return node.key == key; });
  if (found == graph.nodes.end())
    return std::nullopt;
  return static_cast<std::size_t>(
      std::distance(graph.nodes.begin(), found));
}

bool offsetsAreValid(const Stage6LevelizationReceipt& receipt,
                     std::size_t node_count)
{
  if (receipt.level_offsets.empty()
      || receipt.level_offsets.front() != 0
      || receipt.level_offsets.back() != node_count)
    return false;
  if (node_count == 0)
    return receipt.level_offsets.size() == 1;
  if (receipt.level_offsets.size() < 2)
    return false;
  for (std::size_t index = 1; index < receipt.level_offsets.size(); ++index) {
    if (receipt.level_offsets[index - 1] >= receipt.level_offsets[index]
        || receipt.level_offsets[index] > node_count)
      return false;
  }
  return true;
}

bool nodeKeysAreUnique(const std::vector<csr::NodeKey>& keys)
{
  for (std::size_t left = 0; left < keys.size(); ++left)
    for (std::size_t right = left + 1; right < keys.size(); ++right)
      if (keys[left] == keys[right])
        return false;
  return true;
}

bool graphKeysAreUnique(const DualCsrFunctionalProjection& graph)
{
  for (std::size_t left = 0; left < graph.nodes.size(); ++left)
    for (std::size_t right = left + 1; right < graph.nodes.size(); ++right)
      if (graph.nodes[left].key == graph.nodes[right].key)
        return false;
  return true;
}

bool isCompletePermutation(const DualCsrFunctionalProjection& graph,
                           const std::vector<csr::NodeKey>& keys)
{
  if (keys.size() != graph.nodes.size() || !nodeKeysAreUnique(keys)
      || !graphKeysAreUnique(graph))
    return false;
  return std::all_of(keys.begin(), keys.end(), [&](const csr::NodeKey& key) {
    return nodeIndex(graph, key).has_value();
  });
}

}  // namespace

Stage6SeparatedReceipt separateStage6FunctionalReceipt(
    const FunctionalSemanticReceipt& source, std::uint64_t generation)
{
  return {
      .functional = {
          .graph = source.graph,
          .timing = {
              .schema_version = source.timing.schema_version,
              .nodes = source.timing.nodes,
              .effective_clock = source.timing.effective_clock,
              .physical_clock = source.timing.physical_clock,
              .data_slews = source.timing.data_slews,
              .paths = source.timing.paths,
              .arcs = source.timing.arcs}},
      .levelization = {
          .generation = generation,
          .complete = true,
          .level_offsets = source.timing.level_offsets,
          .level_nodes = source.timing.level_nodes,
          .node_to_level = source.timing.node_to_level}};
}

std::vector<Stage6LevelizationViolation>
validateStage6LevelizationReceipt(
    const DualCsrFunctionalProjection& graph,
    const Stage6LevelizationReceipt& receipt,
    std::uint64_t expected_generation)
{
  Violations errors;
  const std::size_t node_count = graph.nodes.size();
  if (receipt.generation != expected_generation)
    reject(errors, Stage6LevelizationViolationCode::kGeneration,
           "levelization.generation", "generation binding differs");
  if (!receipt.complete)
    reject(errors, Stage6LevelizationViolationCode::kIncomplete,
           "levelization.complete", "receipt is not complete");

  const bool offsets_valid = offsetsAreValid(receipt, node_count);
  if (!offsets_valid)
    reject(errors, Stage6LevelizationViolationCode::kOffsets,
           "levelization.level_offsets",
           "offsets must delimit nonempty levels and exactly cover all nodes");

  const bool level_permutation
      = isCompletePermutation(graph, receipt.level_nodes);
  if (!level_permutation)
    reject(errors, Stage6LevelizationViolationCode::kPermutation,
           "levelization.level_nodes",
           "level nodes must be the complete graph NodeKey permutation");

  bool inverse_valid = receipt.node_to_level.size() == node_count;
  if (inverse_valid && offsets_valid && level_permutation) {
    const std::size_t level_count = receipt.level_offsets.size() - 1;
    for (std::size_t level = 0; level < level_count; ++level) {
      const std::size_t begin
          = static_cast<std::size_t>(receipt.level_offsets[level]);
      const std::size_t end
          = static_cast<std::size_t>(receipt.level_offsets[level + 1]);
      for (std::size_t position = begin; position < end; ++position) {
        const auto node = nodeIndex(graph, receipt.level_nodes[position]);
        if (!node || receipt.node_to_level[*node] != level + 1)
          inverse_valid = false;
      }
    }
    for (const auto value : receipt.node_to_level)
      if (value == 0 || value > level_count)
        inverse_valid = false;
  }
  if (!inverse_valid)
    reject(errors, Stage6LevelizationViolationCode::kInverse,
           "levelization.node_to_level",
           "inverse map does not match the level permutation");

  if (inverse_valid) {
    for (std::size_t index = 0; index < graph.active_arcs.size(); ++index) {
      const auto& arc = graph.active_arcs[index];
      if (!arc.eligibility.levelization)
        continue;
      const auto source = nodeIndex(graph, arc.source);
      const auto sink = nodeIndex(graph, arc.sink);
      if (!source || !sink
          || receipt.node_to_level[*source]
                 >= receipt.node_to_level[*sink]) {
        reject(errors, Stage6LevelizationViolationCode::kEligibleEdgeOrder,
               "levelization.active_arcs[" + std::to_string(index) + "]",
               "eligible arc does not strictly cross forward levels");
      }
    }
  }
  return errors;
}

}  // namespace ista::timing_dual_csr::test
