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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "CsrNativeTimingOracle.hpp"

namespace ista {
class Arc;
class Database;
class TimingPoint;
}  // namespace ista

namespace ista::csr::test {

using CsrNativeTimingNodeKeyLookup = std::map<std::string, NodeKey, std::less<>>;
using CsrNativeTimingArcKeyLookup = std::vector<ArcKey>;

// The top-level reader is the only timing-oracle component that constructs
// semantic keys. Node and Arc readers receive immutable lookups.
[[nodiscard]] CsrNativeTimingOracle readNativeTimingOracle(const ista::Database& database);
[[nodiscard]] CsrNativeTimingNode readNativeTimingNode(std::string_view native_name, const ista::TimingPoint& timing_point,
                                                       const CsrNativeTimingNodeKeyLookup& node_keys, const CsrNativeTimingArcKeyLookup& arc_keys);
[[nodiscard]] CsrNativeTimingArc readNativeTimingArc(const ista::Arc& arc, std::size_t native_rank, const CsrNativeTimingArcKeyLookup& arc_keys);

[[nodiscard]] std::vector<std::uint8_t> canonicalIndependentNativeTimingOracleBytes(const CsrNativeTimingOracle& oracle);

}  // namespace ista::csr::test
