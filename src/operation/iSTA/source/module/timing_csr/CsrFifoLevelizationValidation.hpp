// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>

#include "ActiveCsrEdgeView.hpp"
#include "CsrFifoLevelization.hpp"

namespace ista::csr {

enum class CsrFifoLevelizationValidationCode : std::uint8_t
{
  kValid,
  kViewError,
  kIncomplete,
  kMalformedOffsets,
  kMalformedPermutation,
  kMalformedInverse,
  kEligibleEdgeOrderViolation
};

struct CsrFifoLevelizationValidationResult
{
  CsrFifoLevelizationValidationCode code{CsrFifoLevelizationValidationCode::kValid};
  std::optional<CsrViewError> view_error;

  [[nodiscard]] bool valid() const noexcept
  {
    return code == CsrFifoLevelizationValidationCode::kValid && !view_error;
  }

  friend bool operator==(const CsrFifoLevelizationValidationResult&, const CsrFifoLevelizationValidationResult&) = default;
};

// Validates the complete public representation and every current eligible
// precedence edge. A non-empty levelization view_error is always a failure,
// even when all arrays and complete=true otherwise look valid.
[[nodiscard]] CsrFifoLevelizationValidationResult validateCsrFifoLevelization(const ActiveCsrEdgeView& edges,
                                                                               const CsrFifoLevelization& levels);

}  // namespace ista::csr
