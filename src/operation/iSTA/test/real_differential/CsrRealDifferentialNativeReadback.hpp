// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "CsrRealDifferentialWire.hpp"
#include "CsrReferenceTypes.hpp"

namespace ista {
class Arc;
class Database;
class TimingPoint;
}  // namespace ista

namespace ista::csr::real_diff {

using NativeNodeKeyLookup = std::map<std::string, NodeKey, std::less<>>;

struct NativeReadbackKeys
{
  NativeNodeKeyLookup node_keys;
  std::vector<ArcKey> arc_keys_by_rank;
};

[[nodiscard]] NeutralWire readNativeTiming(const Database& database, const CsrReferenceInput& input);
[[nodiscard]] NeutralNode readNativeTimingNode(std::string_view native_name, const TimingPoint& point, const NativeReadbackKeys& keys);
[[nodiscard]] NeutralArc readNativeTimingArc(const Arc& arc, std::size_t native_rank, const NativeReadbackKeys& keys);

[[nodiscard]] const NodeKey& requireNativeNodeKey(const NativeReadbackKeys& keys, std::string_view full_name);
[[nodiscard]] const ArcKey& requireNativeArcKey(const NativeReadbackKeys& keys, std::size_t native_rank);

}  // namespace ista::csr::real_diff
