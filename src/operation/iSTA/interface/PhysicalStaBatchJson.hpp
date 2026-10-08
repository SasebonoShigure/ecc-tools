// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "PhysicalTimingEdit.hpp"
#include <utility>

namespace ista {
struct PhysicalStaRecoveryBatch
{
  timing_edit::EditBatch batch;
  std::vector<PhysicalBufferLocation> locations;
  StaExecutionRecord execution;
  std::string levelization;
};
// Production requests contain only expected generation and located semantic edits.
[[nodiscard]] std::pair<timing_edit::EditBatch, std::vector<PhysicalBufferLocation>>
decodePhysicalStaBatchRequest(std::string_view bytes);
// Baseline/configuration identity is bound once by the journal. Every record
// names it but stores only this generation's edits and actual execution.
[[nodiscard]] std::string encodePhysicalStaRecoveryBatch(std::string_view baseline_id,
    const timing_edit::EditBatch& batch, std::span<const PhysicalBufferLocation> locations,
    const StaExecutionRecord& execution, std::string_view levelization, std::int32_t dbu_per_um);
[[nodiscard]] PhysicalStaRecoveryBatch decodePhysicalStaRecoveryBatch(std::string_view bytes, std::string_view baseline_id,
    std::uint64_t expected_generation, const StaExecutionRequest& request, std::int32_t dbu_per_um);
}  // namespace ista
