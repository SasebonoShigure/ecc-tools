// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <vector>

#include "TimingEditTypes.hpp"

namespace ista::timing_edit {

[[nodiscard]] std::vector<std::uint8_t> canonicalCreationTokenBytes(const CreationToken& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalStableNodeRefBytes(const StableNodeRef& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalStableNetRefBytes(const StableNetRef& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalStableArcRefBytes(const StableArcRef& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalPreStateEvidenceBytes(const CanonicalPreStateEvidence& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalBaselineSemanticIdentityBytes(const BaselineSemanticIdentity& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalCommittedEditIdentityBytes(const CommittedEditIdentity& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalLibraryCellRefBytes(const LibraryCellRef& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalBufferInsertionMutationBytes(const BufferInsertionMutation& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalDesignEditPlanBytes(const DesignEditPlan& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalDesignEditPlanBytes(const BaselineSemanticIdentity& baseline, const EditBatch& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalTimingGraphEditBytes(const TimingGraphEdit& value);
[[nodiscard]] std::uint8_t timingGraphEditCanonicalRank(const TimingGraphEdit& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalTimingGraphDeltaBytes(const TimingGraphDelta& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalPreparedBatchDataBytes(const PreparedBatchData& value);

}  // namespace ista::timing_edit
