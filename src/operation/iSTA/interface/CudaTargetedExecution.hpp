// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "CudaDualCsrSession.hpp"
#include "StaExecutionContract.hpp"

namespace ista::csr::detail {
class NativeLibraryModels;
// Shared with the physical CSR transaction. Only the affected descriptor and
// its live RC owners are copied; immutable load models remain session-borrowed.
[[nodiscard]] CudaDualCsrPatch makeCudaTargetedPatch(
    const timing_dual_csr::DualCsrState& before,
    const timing_dual_csr::DualCsrBatchDescriptor& descriptor, NativeLibraryModels& models);

// Shared production policy; the returned pending candidate still requires
// common-owner qualification and publication. Detailed failed-attempt audit is ON only.
[[nodiscard]] CudaDualCsrSession stageCudaTargetedBatch(
    CudaDualCsrSession& owner, const CudaDualCsrPatch& patch, StaUpdateStrategy strategy,
    StaFallbackPolicy fallback, StaExecutionRecord& execution,
    std::optional<CudaDualCsrTelemetry>& rejected_mixed_attempt);
}
