// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "CsrReferenceTypes.hpp"
#include "json.hpp"

namespace ista::buffer_trace::test {

// Test-only, field-preserving projections used by the cross-process
// controller.  They deliberately expose topology, electrical inputs,
// constraints, and timing lanes instead of replacing them with a digest.
[[nodiscard]] nlohmann::json csrReferenceInputToJson(
    const csr::CsrReferenceInput& input);
[[nodiscard]] nlohmann::json csrTimingResultToJson(
    const csr::CsrTimingResult& result);

}  // namespace ista::buffer_trace::test
