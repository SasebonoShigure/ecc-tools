// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "TimingEditTypes.hpp"

namespace ista {
class Database;
class TimingCell;
}

namespace ista::timing_edit {

// Resolves the exact referenced native Liberty cell and proves the Stage 3
// supported shape: a two-port identity function with one matching enabled
// graph arc whose authoritative CSR signatures are explicitly positive-unate.
[[nodiscard]] const TimingCell& requireLegalNativeBufferCell(const Database& database, const LibraryCellRef& reference);

}  // namespace ista::timing_edit
