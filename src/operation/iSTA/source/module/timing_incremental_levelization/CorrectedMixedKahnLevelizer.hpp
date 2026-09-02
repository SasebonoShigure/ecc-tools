// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "MixedLevelizationGraphBuilder.hpp"

namespace ista::timing_incremental_levelization {

struct MixedKahnLevelization
{
  std::vector<std::uint64_t> level_offsets{0};
  std::vector<MixedVertexId> level_vertices;
  std::vector<std::uint64_t> vertex_to_level;
  std::uint64_t processed_count{0};
  std::uint64_t max_frontier{0};

  friend bool operator==(const MixedKahnLevelization&,
                         const MixedKahnLevelization&) = default;
};

enum class MixedKahnFailureCode : std::uint8_t
{
  kMixedCycle,
  kInvariantFailure
};

struct MixedKahnFailure
{
  MixedKahnFailureCode code{MixedKahnFailureCode::kInvariantFailure};
  std::uint64_t processed_count{0};
  std::uint64_t remaining_vertex_count{0};
  std::uint64_t max_frontier{0};
  std::string detail;

  friend bool operator==(const MixedKahnFailure&,
                         const MixedKahnFailure&) = default;
};

using MixedKahnAttempt =
    std::variant<MixedKahnLevelization, MixedKahnFailure>;

[[nodiscard]] MixedKahnAttempt runCorrectedMixedKahn(
    const MixedPrecedenceGraph& graph);

}  // namespace ista::timing_incremental_levelization
