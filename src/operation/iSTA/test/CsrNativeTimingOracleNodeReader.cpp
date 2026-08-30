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
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#include "AnalysisType.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "PathSourceType.hpp"
#include "TimingPathState.hpp"
#include "TimingPoint.hpp"
#include "TimingSemanticKey.hpp"
#include "TransType.hpp"

namespace ista::csr::test {
namespace {

std::size_t analysisIndex(AnalysisType analysis)
{
  switch (analysis) {
    case AnalysisType::kMax:
      return 0;
    case AnalysisType::kMin:
      return 1;
    case AnalysisType::kNone:
      break;
  }
  throw std::runtime_error("native timing oracle: unknown analysis lane");
}

AnalysisMode convertAnalysis(AnalysisType analysis)
{
  return analysisIndex(analysis) == 0 ? AnalysisMode::kMax : AnalysisMode::kMin;
}

std::size_t transitionIndex(TransType transition)
{
  switch (transition) {
    case TransType::kRise:
      return 0;
    case TransType::kFall:
      return 1;
    case TransType::kNone:
      break;
  }
  throw std::runtime_error("native timing oracle: unknown transition lane");
}

Transition convertTransition(TransType transition)
{
  switch (transition) {
    case TransType::kRise:
      return Transition::kRise;
    case TransType::kFall:
      return Transition::kFall;
    case TransType::kNone:
      return Transition::kNone;
  }
  throw std::runtime_error("native timing oracle: unknown transition value");
}

PathSource convertPathSource(PathSourceType source)
{
  switch (source) {
    case PathSourceType::kInput:
      return PathSource::kInput;
    case PathSourceType::kRegister:
      return PathSource::kRegister;
    case PathSourceType::kNone:
      break;
  }
  throw std::runtime_error("native timing oracle: unknown path-source lane");
}

double checkedDouble(double value)
{
  if (std::isnan(value)) {
    throw std::runtime_error("native timing oracle: NaN timing value");
  }
  return value;
}

const NodeKey& requireNodeKey(const CsrNativeTimingNodeKeyLookup& lookup, std::string_view full_name)
{
  const auto found = lookup.find(full_name);
  if (found == lookup.end()) {
    throw std::runtime_error("native timing oracle: stale native node reference");
  }
  return found->second;
}

const ArcKey& requireArcKey(const CsrNativeTimingArcKeyLookup& lookup, std::size_t native_rank)
{
  if (native_rank >= lookup.size()) {
    throw std::runtime_error("native timing oracle: predecessor Arc rank is out of range");
  }
  return lookup[native_rank];
}

template <typename NativeValue, typename OracleValue, typename Convert>
void importLaneMap(const std::map<AnalysisType, std::map<TransType, NativeValue>>& source, CsrNativeTimingLaneMatrix<OracleValue>& destination, Convert convert)
{
  for (const auto& [analysis, transitions] : source) {
    const std::size_t analysis_index = analysisIndex(analysis);
    for (const auto& [transition, value] : transitions) {
      destination[analysis_index][transitionIndex(transition)] = convert(value);
    }
  }
}

CsrNativeClockTimingState readClockState(const std::map<AnalysisType, std::map<TransType, double>>& arrivals,
                                         const std::map<AnalysisType, std::map<TransType, double>>& slews,
                                         const std::map<AnalysisType, std::map<TransType, std::string>>& predecessor_nodes,
                                         const std::map<AnalysisType, std::map<TransType, double>>& predecessor_arc_delays,
                                         const std::map<AnalysisType, std::map<TransType, TransType>>& predecessor_transitions,
                                         const CsrNativeTimingNodeKeyLookup& node_keys)
{
  CsrNativeClockTimingState state;
  importLaneMap(arrivals, state.arrivals, [](double value) { return checkedDouble(value); });
  importLaneMap(slews, state.slews, [](double value) { return checkedDouble(value); });
  importLaneMap(predecessor_nodes, state.predecessor_nodes, [&](const std::string& full_name) { return requireNodeKey(node_keys, full_name); });
  importLaneMap(predecessor_arc_delays, state.predecessor_arc_delays, [](double value) { return checkedDouble(value); });
  importLaneMap(predecessor_transitions, state.predecessor_transitions, [](TransType transition) { return convertTransition(transition); });
  return state;
}

CsrNativePathTimingState readPathState(AnalysisType analysis, PathSourceType source, TransType lane_transition, const std::string& clock_tag,
                                       const TimingPathState& native, const CsrNativeTimingNodeKeyLookup& node_keys,
                                       const CsrNativeTimingArcKeyLookup& arc_keys)
{
  CsrNativePathTimingState path;
  path.analysis = convertAnalysis(analysis);
  path.source = convertPathSource(source);
  transitionIndex(lane_transition);
  path.lane_transition = convertTransition(lane_transition);
  path.clock_tag = clock_tag;
  path.arrival = checkedDouble(native.get_arrival());
  path.slew = checkedDouble(native.get_slew());
  path.launch = checkedDouble(native.get_launch_time());
  path.start_node = requireNodeKey(node_keys, native.get_start_point());
  path.predecessor_arc_delay = checkedDouble(native.get_predecessor_arc_delay());
  path.clock_name = native.get_clock_name();
  path.state_transition = convertTransition(native.get_trans_type());
  path.predecessor_transition = convertTransition(native.get_predecessor_trans_type());
  path.crpr_transition = convertTransition(native.get_crpr_clock_trans_type());

  const bool has_predecessor_node = !native.get_predecessor().empty();
  const bool has_predecessor_arc = native.get_predecessor_arc_idx() != std::numeric_limits<std::size_t>::max();
  if (has_predecessor_node != has_predecessor_arc) {
    throw std::runtime_error("native timing oracle: path predecessor presence mismatch");
  }
  if (has_predecessor_node) {
    path.predecessor_node = requireNodeKey(node_keys, native.get_predecessor());
    path.predecessor_arc = requireArcKey(arc_keys, native.get_predecessor_arc_idx());
  }
  if (!native.get_crpr_clock_pin().empty()) {
    path.crpr_node = requireNodeKey(node_keys, native.get_crpr_clock_pin());
  }
  return path;
}

bool byteStringLess(std::string_view lhs, std::string_view rhs)
{
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                      [](char left, char right) { return static_cast<unsigned char>(left) < static_cast<unsigned char>(right); });
}

bool pathLess(const CsrNativePathTimingState& lhs, const CsrNativePathTimingState& rhs)
{
  const auto lhs_analysis = token(lhs.analysis);
  const auto rhs_analysis = token(rhs.analysis);
  if (lhs_analysis != rhs_analysis) {
    return lhs_analysis < rhs_analysis;
  }
  const auto lhs_source = token(lhs.source);
  const auto rhs_source = token(rhs.source);
  if (lhs_source != rhs_source) {
    return lhs_source < rhs_source;
  }
  const auto lhs_transition = token(lhs.lane_transition);
  const auto rhs_transition = token(rhs.lane_transition);
  if (lhs_transition != rhs_transition) {
    return lhs_transition < rhs_transition;
  }
  return byteStringLess(lhs.clock_tag, rhs.clock_tag);
}

}  // namespace

CsrNativeTimingNode readNativeTimingNode(std::string_view native_name, const TimingPoint& timing_point, const CsrNativeTimingNodeKeyLookup& node_keys,
                                         const CsrNativeTimingArcKeyLookup& arc_keys)
{
  CsrNativeTimingNode node;
  node.key = requireNodeKey(node_keys, native_name);
  node.level = timing_point.get_level();
  node.is_clock_point = timing_point.get_is_clock_point();
  node.arrival = checkedDouble(timing_point.get_arrival());
  node.required = checkedDouble(timing_point.get_required());
  node.slack = checkedDouble(timing_point.get_slack());
  node.launch = checkedDouble(timing_point.get_launch_time());
  if (const std::string_view clock_name = timing_point.get_clock_name(); !clock_name.empty()) {
    node.clock_name = std::string(clock_name);
  }

  const bool has_predecessor_node = !timing_point.get_predecessor().empty();
  const bool has_predecessor_arc = timing_point.get_predecessor_arc_idx() != std::numeric_limits<std::size_t>::max();
  if (has_predecessor_node != has_predecessor_arc) {
    throw std::runtime_error("native timing oracle: scalar predecessor presence mismatch");
  }
  if (has_predecessor_node) {
    node.predecessor_node = requireNodeKey(node_keys, timing_point.get_predecessor());
    node.predecessor_arc = requireArcKey(arc_keys, timing_point.get_predecessor_arc_idx());
  }

  node.effective_clock = readClockState(timing_point.get_clock_arrival_map(), timing_point.get_clock_slew_map(), timing_point.get_clock_predecessor_map(),
                                        timing_point.get_clock_predecessor_arc_delay_map(), timing_point.get_clock_predecessor_trans_type_map(), node_keys);
  node.physical_clock = readClockState(timing_point.get_physical_clock_arrival_map(), timing_point.get_physical_clock_slew_map(),
                                       timing_point.get_physical_clock_predecessor_map(), timing_point.get_physical_clock_predecessor_arc_delay_map(),
                                       timing_point.get_physical_clock_predecessor_trans_type_map(), node_keys);
  importLaneMap(timing_point.get_data_slew_map(), node.data_slews, [](double value) { return checkedDouble(value); });

  for (const auto& [analysis, sources] : timing_point.get_path_state_map()) {
    convertAnalysis(analysis);
    for (const auto& [source, transitions] : sources) {
      convertPathSource(source);
      for (const auto& [transition, tagged_states] : transitions) {
        transitionIndex(transition);
        for (const auto& [clock_tag, state] : tagged_states) {
          node.paths.push_back(readPathState(analysis, source, transition, clock_tag, state, node_keys, arc_keys));
        }
      }
    }
  }
  std::sort(node.paths.begin(), node.paths.end(), pathLess);
  return node;
}

}  // namespace ista::csr::test
