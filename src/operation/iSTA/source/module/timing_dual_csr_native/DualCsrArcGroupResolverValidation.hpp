// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <vector>

#include "DualCsrArcGroupIndex.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_dual_csr {

struct DualCsrArcGroupBindingAttempt
{
  std::vector<DualCsrArcGroupIndexBinding> bindings;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return violations.empty();
  }
};

// Generation zero and recovery only: materializes the complete typed
// resolver binding once, outside targeted capture accounting.
[[nodiscard]] DualCsrArcGroupBindingAttempt
captureFullDualCsrArcGroupBindings(
    const timing_edit::TimingEditResolverState& resolver);

// Generation zero and recovery only: exact full resolver/index check.
[[nodiscard]] DualCsrViolations validateDualCsrArcGroupResolverFull(
    const DualCsrArcGroupIndex& index,
    const timing_edit::TimingEditResolverState& resolver);

// Normal targeted induction: exact-before is already committed-valid, so
// only changed slots and the append range are inspected.
[[nodiscard]] DualCsrViolations validateDualCsrArcGroupResolverTransition(
    const DualCsrArcGroupIndex& committed,
    const DualCsrArcGroupIndex& staged,
    const DualCsrBatchDescriptor& descriptor,
    const timing_edit::TimingEditResolverState& staged_resolver);

}  // namespace ista::timing_dual_csr
