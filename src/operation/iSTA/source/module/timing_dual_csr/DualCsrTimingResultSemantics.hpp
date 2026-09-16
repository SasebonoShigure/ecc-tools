// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "CsrReferenceValidatorDetail.hpp"
#include "DualCsrTimingResultValidator.hpp"

namespace ista::timing_dual_csr::detail {

inline constexpr double kPathRoundScale = 1E15;

inline void rejectSemantic(DualCsrViolations& violations, std::string field,
                    std::string message)
{
  violations.push_back({DualCsrViolationCode::kTimingResult,
                        std::move(field), std::move(message)});
}

template <typename T>
bool hasShape(const csr::OptionalSoa<T>& values, std::size_t expected) noexcept
{
  return values.presence.size() == expected && values.values.size() == expected;
}

template <typename T, typename Predicate>
bool validatePresentDomain(const csr::OptionalSoa<T>& values,
                           std::size_t expected, const std::string& field,
                           Predicate predicate, const std::string& message,
                           DualCsrViolations& violations)
{
  if (!hasShape(values, expected))
    return false;
  bool valid = true;
  for (std::size_t lane = 0; lane < expected; ++lane)
    if (values.presence[lane] == 1 && !predicate(values.values[lane])) {
      rejectSemantic(violations, field, message);
      valid = false;
    }
  return valid;
}

inline bool hasResolvedClockSource(const csr::ClockConstraintModel& clock,
                            csr::NodeId node)
{
  return std::ranges::any_of(
      clock.ordered_sources, [&](const csr::RawNodeReference& source) {
        return source.resolved_node && *source.resolved_node == node;
      });
}

inline bool checkedRoundedPathArrival(double lhs, double rhs, double& result)
{
  if (!csr::detail::isFinite(lhs) || !csr::detail::isFinite(rhs))
    return false;
  const double scaled = (lhs + rhs) * kPathRoundScale;
  if (!csr::detail::isFinite(scaled))
    return false;
  result = std::round(scaled) / kPathRoundScale;
  return csr::detail::isFinite(result);
}

void validateClockAndSlewDomains(const DualCsrState& state,
                                 const csr::CsrTimingResult& result,
                                 DualCsrViolations& violations);

void validateDualCsrTimingSemantics(const DualCsrState& state,
                                    const DualCsrTimingResult& result,
                                    DualCsrViolations& violations);

}  // namespace ista::timing_dual_csr::detail
