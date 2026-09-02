// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "ActiveCsrEdgeView.hpp"

namespace ista::timing_incremental_levelization {

enum class ActualDagClassificationCode : std::uint8_t
{
  kDag,
  kCycle,
  kViewError,
  kInvariantFailure
};

struct ActualDagClassification
{
  ActualDagClassificationCode code{ActualDagClassificationCode::kDag};
  std::optional<csr::CsrViewError> view_error;
  std::uint64_t eligible_arc_visits{0};
  std::uint64_t unique_endpoint_pairs{0};
  std::uint64_t processed_count{0};
  std::string detail;

  [[nodiscard]] bool isDag() const noexcept
  {
    return code == ActualDagClassificationCode::kDag;
  }

  friend bool operator==(const ActualDagClassification&,
                         const ActualDagClassification&) = default;
};

[[nodiscard]] ActualDagClassification classifyActualDag(
    const csr::ActiveCsrEdgeView& edges);

}  // namespace ista::timing_incremental_levelization
