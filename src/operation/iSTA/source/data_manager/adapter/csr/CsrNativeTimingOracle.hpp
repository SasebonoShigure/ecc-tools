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

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "CsrReferencePrimitives.hpp"

namespace ista::csr {

inline constexpr auto kNativeTimingOracleSchema = "ista-csr-native-timing-oracle/1.0.0";

template <typename T>
using CsrNativeTimingLaneMatrix = std::array<std::array<std::optional<T>, kTransitionCount>, kAnalysisCount>;

template <typename T>
using CsrNativeTimingArcLaneMatrix = std::array<std::array<std::array<std::optional<T>, kTransitionCount>, kTransitionCount>, kAnalysisCount>;

struct CsrNativeClockTimingState
{
  CsrNativeTimingLaneMatrix<double> arrivals;
  CsrNativeTimingLaneMatrix<double> slews;
  CsrNativeTimingLaneMatrix<NodeKey> predecessor_nodes;
  CsrNativeTimingLaneMatrix<double> predecessor_arc_delays;
  CsrNativeTimingLaneMatrix<Transition> predecessor_transitions;

  friend bool operator==(const CsrNativeClockTimingState&, const CsrNativeClockTimingState&) = default;
};

struct CsrNativePathTimingState
{
  AnalysisMode analysis{AnalysisMode::kMax};
  PathSource source{PathSource::kInput};
  Transition lane_transition{Transition::kRise};
  std::string clock_tag;
  double arrival{0.0};
  double slew{0.0};
  double launch{0.0};
  NodeKey start_node;
  std::optional<NodeKey> predecessor_node;
  std::optional<ArcKey> predecessor_arc;
  double predecessor_arc_delay{0.0};
  std::string clock_name;
  Transition state_transition{Transition::kNone};
  Transition predecessor_transition{Transition::kNone};
  std::optional<NodeKey> crpr_node;
  Transition crpr_transition{Transition::kNone};

  friend bool operator==(const CsrNativePathTimingState&, const CsrNativePathTimingState&) = default;
};

struct CsrNativeTimingNode
{
  NodeKey key;
  std::int32_t level{0};
  bool is_clock_point{false};
  double arrival{0.0};
  double required{0.0};
  double slack{0.0};
  double launch{0.0};
  std::optional<std::string> clock_name;
  std::optional<NodeKey> predecessor_node;
  std::optional<ArcKey> predecessor_arc;
  CsrNativeClockTimingState effective_clock;
  CsrNativeClockTimingState physical_clock;
  CsrNativeTimingLaneMatrix<double> data_slews;
  std::vector<CsrNativePathTimingState> paths;

  friend bool operator==(const CsrNativeTimingNode&, const CsrNativeTimingNode&) = default;
};

struct CsrNativeTimingArcDelay
{
  double delay{0.0};
  bool is_fallback{false};

  friend bool operator==(const CsrNativeTimingArcDelay&, const CsrNativeTimingArcDelay&) = default;
};

struct CsrNativeTimingArcEntry
{
  std::int32_t timing_arc_index{0};
  CsrNativeTimingArcLaneMatrix<CsrNativeTimingArcDelay> lanes;

  friend bool operator==(const CsrNativeTimingArcEntry&, const CsrNativeTimingArcEntry&) = default;
};

struct CsrNativeTimingArc
{
  ArcKey key;
  double delay{0.0};
  double min_delay{0.0};
  double max_delay{0.0};
  CsrNativeTimingLaneMatrix<double> transition_delays;
  CsrNativeTimingArcLaneMatrix<double> input_output_delays;
  CsrNativeTimingArcLaneMatrix<double> graph_delays;
  std::array<std::optional<Transition>, kTransitionCount> transition_types;
  std::vector<CsrNativeTimingArcEntry> timing_arcs;

  friend bool operator==(const CsrNativeTimingArc&, const CsrNativeTimingArc&) = default;
};

struct CsrNativeTimingOracle
{
  std::string schema{kNativeTimingOracleSchema};
  std::vector<NodeKey> native_timing_order;
  std::vector<CsrNativeTimingNode> nodes;
  std::vector<CsrNativeTimingArc> arcs;

  friend bool operator==(const CsrNativeTimingOracle&, const CsrNativeTimingOracle&) = default;
};

}  // namespace ista::csr
