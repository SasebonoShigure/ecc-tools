// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <string>

#include "CorrectedMixedKahnLevelizer.hpp"

namespace ista::timing_incremental_levelization {

enum class MixedValidationCode : std::uint8_t
{
  kValid,
  kMalformedDomain,
  kMalformedAdjacency,
  kIndegreeMismatch,
  kMalformedLevelization
};

struct MixedValidationResult
{
  MixedValidationCode code{MixedValidationCode::kValid};
  std::string detail;

  [[nodiscard]] bool valid() const noexcept
  {
    return code == MixedValidationCode::kValid;
  }

  friend bool operator==(const MixedValidationResult&,
                         const MixedValidationResult&) = default;
};

[[nodiscard]] MixedValidationResult validateMixedPrecedenceGraph(
    const MixedPrecedenceGraph& graph);
[[nodiscard]] MixedValidationResult validateMixedKahnLevelization(
    const MixedPrecedenceGraph& graph,
    const MixedKahnLevelization& levels);

}  // namespace ista::timing_incremental_levelization
