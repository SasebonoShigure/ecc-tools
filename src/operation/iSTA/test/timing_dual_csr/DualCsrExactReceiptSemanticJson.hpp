// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrExactReceipt.hpp"
#include "json.hpp"

namespace ista::timing_dual_csr::test::semantic_json {

[[nodiscard]] nlohmann::json stableNode(
    const timing_edit::StableNodeRef& value);
[[nodiscard]] nlohmann::json stableNet(
    const timing_edit::StableNetRef& value);
[[nodiscard]] nlohmann::json stableArc(
    const timing_edit::StableArcRef& value);
[[nodiscard]] nlohmann::json resolver(
    const timing_edit::TimingEditResolverState& value);
[[nodiscard]] nlohmann::json identity(
    const timing_edit::CommittedEditIdentity& value);
[[nodiscard]] nlohmann::json referenceInput(
    const csr::CsrReferenceInput& value);
[[nodiscard]] nlohmann::json descriptor(
    const DualCsrBatchDescriptor& value);
[[nodiscard]] nlohmann::json semanticPlan(
    const timing_edit::DesignEditPlan& value);

}  // namespace ista::timing_dual_csr::test::semantic_json
