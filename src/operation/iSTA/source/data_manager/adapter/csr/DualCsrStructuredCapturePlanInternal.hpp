// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "AffectedCapturePlan.hpp"
#include "DualCsrArcGroupIndex.hpp"
#include "DualSemanticReadView.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_dual_csr::detail {

[[nodiscard]] csr::NodeKey stableNodeKey(
    const timing_edit::StableNodeRef& ref,
    timing_edit::TimingEditResolverView resolver);

[[nodiscard]] csr::ArcKey materializedArcKey(
    const timing_edit::StableArcRef& ref,
    timing_edit::TimingEditResolverView resolver);

[[nodiscard]] csr::ArcKey committedArcKey(
    const timing_edit::StableArcRef& ref,
    timing_edit::TimingEditResolverView resolver,
    const DualSemanticReadView& view);

[[nodiscard]] AffectedCapturePlan makeStructuredCapturePlan(
    const timing_edit::PreparedBatch& batch,
    const timing_edit::TimingEditResolverState& committed,
    timing_edit::TimingEditResolverView staged,
    const DualCsrArcGroupIndex& arc_group_index,
    const DualSemanticReadView& view);

}  // namespace ista::timing_dual_csr::detail
