// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrArcGroupIndex.hpp"
#include "DualSemanticReadView.hpp"
#include "NarrowTimingCaptureInternal.hpp"
#include "TimingEditResolver.hpp"

namespace ista { class Database; }
namespace ista::timing_edit { struct NativeTargetedEditBatch; }
namespace ista::csr::detail { class NativeLibraryModels; }
namespace ista::timing_dual_csr::detail {
[[nodiscard]] NativeCaptureResult captureNativeDualCsrPayload(
    const ista::Database&, csr::detail::NativeLibraryModels&, const DualSemanticReadView&,
    const DualCsrArcGroupIndex&,
    timing_edit::TimingEditResolverView,
    const AffectedCapturePlan&, const timing_edit::NativeTargetedEditBatch*);
}
