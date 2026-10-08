// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "NativeDualCsrReferenceCapture.hpp"
namespace ista { class Database; }
namespace ista::timing_edit { struct NativeTargetedEditBatch; }
namespace ista::timing_dual_csr::detail {
[[nodiscard]] std::optional<NarrowCaptureError> captureNativeDualCsrParasitics(
    const Database&, const timing_edit::NativeTargetedEditBatch&, const DualSemanticReadView&,
    const AffectedCapturePlan&, NarrowTimingRecords&);
}
