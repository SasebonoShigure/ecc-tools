// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <optional>

#include "AffectedCapturePlan.hpp"

namespace ista {
class Database;
}

namespace ista::timing_dual_csr::detail {

[[nodiscard]] std::optional<NarrowCaptureError>
validateNativeDataOnlyNets(const ista::Database& database,
                           const AffectedCapturePlan& plan);

}  // namespace ista::timing_dual_csr::detail
