// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrTimingResultValidator.hpp"

namespace ista::timing_dual_csr::detail {

void validateDualCsrTimingSemantics(const DualCsrState& state,
                                    const DualCsrTimingResult& result,
                                    DualCsrViolations& violations);

}  // namespace ista::timing_dual_csr::detail
