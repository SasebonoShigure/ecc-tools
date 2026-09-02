// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "DualCsrState.hpp"

namespace ista::timing_dual_csr {

enum class DualCsrComputeStrategy : std::uint8_t
{
  kCpu
};

enum class DualCsrObservedUpdateStrategy : std::uint8_t
{
  kIncremental,
  kTargetedDualCsr
};

enum class DualCsrObservedFallbackStrategy : std::uint8_t
{
  kError,
  kNone
};

enum class DualCsrObservedPropagationStrategy : std::uint8_t
{
  kFullGraph
};

enum class DualCsrMemoryEstimateKind : std::uint8_t
{
  kShallowVectorStorage
};

struct DualCsrRequestedStrategyObservation
{
  DualCsrComputeStrategy compute{DualCsrComputeStrategy::kCpu};
  DualCsrObservedUpdateStrategy update{
      DualCsrObservedUpdateStrategy::kIncremental};
  DualCsrObservedFallbackStrategy fallback{
      DualCsrObservedFallbackStrategy::kError};

  friend bool operator==(const DualCsrRequestedStrategyObservation&,
                         const DualCsrRequestedStrategyObservation&) = default;
};

struct DualCsrExecutedStrategyObservation
{
  DualCsrComputeStrategy compute{DualCsrComputeStrategy::kCpu};
  DualCsrObservedUpdateStrategy update{
      DualCsrObservedUpdateStrategy::kTargetedDualCsr};
  DualCsrObservedFallbackStrategy fallback{
      DualCsrObservedFallbackStrategy::kNone};
  DualCsrObservedPropagationStrategy propagation{
      DualCsrObservedPropagationStrategy::kFullGraph};

  friend bool operator==(const DualCsrExecutedStrategyObservation&,
                         const DualCsrExecutedStrategyObservation&) = default;
};

inline constexpr DualCsrRequestedStrategyObservation
    kDualCsrRequestedStrategy{};
inline constexpr DualCsrExecutedStrategyObservation
    kDualCsrExecutedStrategy{};
inline constexpr std::size_t kDualCsrObservedVectorCount = 10;
inline constexpr std::array<DualCsrBufferKind,
                            kDualCsrObservedVectorCount>
    kDualCsrObservedBufferOrder{
        DualCsrBufferKind::kNodes,
        DualCsrBufferKind::kArcSlots,
        DualCsrBufferKind::kBaseOutOffsets,
        DualCsrBufferKind::kBaseOutSlots,
        DualCsrBufferKind::kBaseInOffsets,
        DualCsrBufferKind::kBaseInSlots,
        DualCsrBufferKind::kAuxOffsets,
        DualCsrBufferKind::kAuxSources,
        DualCsrBufferKind::kAuxArcs,
        DualCsrBufferKind::kAuxSourceMap};
static_assert(kDualCsrObservedVectorCount
              == static_cast<std::size_t>(
                     DualCsrBufferKind::kAuxSourceMap) + 1);

struct DualCsrVectorMemoryObservation
{
  DualCsrBufferKind buffer{DualCsrBufferKind::kNodes};
  std::uint64_t logical_elements{0};
  std::uint64_t capacity_elements{0};
  std::uint64_t element_size_bytes{0};
  std::uint64_t estimated_reserved_bytes{0};

  friend bool operator==(const DualCsrVectorMemoryObservation&,
                         const DualCsrVectorMemoryObservation&) = default;
};

struct DualCsrVectorMemoryObservationSet
{
  DualCsrMemoryEstimateKind estimate_kind{
      DualCsrMemoryEstimateKind::kShallowVectorStorage};
  std::array<DualCsrVectorMemoryObservation,
             kDualCsrObservedVectorCount> buffers;

  friend bool operator==(const DualCsrVectorMemoryObservationSet&,
                         const DualCsrVectorMemoryObservationSet&) = default;
};

struct DualCsrResourceObservation
{
  timing_edit::TransactionGeneration generation;
  double tombstone_ratio{0.0};
  DualCsrRequestedStrategyObservation requested_strategy;
  DualCsrExecutedStrategyObservation executed_strategy;
  DualCsrVectorMemoryObservationSet vector_memory;

  friend bool operator==(const DualCsrResourceObservation&,
                         const DualCsrResourceObservation&) = default;
};

struct DualCsrResourceObservationAttempt
{
  std::optional<DualCsrResourceObservation> observation;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return observation.has_value() && violations.empty();
  }
};

[[nodiscard]] double dualCsrTombstoneRatio(
    std::uint64_t physical_edges, std::uint64_t tombstones) noexcept;

[[nodiscard]] DualCsrResourceObservationAttempt
observeDualCsrResources(const DualCsrState& state);

[[nodiscard]] DualCsrViolations validateDualCsrResourceObservation(
    const DualCsrState& state,
    const DualCsrResourceObservation& observation);

namespace detail {

[[nodiscard]] std::optional<std::uint64_t>
checkedShallowVectorStorageBytes(
    std::uint64_t capacity_elements,
    std::uint64_t element_size_bytes) noexcept;

}  // namespace detail
}  // namespace ista::timing_dual_csr
