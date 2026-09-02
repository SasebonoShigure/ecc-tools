// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>

namespace ista::timing_dual_csr::detail {

// Test/benchmark evidence reads snapshots around the targeted interval. The
// production factory owns the increment, so a caller cannot self-report zero.
[[nodiscard]] std::uint64_t currentBaselineFactoryCallCount() noexcept;

}  // namespace ista::timing_dual_csr::detail
