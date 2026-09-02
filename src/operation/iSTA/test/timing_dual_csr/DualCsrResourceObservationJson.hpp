// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <string_view>
#include <vector>

#include "DualCsrResourceObservation.hpp"
#include "json.hpp"

namespace ista::timing_dual_csr::test {

[[nodiscard]] std::string_view dualCsrBufferToken(
    DualCsrBufferKind value);

[[nodiscard]] nlohmann::json resourceObservationToJson(
    const DualCsrResourceObservation& observation);

[[nodiscard]] nlohmann::json resourceObservationsToJson(
    const std::vector<DualCsrResourceObservation>& observations);

}  // namespace ista::timing_dual_csr::test
