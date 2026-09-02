// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrArcGroupIndex.hpp"

namespace ista::timing_dual_csr {

// Core validation is deliberately resolver-free. Resolver induction belongs
// to timing_dual_csr_native and only checks changed slots in targeted mode.
[[nodiscard]] DualCsrViolations validateDualCsrArcGroupIndex(
    const DualCsrState& graph,
    const DualCsrArcGroupIndex& index);

// Pure descriptor-envelope validation. It performs no detached graph/index
// update; the transaction owns the sole staged update.
[[nodiscard]] DualCsrViolations validateDualCsrArcGroupIndexDelta(
    const DualCsrState& committed_graph,
    const DualCsrArcGroupIndex& committed_index,
    const DualCsrBatchDescriptor& descriptor);

}  // namespace ista::timing_dual_csr
