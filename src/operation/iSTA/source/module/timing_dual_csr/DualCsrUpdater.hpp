// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>

#include "DualCsrArcGroupIndex.hpp"
#include "DualCsrDescriptorValidation.hpp"

namespace ista::timing_dual_csr {

enum class DualCsrUpdateFailpoint : std::uint8_t
{
  kNone,
  kAfterDetachedClone,
  kAfterCapacityGrow,
  kAfterStructuralUpdate,
  kBeforeValidation
};

struct DualCsrUpdateOptions
{
  DualCsrUpdateFailpoint failpoint{DualCsrUpdateFailpoint::kNone};
};

struct DualCsrUpdateAttempt
{
  std::optional<DualCsrState> state;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return state.has_value() && violations.empty();
  }
};

struct DualCsrIndexedUpdateAttempt
{
  std::optional<DualCsrState> state;
  std::optional<DualCsrArcGroupIndex> arc_group_index;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return state.has_value() && arc_group_index.has_value()
           && violations.empty();
  }
};

class DualCsrUpdater final
{
 public:
  // Descriptor failures are returned as typed violations. Allocation and the
  // explicit allocation failpoints throw; committed remains untouched because
  // every write targets a detached copy.
  [[nodiscard]] static DualCsrUpdateAttempt stage(
      const DualCsrState& committed, const DualCsrBatchDescriptor& descriptor,
      DualCsrUpdateOptions options = {});

  [[nodiscard]] static DualCsrIndexedUpdateAttempt stage(
      const DualCsrState& committed,
      const DualCsrArcGroupIndex& committed_index,
      const DualCsrBatchDescriptor& descriptor,
      DualCsrUpdateOptions options = {});
};

}  // namespace ista::timing_dual_csr
