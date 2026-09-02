// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrFunctionalProjection.hpp"
#include "json.hpp"

namespace ista::timing_dual_csr::test {

// Field-addressable wire form shared by the targeted and fresh processes.
// The production projector has already removed session-local IDs, native
// traversal ranks, physical slots, and diagnostic fingerprints.
[[nodiscard]] nlohmann::json functionalSemanticReceiptToJson(
    const FunctionalSemanticReceipt& receipt);

namespace detail {
[[nodiscard]] nlohmann::json functionalGraphToJson(
    const DualCsrFunctionalProjection& graph);
[[nodiscard]] nlohmann::json functionalTimingToJson(
    const DualCsrTimingSemanticProjection& timing);
}  // namespace detail

}  // namespace ista::timing_dual_csr::test
