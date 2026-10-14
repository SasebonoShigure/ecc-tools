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
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "TimingGraphCsrAdapterInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::detail {
namespace {
const ista::Pin& requirePin(const GraphBuildState& state, const std::string& name)
{
  const auto pin = state.database.get_pin_map().find(name);
  if (pin == state.database.get_pin_map().end())
    malformed();
  return pin->second;
}

NodeId requireNodeId(const GraphBuildState& state, const std::string& name)
{
  const auto node = state.node_ids_by_full_name.find(name);
  if (node == state.node_ids_by_full_name.end())
    malformed();
  return node->second;
}

const ista::TimingCell* findTimingCell(const GraphBuildState& state, const ista::Instance& instance)
{
  const auto cell = state.database.get_timing_library().get_cell_map().find(instance.get_cell_name());
  return cell == state.database.get_timing_library().get_cell_map().end() ? nullptr : std::addressof(cell->second);
}

bool isInputLike(PinDirection direction)
{
  return direction == PinDirection::kInput || direction == PinDirection::kInout;
}

bool isOutputLike(PinDirection direction)
{
  return direction == PinDirection::kOutput || direction == PinDirection::kInout;
}

using CellArcCounts = std::map<std::pair<std::string, std::string>, std::size_t>;
using CellArcsByOwner = std::unordered_map<std::string, CellArcCounts>;

CellArcsByOwner indexCellArcsByOwner(const GraphBuildState& state)
{
  CellArcsByOwner result;
  for (const auto& [name, instance] : state.database.get_instance_map()) {
    const auto* cell = findTimingCell(state, instance);
    if (cell == nullptr || cell->get_cell_arc_list().empty())
      result.try_emplace(name);
  }
  if (result.empty())
    return result;
  for (const auto& arc : state.database.get_arc_list()) {
    if (arc.get_type() != ista::ArcType::kCell)
      continue;
    const auto owner = result.find(arc.get_owner_name());
    if (owner == result.end())
      continue;
    auto [count, first] = owner->second.try_emplace({arc.get_source_pin(), arc.get_sink_pin()}, 0);
    static_cast<void>(first);
    ++count->second;
  }
  return result;
}

void validateFallbackCartesianProduct(const GraphBuildState& state, const std::string& name, const ista::Instance& instance,
                                      const CellArcsByOwner& arcs_by_owner)
{
  const auto* cell = findTimingCell(state, instance);
  if (cell != nullptr && !cell->get_cell_arc_list().empty())
    return;

  std::vector<std::string> input_like;
  std::vector<std::string> output_like;
  input_like.reserve(instance.get_pin_name_list().size());
  output_like.reserve(instance.get_pin_name_list().size());
  for (const auto& pin_name : instance.get_pin_name_list()) {
    const auto direction = convertPinDirection(requirePin(state, pin_name).get_direction());
    if (isInputLike(direction))
      input_like.push_back(pin_name);
    if (isOutputLike(direction))
      output_like.push_back(pin_name);
  }

  std::set<std::pair<std::string, std::string>> expected;
  for (const auto& source_name : input_like) {
    for (const auto& sink_name : output_like) {
      if (source_name != sink_name)
        expected.emplace(source_name, sink_name);
    }
  }

  const auto actual = arcs_by_owner.find(name);
  const std::size_t actual_size = actual == arcs_by_owner.end() ? 0 : actual->second.size();
  if (actual_size != expected.size())
    malformed();
  for (const auto& pair : expected) {
    if (actual == arcs_by_owner.end())
      malformed();
    const auto match = actual->second.find(pair);
    if (match == actual->second.end() || match->second != 1)
      malformed();
  }
}

void assignCanonicalArcs(GraphBuildState& state)
{
  const auto& native_arcs = state.database.get_arc_list();
  std::vector<std::size_t> order(state.native_arc_keys.size());
  std::iota(order.begin(), order.end(), 0);
  std::ranges::sort(order, [&](std::size_t lhs, std::size_t rhs) { return arcKeyLess(state.native_arc_keys[lhs], state.native_arc_keys[rhs]); });
  state.arc_ids_by_native_rank.resize(order.size());
  for (std::size_t canonical = 0; canonical < order.size(); ++canonical) {
    const std::size_t native = order[canonical];
    if (native >= native_arcs.size())
      malformed();
    if (canonical > 0 && state.native_arc_keys[order[canonical - 1]] == state.native_arc_keys[native])
      collision();
    state.arc_ids_by_native_rank[native] = ArcId{canonical};
    const auto& native_arc = native_arcs.at(native);
    const auto& key = state.native_arc_keys[native];
    state.snapshot.arcs.push_back({ArcId{canonical}, key, requireNodeId(state, native_arc.get_source_pin()), requireNodeId(state, native_arc.get_sink_pin()),
                                   key.base.kind, native, native_arc.get_is_clock_arc(), native_arc.get_is_disable_arc(), native_arc.get_is_loop_disable()});
    state.snapshot.arc_ids_by_key.push_back({key, ArcId{canonical}});
  }
}

}  // namespace

void exportArcs(GraphBuildState& state)
{
  std::vector<ArcKeyBase> bases;
  bases.reserve(state.database.get_arc_list().size());
  for (const auto& arc : state.database.get_arc_list())
    bases.push_back(projectCanonicalArcKeyBase(
        CanonicalArcKeyContext{state.database, state.snapshot.nodes,
                               state.node_ids_by_full_name,
                               state.ordered_pin_owners},
        arc));
  try {
    state.native_arc_keys = assignParallelOrdinals(bases);
  } catch (const std::invalid_argument&) {
    malformed();
  }
  const auto arcs_by_owner = indexCellArcsByOwner(state);
  for (const auto& [name, instance] : state.database.get_instance_map())
    validateFallbackCartesianProduct(state, name, instance, arcs_by_owner);
  assignCanonicalArcs(state);
}

}  // namespace ista::csr::detail
