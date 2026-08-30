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
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

inline constexpr auto kNativeGraphOracleSchema = "ista-csr-native-graph-oracle/1.0.0";

struct CsrNativeNodeLevel
{
  NodeKey node_key;
  std::int32_t native_level{0};

  friend bool operator==(const CsrNativeNodeLevel&, const CsrNativeNodeLevel&) = default;
};

struct CsrNativeGraphOracle
{
  std::string schema{kNativeGraphOracleSchema};
  std::vector<NodeKey> native_timing_order;
  std::vector<CsrNativeNodeLevel> native_node_levels;

  friend bool operator==(const CsrNativeGraphOracle&, const CsrNativeGraphOracle&) = default;
};

}  // namespace ista::csr
