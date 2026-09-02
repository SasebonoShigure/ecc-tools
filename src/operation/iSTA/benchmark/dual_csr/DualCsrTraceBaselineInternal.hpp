// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "CommittedDualCsrAnalysisState.hpp"

namespace ista {
class Database;
namespace csr {
struct CsrReferenceInput;
}
}  // namespace ista

namespace ista::timing_dual_csr::detail {

[[nodiscard]] CommittedDualCsrAnalysisState makeTraceBaseline(
    const Database& database,
    const csr::CsrReferenceInput& reference_input,
    timing_edit::TimingEditSessionToken session_token);

}  // namespace ista::timing_dual_csr::detail
