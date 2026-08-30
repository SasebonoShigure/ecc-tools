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
#include <variant>
#include <vector>

#include "CsrGraphTypes.hpp"

namespace ista::csr {

inline constexpr auto kNativeGraphProjectionSchema = "ista-csr-native-graph-projection/1.0.0";

struct NativeGraphNodeProjection
{
  NodeKey key;
  PinDirection direction{PinDirection::kNone};
  std::string instance_cell;
  std::string net_name;
  bool is_port{false};
  bool is_startpoint{false};
  bool is_endpoint{false};
  bool is_sequential{false};
  bool is_clock_pin{false};
  bool is_data_pin{false};
  bool is_output_pin{false};
  friend bool operator==(const NativeGraphNodeProjection&, const NativeGraphNodeProjection&) = default;
};

struct NativeGraphInstanceProjection
{
  std::string instance_name;
  std::string cell_name;
  std::vector<NodeKey> ordered_pins;
  bool is_sequential{false};
  bool has_timing_checks{false};
  bool is_clock_gating{false};
  bool is_macro{false};
  friend bool operator==(const NativeGraphInstanceProjection&, const NativeGraphInstanceProjection&) = default;
};

struct NativeGraphArcProjection
{
  ArcKey key;
  std::uint64_t native_traversal_rank{0};
  bool is_clock_arc{false};
  bool is_disabled{false};
  bool is_loop_disabled{false};
  friend bool operator==(const NativeGraphArcProjection&, const NativeGraphArcProjection&) = default;
};

struct NativeGraphAdjacencyRow
{
  NodeKey node;
  std::vector<ArcKey> ordered_arcs;
  friend bool operator==(const NativeGraphAdjacencyRow&, const NativeGraphAdjacencyRow&) = default;
};

struct NativeGraphProjection
{
  std::string schema{kNativeGraphProjectionSchema};
  std::vector<NativeGraphNodeProjection> nodes;
  std::vector<NativeGraphInstanceProjection> instances;
  std::vector<NativeGraphArcProjection> arcs;
  std::vector<NativeGraphAdjacencyRow> base_out;
  std::vector<NativeGraphAdjacencyRow> base_in;
  std::vector<NodeKey> ordered_roots;
  std::vector<NodeKey> ordered_starts;
  std::vector<NodeKey> ordered_ends;
  friend bool operator==(const NativeGraphProjection&, const NativeGraphProjection&) = default;
};

using NativeGraphProjectionResult = std::variant<NativeGraphProjection, CsrGraphExportFailure>;

}  // namespace ista::csr
