// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#pragma once
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "NativeTargetedEditPreparer.hpp"
namespace ista::timing_edit::detail {
csr::NodeRecord targetedNode(const Database& database, const NativeTargetedEditState& state, const std::string& name);
csr::ArcSemanticPayload targetedArc(const Database& database, const NativeTargetedEditState& state, std::size_t rank,
                                    std::map<std::string, csr::ArcOutputLoadMatrix>& loads);
std::vector<csr::ArcId> targetedRow(const Database& database, const NativeTargetedEditState& state, csr::NodeId node, bool incoming);
void canonicalizeTargetedNativeOrder(Database& database, NativeTargetedEditState& state);
}  // namespace ista::timing_edit::detail
