// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "CudaDualCsrSession.hpp"
#include "StaExecutionContract.hpp"

namespace ista::csr::detail {
// Shared production policy; the returned pending candidate still requires
// common-owner qualification and publication. Detailed failed-attempt audit is ON only.
[[nodiscard]] CudaDualCsrSession stageCudaTargetedBatch(
    CudaDualCsrSession& owner, const CudaDualCsrPatch& patch, StaUpdateStrategy strategy,
    StaFallbackPolicy fallback, StaExecutionRecord& execution,
    std::optional<CudaDualCsrTelemetry>& rejected_mixed_attempt);
}
