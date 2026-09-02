// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DualCsrFunctionalProjection.hpp"

namespace ista::timing_dual_csr::test {

struct Stage6TimingValueSemanticProjection
{
  std::string schema_version;
  DualCsrProjectedNodeTiming nodes;
  DualCsrProjectedClockTiming effective_clock;
  DualCsrProjectedClockTiming physical_clock;
  csr::OptionalSoa<double> data_slews;
  DualCsrProjectedPathTiming paths;
  DualCsrProjectedArcTiming arcs;

  friend bool operator==(const Stage6TimingValueSemanticProjection&,
                         const Stage6TimingValueSemanticProjection&) = default;
};

struct Stage6FunctionalReceipt
{
  DualCsrFunctionalProjection graph;
  Stage6TimingValueSemanticProjection timing;

  friend bool operator==(const Stage6FunctionalReceipt&,
                         const Stage6FunctionalReceipt&) = default;
};

struct Stage6LevelizationReceipt
{
  std::uint64_t generation{0};
  bool complete{false};
  std::vector<std::uint64_t> level_offsets;
  std::vector<csr::NodeKey> level_nodes;
  std::vector<std::uint64_t> node_to_level;
  std::vector<csr::NodeKey> timing_order;

  friend bool operator==(const Stage6LevelizationReceipt&,
                         const Stage6LevelizationReceipt&) = default;
};

struct Stage6SeparatedReceipt
{
  Stage6FunctionalReceipt functional;
  Stage6LevelizationReceipt levelization;
};

enum class Stage6LevelizationViolationCode : std::uint8_t
{
  kGeneration,
  kIncomplete,
  kOffsets,
  kPermutation,
  kInverse,
  kTimingOrder,
  kEligibleEdgeOrder
};

struct Stage6LevelizationViolation
{
  Stage6LevelizationViolationCode code{
      Stage6LevelizationViolationCode::kOffsets};
  std::string field;
  std::string message;

  friend bool operator==(const Stage6LevelizationViolation&,
                         const Stage6LevelizationViolation&) = default;
};

[[nodiscard]] Stage6SeparatedReceipt separateStage6FunctionalReceipt(
    const FunctionalSemanticReceipt& source, std::uint64_t generation);

[[nodiscard]] std::vector<Stage6LevelizationViolation>
validateStage6LevelizationReceipt(
    const DualCsrFunctionalProjection& graph,
    const Stage6LevelizationReceipt& receipt,
    std::uint64_t expected_generation);

}  // namespace ista::timing_dual_csr::test
