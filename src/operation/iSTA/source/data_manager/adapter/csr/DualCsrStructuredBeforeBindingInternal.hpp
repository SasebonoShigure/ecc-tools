// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>

namespace ista::timing_edit {
struct CommittedEditIdentity;
class TimingEditResolverState;
}  // namespace ista::timing_edit

namespace ista::timing_dual_csr {
class DualSemanticReadView;
struct AffectedCapturePlan;
struct DualCsrArcGroupIndex;
struct DualCsrState;

namespace detail {

void requireStructuredExactBefore(
    const DualCsrState& graph,
    const timing_edit::TimingEditResolverState& resolver,
    const std::optional<timing_edit::CommittedEditIdentity>& identity,
    const DualCsrArcGroupIndex& arc_group_index,
    const DualSemanticReadView& view);

void requireStructuredSelectedBefore(
    const DualCsrState& graph,
    const timing_edit::TimingEditResolverState& resolver,
    const DualCsrArcGroupIndex& arc_group_index,
    const DualSemanticReadView& view,
    const AffectedCapturePlan& plan);

}  // namespace detail
}  // namespace ista::timing_dual_csr
