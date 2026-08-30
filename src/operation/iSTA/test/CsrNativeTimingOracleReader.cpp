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
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "CsrNativeSemanticKeyTestSupport.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "Database.hpp"

namespace ista::csr::test {
namespace {

bool bytesLess(const std::vector<std::uint8_t>& lhs, const std::vector<std::uint8_t>& rhs)
{
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

const NodeKey& requireNodeKey(const CsrNativeTimingNodeKeyLookup& lookup, std::string_view full_name)
{
  const auto found = lookup.find(full_name);
  if (found == lookup.end()) {
    throw std::runtime_error("native timing oracle: node name does not resolve");
  }
  return found->second;
}

}  // namespace

CsrNativeTimingOracle readNativeTimingOracle(const Database& database)
{
  CsrNativeTimingNodeKeyLookup node_keys;
  for (const auto& [map_name, pin] : database.get_pin_map()) {
    static_cast<void>(map_name);
    NodeKey key = nativeSemanticNodeKey(pin);
    const std::string full_name = key.full_name;
    if (!node_keys.emplace(full_name, std::move(key)).second) {
      throw std::runtime_error("native timing oracle: duplicate native node full name");
    }
  }

  const CsrNativeTimingArcKeyLookup arc_keys = nativeSemanticArcKeys(database);
  if (arc_keys.size() != database.get_arc_list().size()) {
    throw std::runtime_error("native timing oracle: native Arc key cardinality mismatch");
  }

  CsrNativeTimingOracle oracle;
  oracle.native_timing_order.reserve(database.get_timing_order_list().size());
  for (const std::string& full_name : database.get_timing_order_list()) {
    oracle.native_timing_order.push_back(requireNodeKey(node_keys, full_name));
  }

  oracle.nodes.reserve(database.get_timing_point_map().size());
  for (const auto& [full_name, timing_point] : database.get_timing_point_map()) {
    oracle.nodes.push_back(readNativeTimingNode(full_name, timing_point, node_keys, arc_keys));
  }
  std::sort(oracle.nodes.begin(), oracle.nodes.end(), [](const CsrNativeTimingNode& lhs, const CsrNativeTimingNode& rhs) {
    return bytesLess(canonicalNodeKeyBytes(lhs.key), canonicalNodeKeyBytes(rhs.key));
  });

  oracle.arcs.reserve(database.get_arc_list().size());
  for (std::size_t native_rank = 0; native_rank < database.get_arc_list().size(); ++native_rank) {
    oracle.arcs.push_back(readNativeTimingArc(database.get_arc_list()[native_rank], native_rank, arc_keys));
  }
  std::sort(oracle.arcs.begin(), oracle.arcs.end(), [](const CsrNativeTimingArc& lhs, const CsrNativeTimingArc& rhs) {
    return bytesLess(canonicalArcKeyBytes(lhs.key), canonicalArcKeyBytes(rhs.key));
  });
  return oracle;
}

}  // namespace ista::csr::test
