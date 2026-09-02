// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "MixedLevelizationTelemetry.hpp"
#include "MixedLevelizationTypes.hpp"

namespace ista::timing_incremental_levelization {

struct MixedPrecedenceGraph
{
  MixedIdDomain domain;
  std::uint64_t generation_before{0};
  std::uint64_t generation_after{0};
  std::vector<std::uint64_t> adjacency_offsets{0};
  std::vector<MixedVertexId> successors;
  std::vector<std::uint64_t> indegree;
  MixedLevelizationTelemetry telemetry;

  friend bool operator==(const MixedPrecedenceGraph&,
                         const MixedPrecedenceGraph&) = default;
};

enum class MixedGraphBuildFailureCode : std::uint8_t
{
  kOldOldOrderConflict,
  kViewError,
  kMalformedPreviousLevels,
  kMappingOverflow,
  kInvariantFailure
};

struct MixedGraphBuildFailure
{
  MixedGraphBuildFailureCode code{
      MixedGraphBuildFailureCode::kInvariantFailure};
  std::optional<csr::CsrViewError> view_error;
  std::string detail;
  MixedLevelizationTelemetry telemetry;

  friend bool operator==(const MixedGraphBuildFailure&,
                         const MixedGraphBuildFailure&) = default;
};

using MixedGraphBuildAttempt =
    std::variant<MixedPrecedenceGraph, MixedGraphBuildFailure>;

[[nodiscard]] MixedGraphBuildAttempt buildMixedLevelizationGraph(
    const csr::ActiveCsrEdgeView& edges,
    const csr::CsrFifoLevelization& previous,
    const MixedLevelizationRequest& request);

}  // namespace ista::timing_incremental_levelization
