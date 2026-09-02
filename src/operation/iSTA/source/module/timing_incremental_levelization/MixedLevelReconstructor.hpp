// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "CorrectedMixedKahnLevelizer.hpp"

namespace ista::timing_incremental_levelization {

enum class MixedReconstructionFailureCode : std::uint8_t
{
  kMalformedPreviousLevels,
  kMappingOverflow,
  kInvariantFailure
};

struct MixedReconstructionFailure
{
  MixedReconstructionFailureCode code{
      MixedReconstructionFailureCode::kInvariantFailure};
  std::string detail;

  friend bool operator==(const MixedReconstructionFailure&,
                         const MixedReconstructionFailure&) = default;
};

using MixedReconstructionAttempt =
    std::variant<csr::CsrFifoLevelization, MixedReconstructionFailure>;

[[nodiscard]] MixedReconstructionAttempt reconstructMixedLevelization(
    const MixedIdDomain& domain,
    const csr::CsrFifoLevelization& previous,
    const MixedKahnLevelization& mixed);

}  // namespace ista::timing_incremental_levelization
