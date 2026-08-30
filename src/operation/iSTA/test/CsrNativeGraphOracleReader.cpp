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
#include <cstddef>
#include <cstdint>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include "Arc.hpp"
#include "CsrFingerprint.hpp"
#include "CsrNativeGraphTestSupport.hpp"
#include "CsrNativeSemanticKeyTestSupport.hpp"
#include "Database.hpp"
#include "TimingPoint.hpp"

namespace ista::csr::test {
namespace {

bool nodeLess(const CsrNativeNodeLevel& lhs, const CsrNativeNodeLevel& rhs)
{
  const auto lhs_bytes = canonicalNodeKeyBytes(lhs.node_key);
  const auto rhs_bytes = canonicalNodeKeyBytes(rhs.node_key);
  return std::lexicographical_compare(lhs_bytes.begin(), lhs_bytes.end(), rhs_bytes.begin(), rhs_bytes.end());
}

bool active(const Arc& arc)
{
  return !arc.get_is_disable_arc() && !arc.get_is_loop_disable();
}

std::size_t activeIndegree(const Database& database, const std::string& name)
{
  std::size_t indegree = 0;
  const auto found = database.get_incoming_arc_list_map().find(name);
  if (found == database.get_incoming_arc_list_map().end()) {
    return indegree;
  }
  for (std::size_t arc_index : found->second) {
    if (active(database.get_arc_list().at(arc_index))) {
      ++indegree;
    }
  }
  return indegree;
}

void appendCanonicalLevels(const Database& database, const std::map<std::string, std::int32_t>& levels, CsrNativeGraphOracle& oracle)
{
  for (const auto& [name, point] : database.get_timing_point_map()) {
    static_cast<void>(point);
    oracle.native_node_levels.push_back(CsrNativeNodeLevel{nativeSemanticNodeKey(database, name), levels.at(name)});
  }
  std::sort(oracle.native_node_levels.begin(), oracle.native_node_levels.end(), nodeLess);
}

}  // namespace

CsrNativeGraphOracle readNativeGraphOracle(const Database& database)
{
  CsrNativeGraphOracle oracle;
  oracle.native_timing_order.reserve(database.get_timing_order_list().size());
  for (const std::string& name : database.get_timing_order_list()) {
    oracle.native_timing_order.push_back(nativeSemanticNodeKey(database, name));
  }
  for (const auto& [name, timing_point] : database.get_timing_point_map()) {
    oracle.native_node_levels.push_back(CsrNativeNodeLevel{nativeSemanticNodeKey(database, name), timing_point.get_level()});
  }
  std::sort(oracle.native_node_levels.begin(), oracle.native_node_levels.end(), nodeLess);
  return oracle;
}

IndependentFifoOracle runIndependentFifoOracle(const Database& database)
{
  IndependentFifoOracle result;
  std::map<std::string, std::size_t> indegrees;
  std::map<std::string, std::int32_t> levels;
  std::queue<std::string> ready;

  for (const auto& [name, point] : database.get_timing_point_map()) {
    static_cast<void>(point);
    const std::size_t indegree = activeIndegree(database, name);
    indegrees.emplace(name, indegree);
    levels.emplace(name, indegree == 0 ? 1 : 0);
    if (indegree == 0) {
      ready.push(name);
    }
  }

  while (!ready.empty()) {
    std::string source = ready.front();
    ready.pop();
    result.oracle.native_timing_order.push_back(nativeSemanticNodeKey(database, source));

    const auto row = database.get_outgoing_arc_list_map().find(source);
    if (row == database.get_outgoing_arc_list_map().end()) {
      continue;
    }
    for (std::size_t arc_index : row->second) {
      const Arc& arc = database.get_arc_list().at(arc_index);
      if (!active(arc)) {
        continue;
      }
      auto sink_level = levels.find(arc.get_sink_pin());
      auto sink_indegree = indegrees.find(arc.get_sink_pin());
      if (sink_level == levels.end() || sink_indegree == indegrees.end()) {
        throw std::runtime_error("independent FIFO: arc sink has no timing point");
      }
      sink_level->second = std::max(sink_level->second, levels.at(source) + 1);
      if (sink_indegree->second > 0) {
        --sink_indegree->second;
      }
      if (sink_indegree->second == 0) {
        ready.push(arc.get_sink_pin());
      }
    }
  }

  appendCanonicalLevels(database, levels, result.oracle);
  result.has_active_cycle = result.oracle.native_timing_order.size() != database.get_timing_point_map().size();
  return result;
}

}  // namespace ista::csr::test
