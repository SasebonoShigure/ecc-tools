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

#include "CsrNativeGraphOracle.hpp"
#include "NativeGraphProjection.hpp"

namespace ista::csr {

[[nodiscard]] std::vector<std::uint8_t> canonicalNativeGraphProjectionBytes(const NativeGraphProjection& projection);
[[nodiscard]] std::vector<std::uint8_t> canonicalNativeGraphOracleBytes(const CsrNativeGraphOracle& oracle);

}  // namespace ista::csr
