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
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "TimingEditTypes.hpp"

namespace ista::timing_edit {

enum class TimingEditContractCode : std::uint8_t
{
  kSchemaMismatch,
  kInvalidValue,
  kInvalidToken,
  kInvalidSemanticRef,
  kDuplicate,
  kConflict,
  kGenerationMismatch,
  kCapacityOverflow,
  kUnsupported,
  kNotCanonical,
  kIdentityMismatch
};

[[nodiscard]] std::string_view token(TimingEditContractCode value);

struct TimingEditContractViolation
{
  TimingEditContractCode code{TimingEditContractCode::kInvalidValue};
  std::string field;
  std::string message;

  friend bool operator==(const TimingEditContractViolation&, const TimingEditContractViolation&) = default;
};

using TimingEditContractViolations = std::vector<TimingEditContractViolation>;
using EditBatchNormalization = std::variant<EditBatch, TimingEditContractViolations>;
using DesignEditPlanNormalization = std::variant<DesignEditPlan, TimingEditContractViolations>;
using TimingGraphDeltaNormalization = std::variant<TimingGraphDelta, TimingEditContractViolations>;

[[nodiscard]] TimingEditContractViolations validateCreationToken(const CreationToken& value, std::string_view field = "creation_token");
[[nodiscard]] TimingEditContractViolations validateStableNodeRef(const StableNodeRef& value, std::string_view field = "node_ref");
[[nodiscard]] TimingEditContractViolations validateStableNetRef(const StableNetRef& value, std::string_view field = "net_ref");
[[nodiscard]] TimingEditContractViolations validateStableArcRef(const StableArcRef& value, std::string_view field = "arc_ref");
[[nodiscard]] TimingEditContractViolations validateBaselineSemanticIdentity(const BaselineSemanticIdentity& value,
                                                                             std::string_view field = "baseline");
[[nodiscard]] TimingEditContractViolations validateCommittedEditIdentity(const CommittedEditIdentity& value,
                                                                          std::string_view field = "committed_identity");
[[nodiscard]] TimingEditContractViolations validateEditBatch(const EditBatch& value);
[[nodiscard]] EditBatchNormalization normalizeEditBatch(EditBatch value);
[[nodiscard]] TimingEditContractViolations validateDesignEditPlan(const DesignEditPlan& value);
[[nodiscard]] DesignEditPlanNormalization normalizeDesignEditPlan(DesignEditPlan value);
[[nodiscard]] TimingEditContractViolations validateTimingGraphDelta(const TimingGraphDelta& value, std::string_view field = "delta");
[[nodiscard]] TimingGraphDeltaNormalization normalizeTimingGraphDelta(TimingGraphDelta value, std::string_view field = "delta");
[[nodiscard]] TimingEditContractViolations validatePreparedBatchData(const PreparedBatchData& value);

[[nodiscard]] TokenMaterialization defaultTokenMaterialization(const CreationToken& token, const LibraryCellRef& library_cell);

}  // namespace ista::timing_edit
