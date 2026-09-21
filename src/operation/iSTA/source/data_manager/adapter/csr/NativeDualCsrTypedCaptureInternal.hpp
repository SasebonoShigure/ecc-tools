// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "DualSemanticReadView.hpp"
#include "NarrowTimingCaptureInternal.hpp"
#include "TimingEditResolver.hpp"

namespace ista { class Database; }
namespace ista::timing_dual_csr::detail {

using PlannedNodeId = std::variant<csr::NodeId, NarrowCaptureError>;
[[nodiscard]] PlannedNodeId resolvePlannedNodeId(
    const DualSemanticReadView& committed,
    timing_edit::TimingEditResolverView staged,
    const AffectedCapturePlan& plan,
    const csr::NodeKey& key);

[[nodiscard]] std::optional<NarrowCaptureError>
validateCapturedLibraryCell(
    const timing_edit::LibraryCellRef& requested,
    const csr::ReferencedLibraryCellModel& model);

using NodeRoleProjection = std::variant<
    std::optional<CapturedNodeRole>, NarrowCaptureError>;
[[nodiscard]] NodeRoleProjection projectPlannedNodeRole(
    const std::vector<AffectedNodeModelCapture>& requests,
    timing_edit::TimingEditResolverView staged,
    const timing_edit::AppendOnlyHighWater& after,
    const std::map<std::string, const csr::ReferencedLibraryCellModel*>& cells,
    csr::NodeRecord& node);

using CapacitanceProjection = std::variant<
    CapturedCellPinCapacitance, NarrowCaptureError>;
[[nodiscard]] CapacitanceProjection projectPlannedCapacitance(
    const ista::Database& database,
    const std::vector<AffectedNodeModelCapture>& requests,
    timing_edit::TimingEditResolverView staged,
    const timing_edit::AppendOnlyHighWater& after,
    const csr::NodeRecord& node);

}  // namespace ista::timing_dual_csr::detail
