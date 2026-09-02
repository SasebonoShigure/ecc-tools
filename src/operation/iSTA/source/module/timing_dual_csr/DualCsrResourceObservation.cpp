// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrResourceObservation.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "DualCsrStateValidation.hpp"

namespace ista::timing_dual_csr {
namespace {

void reject(DualCsrViolations& violations, DualCsrViolationCode code,
            std::string field, std::string message)
{
  violations.push_back(
      {code, std::move(field), std::move(message)});
}

bool toU64(std::size_t value, std::uint64_t& result) noexcept
{
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value
        > static_cast<std::size_t>(
            std::numeric_limits<std::uint64_t>::max()))
      return false;
  }
  result = static_cast<std::uint64_t>(value);
  return true;
}

template <typename T>
bool exactExtent(const csr::CsrBufferExtent& extent,
                 const std::vector<T>& values) noexcept
{
  std::uint64_t logical = 0;
  std::uint64_t capacity = 0;
  return toU64(values.size(), logical)
         && toU64(values.capacity(), capacity)
         && extent.logical_size == logical
         && extent.capacity == capacity;
}

template <typename T>
bool observeVector(DualCsrVectorMemoryObservation& target,
                   DualCsrBufferKind buffer,
                   const std::vector<T>& values,
                   std::string field,
                   DualCsrViolations& violations)
{
  std::uint64_t logical = 0;
  std::uint64_t capacity = 0;
  std::uint64_t element_size = 0;
  if (!toU64(values.size(), logical)
      || !toU64(values.capacity(), capacity)
      || !toU64(sizeof(T), element_size)) {
    reject(violations, DualCsrViolationCode::kCapacity,
           std::move(field),
           "vector size, capacity, or element size exceeds uint64_t");
    return false;
  }
  const auto reserved = detail::checkedShallowVectorStorageBytes(
      capacity, element_size);
  if (!reserved) {
    reject(violations, DualCsrViolationCode::kCapacity,
           std::move(field),
           "shallow vector storage estimate overflows uint64_t");
    return false;
  }
  target = {.buffer = buffer,
            .logical_elements = logical,
            .capacity_elements = capacity,
            .element_size_bytes = element_size,
            .estimated_reserved_bytes = *reserved};
  return true;
}

void requireExactAdjacencyExtents(const DualCsrState& state,
                                  DualCsrViolations& violations)
{
  const auto require = [&](bool condition, std::string field) {
    if (!condition)
      reject(violations, DualCsrViolationCode::kCapacity,
             std::move(field),
             "declared extent differs from the real vector size/capacity");
  };
  require(exactExtent(state.base_out.offsets_extent,
                      state.base_out.row_offsets),
          "resource_observation.base_out_offsets");
  require(exactExtent(state.base_out.edge_extent,
                      state.base_out.physical_slots),
          "resource_observation.base_out_slots");
  require(exactExtent(state.base_in.offsets_extent,
                      state.base_in.row_offsets),
          "resource_observation.base_in_offsets");
  require(exactExtent(state.base_in.edge_extent,
                      state.base_in.physical_slots),
          "resource_observation.base_in_slots");
  require(exactExtent(state.aux_out.offsets_extent,
                      state.aux_out.row_offsets),
          "resource_observation.aux_offsets");
  require(exactExtent(state.aux_out.source_extent,
                      state.aux_out.row_sources),
          "resource_observation.aux_sources");
  require(exactExtent(state.aux_out.edge_extent,
                      state.aux_out.arc_ids),
          "resource_observation.aux_arcs");
  require(exactExtent(state.aux_out.mapping_extent,
                      state.aux_out.source_to_aux_row),
          "resource_observation.aux_source_map");
}

}  // namespace

namespace detail {

std::optional<std::uint64_t> checkedShallowVectorStorageBytes(
    std::uint64_t capacity_elements,
    std::uint64_t element_size_bytes) noexcept
{
  if (element_size_bytes == 0
      || (capacity_elements != 0
          && element_size_bytes
                 > std::numeric_limits<std::uint64_t>::max()
                       / capacity_elements))
    return std::nullopt;
  return capacity_elements * element_size_bytes;
}

}  // namespace detail

double dualCsrTombstoneRatio(std::uint64_t physical_edges,
                             std::uint64_t tombstones) noexcept
{
  if (physical_edges == 0)
    return 0.0;
  return static_cast<double>(tombstones)
         / static_cast<double>(physical_edges);
}

DualCsrResourceObservationAttempt observeDualCsrResources(
    const DualCsrState& state)
{
  DualCsrResourceObservationAttempt attempt;
  attempt.violations = validateDualCsrState(state);
  requireExactAdjacencyExtents(state, attempt.violations);
  if (!attempt.violations.empty())
    return attempt;

  DualCsrResourceObservation observation{
      .generation = state.generation,
      .tombstone_ratio = dualCsrTombstoneRatio(
          state.telemetry.base_physical_edges,
          state.telemetry.base_tombstones),
      .requested_strategy = kDualCsrRequestedStrategy,
      .executed_strategy = kDualCsrExecutedStrategy,
      .vector_memory = {}};
  auto& buffers = observation.vector_memory.buffers;
  bool valid = true;
  valid &= observeVector(buffers[0], DualCsrBufferKind::kNodes,
                         state.nodes, "resource_observation.nodes",
                         attempt.violations);
  valid &= observeVector(buffers[1], DualCsrBufferKind::kArcSlots,
                         state.arc_slots, "resource_observation.arc_slots",
                         attempt.violations);
  valid &= observeVector(buffers[2], DualCsrBufferKind::kBaseOutOffsets,
                         state.base_out.row_offsets,
                         "resource_observation.base_out_offsets",
                         attempt.violations);
  valid &= observeVector(buffers[3], DualCsrBufferKind::kBaseOutSlots,
                         state.base_out.physical_slots,
                         "resource_observation.base_out_slots",
                         attempt.violations);
  valid &= observeVector(buffers[4], DualCsrBufferKind::kBaseInOffsets,
                         state.base_in.row_offsets,
                         "resource_observation.base_in_offsets",
                         attempt.violations);
  valid &= observeVector(buffers[5], DualCsrBufferKind::kBaseInSlots,
                         state.base_in.physical_slots,
                         "resource_observation.base_in_slots",
                         attempt.violations);
  valid &= observeVector(buffers[6], DualCsrBufferKind::kAuxOffsets,
                         state.aux_out.row_offsets,
                         "resource_observation.aux_offsets",
                         attempt.violations);
  valid &= observeVector(buffers[7], DualCsrBufferKind::kAuxSources,
                         state.aux_out.row_sources,
                         "resource_observation.aux_sources",
                         attempt.violations);
  valid &= observeVector(buffers[8], DualCsrBufferKind::kAuxArcs,
                         state.aux_out.arc_ids,
                         "resource_observation.aux_arcs",
                         attempt.violations);
  valid &= observeVector(buffers[9], DualCsrBufferKind::kAuxSourceMap,
                         state.aux_out.source_to_aux_row,
                         "resource_observation.aux_source_map",
                         attempt.violations);
  if (valid && attempt.violations.empty())
    attempt.observation = std::move(observation);
  return attempt;
}

DualCsrViolations validateDualCsrResourceObservation(
    const DualCsrState& state,
    const DualCsrResourceObservation& observation)
{
  auto expected = observeDualCsrResources(state);
  DualCsrViolations violations = std::move(expected.violations);
  if (expected.success() && observation != *expected.observation)
    reject(violations, DualCsrViolationCode::kExpectedValue,
           "resource_observation",
           "resource observation differs from real Dual-CSR vector storage");
  return violations;
}

}  // namespace ista::timing_dual_csr
