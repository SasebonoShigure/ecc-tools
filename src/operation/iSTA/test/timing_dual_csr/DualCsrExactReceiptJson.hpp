// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrExactReceipt.hpp"
#include "json.hpp"

namespace ista::timing_dual_csr::test {

[[nodiscard]] nlohmann::json structuralReceiptToJson(
    const DualCsrStructuralReceipt& receipt);
[[nodiscard]] nlohmann::json exactReceiptToJson(
    const DualCsrExactReceipt& receipt);

}  // namespace ista::timing_dual_csr::test
