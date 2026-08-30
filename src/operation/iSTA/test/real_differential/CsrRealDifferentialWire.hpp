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
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferencePrimitives.hpp"
#include "json.hpp"

namespace ista::csr::real_diff {

using Json = nlohmann::json;

template <typename T>
using NeutralLaneMatrix = std::array<std::array<std::optional<T>, kTransitionCount>, kAnalysisCount>;

template <typename T>
using NeutralArcLaneMatrix = std::array<std::array<std::array<std::optional<T>, kTransitionCount>, kTransitionCount>, kAnalysisCount>;

struct NeutralClockState
{
  NeutralLaneMatrix<double> arrivals;
  NeutralLaneMatrix<double> slews;
  NeutralLaneMatrix<NodeKey> predecessor_nodes;
  NeutralLaneMatrix<double> predecessor_arc_delays;
  NeutralLaneMatrix<Transition> predecessor_transitions;

  friend bool operator==(const NeutralClockState&, const NeutralClockState&) = default;
};

struct NeutralPath
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

  friend bool operator==(const NeutralPath&, const NeutralPath&) = default;
};

struct NeutralNode
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
  NeutralClockState effective_clock;
  NeutralClockState physical_clock;
  NeutralLaneMatrix<double> data_slews;
  std::vector<NeutralPath> paths;

  friend bool operator==(const NeutralNode&, const NeutralNode&) = default;
};

struct NeutralArcDelay
{
  double delay{0.0};
  bool is_fallback{false};

  friend bool operator==(const NeutralArcDelay&, const NeutralArcDelay&) = default;
};

struct NeutralArcEntry
{
  std::int32_t timing_arc_index{0};
  NeutralArcLaneMatrix<NeutralArcDelay> lanes;

  friend bool operator==(const NeutralArcEntry&, const NeutralArcEntry&) = default;
};

struct NeutralArc
{
  ArcKey key;
  double delay{0.0};
  double min_delay{0.0};
  double max_delay{0.0};
  NeutralLaneMatrix<double> transition_delays;
  NeutralArcLaneMatrix<double> input_output_delays;
  NeutralArcLaneMatrix<double> graph_delays;
  std::array<std::optional<Transition>, kTransitionCount> transition_types;
  std::vector<NeutralArcEntry> timing_arcs;

  friend bool operator==(const NeutralArc&, const NeutralArc&) = default;
};

struct NeutralAdjacencyRow
{
  NodeKey node;
  std::vector<ArcKey> arcs;

  friend bool operator==(const NeutralAdjacencyRow&, const NeutralAdjacencyRow&) = default;
};

struct NeutralWire
{
  std::vector<NodeKey> timing_order;
  std::vector<NeutralAdjacencyRow> out_rows;
  std::vector<NeutralAdjacencyRow> in_rows;
  std::vector<NeutralNode> nodes;
  std::vector<NeutralArc> arcs;

  friend bool operator==(const NeutralWire&, const NeutralWire&) = default;
};

[[nodiscard]] Json parseStrictJson(std::string_view bytes, std::string_view label);
void requireExactKeys(const Json& value, std::initializer_list<std::string_view> keys, std::string_view label);

[[nodiscard]] Json numberToJson(double value);
[[nodiscard]] double numberFromJson(const Json& value, std::string_view label);
[[nodiscard]] Json nodeKeyToJson(const NodeKey& key);
[[nodiscard]] NodeKey nodeKeyFromJson(const Json& value, std::string_view label);
[[nodiscard]] Json arcKeyToJson(const ArcKey& key);
[[nodiscard]] ArcKey arcKeyFromJson(const Json& value, std::string_view label);

[[nodiscard]] Json neutralWireToJson(const NeutralWire& wire);
[[nodiscard]] NeutralWire neutralWireFromJson(const Json& value);
[[nodiscard]] std::string encodeNeutralWire(const NeutralWire& wire);
[[nodiscard]] NeutralWire decodeNeutralWire(std::string_view bytes);

}  // namespace ista::csr::real_diff
