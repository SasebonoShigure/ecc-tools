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

#include "CsrRealDifferentialOracleEncoding.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrNativeSemanticKeyTestSupport.hpp"
#include "Database.hpp"

namespace ista::csr::real_diff {
namespace {

NeutralClockState convertClock(const CsrNativeClockTimingState& source)
{
  return {source.arrivals, source.slews, source.predecessor_nodes, source.predecessor_arc_delays, source.predecessor_transitions};
}

NeutralPath convertPath(const CsrNativePathTimingState& source)
{
  NeutralPath result;
  result.analysis = source.analysis;
  result.source = source.source;
  result.lane_transition = source.lane_transition;
  result.clock_tag = source.clock_tag;
  result.arrival = source.arrival;
  result.slew = source.slew;
  result.launch = source.launch;
  result.start_node = source.start_node;
  result.predecessor_node = source.predecessor_node;
  result.predecessor_arc = source.predecessor_arc;
  result.predecessor_arc_delay = source.predecessor_arc_delay;
  result.clock_name = source.clock_name;
  result.state_transition = source.state_transition;
  result.predecessor_transition = source.predecessor_transition;
  result.crpr_node = source.crpr_node;
  result.crpr_transition = source.crpr_transition;
  return result;
}

NeutralNode convertNode(const CsrNativeTimingNode& source)
{
  NeutralNode result;
  result.key = source.key;
  result.level = source.level;
  result.is_clock_point = source.is_clock_point;
  result.arrival = source.arrival;
  result.required = source.required;
  result.slack = source.slack;
  result.launch = source.launch;
  result.clock_name = source.clock_name;
  result.predecessor_node = source.predecessor_node;
  result.predecessor_arc = source.predecessor_arc;
  result.effective_clock = convertClock(source.effective_clock);
  result.physical_clock = convertClock(source.physical_clock);
  result.data_slews = source.data_slews;
  for (const CsrNativePathTimingState& path : source.paths) {
    result.paths.push_back(convertPath(path));
  }
  return result;
}

NeutralArc convertArc(const CsrNativeTimingArc& source)
{
  NeutralArc result;
  result.key = source.key;
  result.delay = source.delay;
  result.min_delay = source.min_delay;
  result.max_delay = source.max_delay;
  result.transition_delays = source.transition_delays;
  result.input_output_delays = source.input_output_delays;
  result.graph_delays = source.graph_delays;
  result.transition_types = source.transition_types;
  for (const CsrNativeTimingArcEntry& entry : source.timing_arcs) {
    NeutralArcEntry converted;
    converted.timing_arc_index = entry.timing_arc_index;
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t input = 0; input < kTransitionCount; ++input) {
        for (std::size_t output = 0; output < kTransitionCount; ++output) {
          if (entry.lanes[analysis][input][output]) {
            const CsrNativeTimingArcDelay& value = *entry.lanes[analysis][input][output];
            converted.lanes[analysis][input][output] = NeutralArcDelay{value.delay, value.is_fallback};
          }
        }
      }
    }
    result.timing_arcs.push_back(std::move(converted));
  }
  return result;
}

NeutralAdjacencyRow makeRow(const Database& database, const NodeKey& node, const std::vector<ArcKey>& keys, bool outgoing)
{
  NeutralAdjacencyRow row;
  row.node = node;
  const auto& rows = outgoing ? database.get_outgoing_arc_list_map() : database.get_incoming_arc_list_map();
  const auto found = rows.find(node.full_name);
  if (found != rows.end()) {
    for (std::size_t rank : found->second) {
      if (rank >= keys.size()) {
        throw std::runtime_error("opaque oracle: adjacency rank is out of range");
      }
      row.arcs.push_back(keys[rank]);
    }
  }
  return row;
}

}  // namespace

NeutralWire readAndEncodeNativeTiming(const Database& database)
{
  const CsrNativeTimingOracle oracle = test::readNativeTimingOracle(database);
  if (oracle.schema != kNativeTimingOracleSchema || oracle.nodes.size() != database.get_timing_point_map().size()
      || oracle.arcs.size() != database.get_arc_list().size()) {
    throw std::runtime_error("opaque oracle: native DTO root contract differs");
  }
  NeutralWire result;
  result.timing_order = oracle.native_timing_order;
  std::map<std::string, NodeKey, std::less<>> node_keys;
  for (const CsrNativeTimingNode& node : oracle.nodes) {
    result.nodes.push_back(convertNode(node));
    if (!node_keys.emplace(node.key.full_name, node.key).second) {
      throw std::runtime_error("opaque oracle: duplicate node full name");
    }
  }
  for (const CsrNativeTimingArc& arc : oracle.arcs) {
    result.arcs.push_back(convertArc(arc));
  }
  const std::vector<ArcKey> rank_keys = test::nativeSemanticArcKeys(database);
  for (const NeutralNode& node : result.nodes) {
    result.out_rows.push_back(makeRow(database, node.key, rank_keys, true));
    result.in_rows.push_back(makeRow(database, node.key, rank_keys, false));
  }
  return result;
}

}  // namespace ista::csr::real_diff
