// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrState.hpp"

namespace ista::timing_dual_csr::detail {

// Validates the stable-ID Dual payload through the canonical DB-free Stage 2
// intrinsic validator. The projection exists only for validation and never
// participates in timing traversal or update construction.
void validateReferencePayload(const DualCsrState& state,
                              DualCsrViolations& violations);

}  // namespace ista::timing_dual_csr::detail
