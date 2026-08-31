// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "FreshBaselineRunner.hpp"
#include "json.hpp"

namespace ista::buffer_trace::test {

// A semantic, field-addressable projection.  Digests embedded in Stage 3
// identities remain visible, but never replace the exact edit, database, CSR,
// resolver, or timing fields used for acceptance.
[[nodiscard]] nlohmann::json replayReceiptToJson(
    const ReplayReceipt& receipt);

}  // namespace ista::buffer_trace::test
