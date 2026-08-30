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
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Arc.hpp"
#include "CsrFingerprint.hpp"
#include "CsrNativeGraphTestSupport.hpp"
#include "CsrNativeSemanticKeyTestSupport.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "Pin.hpp"
#include "TimingCell.hpp"

namespace ista::csr::test {
namespace {

bool bytesLess(const std::vector<std::uint8_t>& lhs, const std::vector<std::uint8_t>& rhs)
{
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

PinDirection convertDirection(ista::PinDirection direction)
{
  switch (direction) {
    case ista::PinDirection::kNone:
      return PinDirection::kNone;
    case ista::PinDirection::kInput:
      return PinDirection::kInput;
    case ista::PinDirection::kOutput:
      return PinDirection::kOutput;
    case ista::PinDirection::kInout:
      return PinDirection::kInout;
  }
  throw std::runtime_error("direct projector: unknown pin direction");
}

bool containsName(const std::vector<std::string>& names, const std::string& name)
{
  return std::find(names.begin(), names.end(), name) != names.end();
}

NativeGraphNodeProjection makeNodeProjection(const Database& database, const Pin& pin)
{
  NativeGraphNodeProjection projection;
  projection.key = nativeSemanticNodeKey(pin);
  projection.direction = convertDirection(pin.get_direction());
  projection.net_name = pin.get_net_name();
  projection.is_port = pin.get_is_port();
  projection.is_startpoint = containsName(database.get_start_point_list(), pin.get_full_name());
  projection.is_endpoint = containsName(database.get_end_point_list(), pin.get_full_name());
  if (!pin.get_is_port()) {
    const Instance& instance = database.get_instance_map().at(pin.get_instance_name());
    projection.instance_cell = instance.get_cell_name();
    projection.is_sequential = instance.get_is_sequential();
    projection.is_clock_pin = instance.get_clock_pin_name() == pin.get_full_name();
    projection.is_data_pin = instance.get_data_pin_name() == pin.get_full_name();
    projection.is_output_pin = instance.get_output_pin_name() == pin.get_full_name();
  }
  return projection;
}

std::vector<NativeGraphNodeProjection> makeNodes(const Database& database)
{
  std::vector<NativeGraphNodeProjection> nodes;
  nodes.reserve(database.get_pin_map().size());
  for (const auto& [map_name, pin] : database.get_pin_map()) {
    static_cast<void>(map_name);
    nodes.push_back(makeNodeProjection(database, pin));
  }
  std::sort(nodes.begin(), nodes.end(),
            [](const auto& lhs, const auto& rhs) { return bytesLess(canonicalNodeKeyBytes(lhs.key), canonicalNodeKeyBytes(rhs.key)); });
  return nodes;
}

std::vector<NativeGraphInstanceProjection> makeInstances(const Database& database)
{
  std::vector<NativeGraphInstanceProjection> instances;
  for (const auto& [map_name, instance] : database.get_instance_map()) {
    NativeGraphInstanceProjection projection;
    projection.instance_name = map_name;
    projection.cell_name = instance.get_cell_name();
    for (const std::string& pin_name : instance.get_pin_name_list()) {
      projection.ordered_pins.push_back(nativeSemanticNodeKey(database, pin_name));
    }
    projection.is_sequential = instance.get_is_sequential();
    projection.has_timing_checks = !instance.get_check_arc_list().empty();
    projection.is_clock_gating = instance.get_is_clock_gating();
    const auto cell = database.get_timing_library().get_cell_map().find(instance.get_cell_name());
    projection.is_macro = cell != database.get_timing_library().get_cell_map().end() && cell->second.get_is_macro();
    instances.push_back(std::move(projection));
  }
  return instances;
}

std::vector<NativeGraphArcProjection> makeArcs(const Database& database, const std::vector<ArcKey>& keys)
{
  std::vector<NativeGraphArcProjection> arcs;
  arcs.reserve(keys.size());
  for (std::size_t index = 0; index < keys.size(); ++index) {
    const Arc& arc = database.get_arc_list()[index];
    arcs.push_back(
        NativeGraphArcProjection{keys[index], static_cast<std::uint64_t>(index), arc.get_is_clock_arc(), arc.get_is_disable_arc(), arc.get_is_loop_disable()});
  }
  std::sort(arcs.begin(), arcs.end(), [](const auto& lhs, const auto& rhs) { return bytesLess(canonicalArcKeyBytes(lhs.key), canonicalArcKeyBytes(rhs.key)); });
  return arcs;
}

std::vector<NativeGraphAdjacencyRow> makeRows(const std::vector<NativeGraphNodeProjection>& nodes,
                                              const std::map<std::string, std::vector<std::size_t>>& native_rows, const std::vector<ArcKey>& keys)
{
  std::vector<NativeGraphAdjacencyRow> rows;
  rows.reserve(nodes.size());
  for (const auto& node : nodes) {
    NativeGraphAdjacencyRow row;
    row.node = node.key;
    const auto found = native_rows.find(node.key.full_name);
    if (found != native_rows.end()) {
      for (std::size_t arc_index : found->second) {
        row.ordered_arcs.push_back(keys.at(arc_index));
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<NodeKey> mapNodeNames(const Database& database, const std::vector<std::string>& names)
{
  std::vector<NodeKey> keys;
  keys.reserve(names.size());
  for (const std::string& name : names) {
    keys.push_back(nativeSemanticNodeKey(database, name));
  }
  return keys;
}

std::vector<NodeKey> makeRoots(const Database& database)
{
  std::vector<NodeKey> roots;
  for (const auto& [name, point] : database.get_timing_point_map()) {
    static_cast<void>(point);
    std::size_t indegree = 0;
    const auto row = database.get_incoming_arc_list_map().find(name);
    if (row != database.get_incoming_arc_list_map().end()) {
      for (std::size_t arc_index : row->second) {
        const Arc& arc = database.get_arc_list().at(arc_index);
        if (!arc.get_is_disable_arc() && !arc.get_is_loop_disable()) {
          ++indegree;
        }
      }
    }
    if (indegree == 0) {
      roots.push_back(nativeSemanticNodeKey(database, name));
    }
  }
  return roots;
}

}  // namespace

NativeGraphProjection captureDirectNativeGraphProjection(const Database& database)
{
  NativeGraphProjection projection;
  projection.nodes = makeNodes(database);
  projection.instances = makeInstances(database);
  const std::vector<ArcKey> keys = nativeSemanticArcKeys(database);
  projection.arcs = makeArcs(database, keys);
  projection.base_out = makeRows(projection.nodes, database.get_outgoing_arc_list_map(), keys);
  projection.base_in = makeRows(projection.nodes, database.get_incoming_arc_list_map(), keys);
  projection.ordered_roots = makeRoots(database);
  projection.ordered_starts = mapNodeNames(database, database.get_start_point_list());
  projection.ordered_ends = mapNodeNames(database, database.get_end_point_list());
  return projection;
}

}  // namespace ista::csr::test
