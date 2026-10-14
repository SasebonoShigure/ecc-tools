// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "DualCsrDescriptors.hpp"

namespace ista::timing_dual_csr::detail {

// Only the sealed builder may reuse its constructed row/ArcKey proof.
// External descriptors must use validateDualCsrDescriptor.
[[nodiscard]] DualCsrViolations validateConstructedDualCsrDescriptor(
    const DualCsrState& committed, const DualCsrBatchDescriptor& descriptor);

inline void reject(DualCsrViolations& violations, DualCsrViolationCode code,
                   std::string field, std::string message)
{
  violations.push_back(
      {code, std::move(field), std::move(message)});
}

inline bool validateOffsets(const std::vector<std::uint64_t>& offsets,
                            std::size_t rows, std::size_t entries,
                            const std::string& field,
                            DualCsrViolations& violations)
{
  if (rows == static_cast<std::size_t>(-1) || offsets.size() != rows + 1) {
    reject(violations, DualCsrViolationCode::kShape, field,
           "offset count must equal row count plus one");
    return false;
  }
  if (offsets.empty() || offsets.front() != 0) {
    reject(violations, DualCsrViolationCode::kShape, field,
           "offsets must begin at zero");
    return false;
  }
  for (std::size_t index = 1; index < offsets.size(); ++index) {
    if (offsets[index - 1] > offsets[index]) {
      reject(violations, DualCsrViolationCode::kShape, field,
             "offsets must be monotonic");
      return false;
    }
  }
  if (offsets.back() != entries) {
    reject(violations, DualCsrViolationCode::kShape, field,
           "last offset must equal physical entry count");
    return false;
  }
  return true;
}

inline bool rowContainsSlot(const std::vector<std::uint64_t>& offsets,
                            csr::NodeId row, std::uint64_t slot) noexcept
{
  const auto index = row.value();
  return index + 1 < offsets.size() && offsets[index] <= slot
         && slot < offsets[index + 1];
}

inline bool validExtent(const csr::CsrBufferExtent& extent,
                        std::size_t logical_size) noexcept
{
  return extent.logical_size == logical_size
         && extent.capacity >= extent.logical_size;
}

inline std::uint64_t activeArcCount(const DualCsrState& state) noexcept
{
  std::uint64_t count = 0;
  for (const auto& slot : state.arc_slots)
    count += slot.active ? 1U : 0U;
  return count;
}

}  // namespace ista::timing_dual_csr::detail
