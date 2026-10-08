// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "PreparedPhysicalBatch.hpp"
#include "StaExecutionContract.hpp"
#include "TimingEditResolver.hpp"

namespace ista {
using izh::PreparedPhysicalBatch;
struct PhysicalBufferLocation
{
  ista::timing_edit::CreationToken token;
  std::int32_t x_dbu{}, y_dbu{};
  idb::IdbOrient orientation{idb::IdbOrient::kNone};
  friend bool operator==(const PhysicalBufferLocation&, const PhysicalBufferLocation&) = default;
};
// Called inside the common operation guard, after legalization. Resolves only
// affected nets; the prepared batch owns all new objects and local undo.
[[nodiscard]] PreparedPhysicalBatch preparePhysicalTimingEdit(
    idb::IdbDesign& design, const ista::timing_edit::EditBatch& edits,
    ista::timing_edit::TimingEditResolverView resolver,
    std::span<const PhysicalBufferLocation> locations, std::int32_t dbu_per_um);
// Only newly created native pins are written; their transaction already owns
// deletion on failure. Match the baseline STA wrapper's DBU coordinate convention.
void copyCreatedPhysicalPinCoordinates(const PreparedPhysicalBatch& physical, ista::Database& database);
}  // namespace ista
