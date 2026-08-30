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

#include "TimingGraphCsrAdapterInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::detail {
namespace {

Transition convertTransition(ista::TransType transition)
{
  switch (transition) {
    case ista::TransType::kNone:
      return Transition::kNone;
    case ista::TransType::kRise:
      return Transition::kRise;
    case ista::TransType::kFall:
      return Transition::kFall;
  }
  malformed();
}

TimingSense convertSense(ista::TimingArcSense sense)
{
  switch (sense) {
    case ista::TimingArcSense::kNone:
      return TimingSense::kNoneUsesPositiveFallback;
    case ista::TimingArcSense::kPositive:
      return TimingSense::kPositiveUnate;
    case ista::TimingArcSense::kNegative:
      return TimingSense::kNegativeUnate;
    case ista::TimingArcSense::kNonUnate:
      return TimingSense::kNonUnate;
  }
  malformed();
}

TableVariable convertTableVariable(ista::TimingTableVariableType variable)
{
  switch (variable) {
    case ista::TimingTableVariableType::kNone:
      return TableVariable::kNone;
    case ista::TimingTableVariableType::kInputTransition:
      return TableVariable::kInputTransition;
    case ista::TimingTableVariableType::kOutputCapacitance:
      return TableVariable::kOutputCapacitance;
    case ista::TimingTableVariableType::kConstrainedTransition:
      return TableVariable::kConstrainedTransition;
  }
  malformed();
}

std::size_t transitionIndex(ista::TransType transition)
{
  switch (transition) {
    case ista::TransType::kRise:
      return 0;
    case ista::TransType::kFall:
      return 1;
    case ista::TransType::kNone:
      malformed();
  }
  malformed();
}

TimingTableSignature makeTableSignature(const ista::TimingTable& table)
{
  return {true, convertTableVariable(table.get_variable_type1()), convertTableVariable(table.get_variable_type2())};
}

std::array<TimingTableSignature, kTransitionCount> makeTableSignatures(const std::map<ista::TransType, ista::TimingTable>& tables)
{
  std::array<TimingTableSignature, kTransitionCount> result;
  for (const auto& [transition, table] : tables) {
    const std::size_t index = transitionIndex(transition);
    result[index] = makeTableSignature(table);
  }
  return result;
}

LibraryArcSignature makeLibrarySignature(const ista::TimingArc& arc)
{
  if (!arc.get_check_table_map().empty())
    malformed();
  LibraryArcSignature result;
  result.library_name = arc.get_library_name();
  result.arc_index = arc.get_arc_idx();
  result.sense = convertSense(arc.get_sense());
  result.trigger_transition = convertTransition(arc.get_trigger_trans_type());
  result.check_transition = convertTransition(arc.get_check_trans_type());
  result.sdf_condition = arc.get_sdf_cond();
  result.delay_tables = makeTableSignatures(arc.get_delay_table_map());
  result.slew_tables = makeTableSignatures(arc.get_slew_table_map());
  return result;
}

std::vector<LibraryArcSignature> makeLibrarySignatures(const ista::TimingCellArc& cell_arc)
{
  std::vector<LibraryArcSignature> result;
  result.reserve(cell_arc.get_timing_arc_list().size());
  for (const auto& timing_arc : cell_arc.get_timing_arc_list())
    result.push_back(makeLibrarySignature(timing_arc));
  return result;
}

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

const ista::Instance& requireArcInstance(const GraphBuildState& state, const ista::Arc& arc, const ista::Pin& source, const ista::Pin& sink)
{
  if (source.get_is_port() || sink.get_is_port() || source.get_instance_name().empty() || source.get_instance_name() != sink.get_instance_name()
      || arc.get_owner_name() != source.get_instance_name())
    malformed();
  const auto instance = state.database.get_instance_map().find(source.get_instance_name());
  if (instance == state.database.get_instance_map().end() || instance->first != instance->second.get_instance_name())
    malformed();
  const auto source_owner = state.ordered_pin_owners.find(source.get_full_name());
  const auto sink_owner = state.ordered_pin_owners.find(sink.get_full_name());
  if (source_owner == state.ordered_pin_owners.end() || sink_owner == state.ordered_pin_owners.end() || source_owner->second != instance->first
      || sink_owner->second != instance->first) {
    malformed();
  }
  return instance->second;
}

const ista::TimingCell* findTimingCell(const GraphBuildState& state, const ista::Instance& instance)
{
  const auto cell = state.database.get_timing_library().get_cell_map().find(instance.get_cell_name());
  return cell == state.database.get_timing_library().get_cell_map().end() ? nullptr : std::addressof(cell->second);
}

const ista::TimingCellArc& requireBoundCellArc(const ista::Arc& arc, const ista::TimingCell& cell)
{
  const ista::TimingCellArc* native_pointer = arc.get_timing_cell_arc();
  if (native_pointer == nullptr)
    malformed();
  const ista::TimingCellArc* matched = nullptr;
  for (const auto& candidate : cell.get_cell_arc_list()) {
    if (std::addressof(candidate) == native_pointer) {
      matched = std::addressof(candidate);
      break;
    }
  }
  if (matched == nullptr || !matched->get_is_timing_graph_arc())
    malformed();
  return *matched;
}

bool isInputLike(PinDirection direction)
{
  return direction == PinDirection::kInput || direction == PinDirection::kInout;
}

bool isOutputLike(PinDirection direction)
{
  return direction == PinDirection::kOutput || direction == PinDirection::kInout;
}

ArcKeyBase makeCellArcBase(const GraphBuildState& state, const ista::Arc& arc, const ista::Pin& source, const ista::Pin& sink)
{
  const auto& instance = requireArcInstance(state, arc, source, sink);
  const auto* cell = findTimingCell(state, instance);
  const bool fallback_cell = cell == nullptr || cell->get_cell_arc_list().empty();
  const bool raw_source_empty = arc.get_library_source_port().empty();
  const bool raw_sink_empty = arc.get_library_sink_port().empty();

  ArcKeyBase result;
  result.source = state.snapshot.nodes[requireNodeId(state, source.get_full_name()).value()].key;
  result.sink = state.snapshot.nodes[requireNodeId(state, sink.get_full_name()).value()].key;
  result.kind = ArcKind::kCell;
  result.owner_name = instance.get_instance_name();

  if (arc.get_timing_cell_arc() == nullptr) {
    if (!fallback_cell || !raw_source_empty || !raw_sink_empty || source.get_full_name() == sink.get_full_name()
        || !isInputLike(convertPinDirection(source.get_direction())) || !isOutputLike(convertPinDirection(sink.get_direction())))
      malformed();
    result.library_source_port = source.get_pin_name();
    result.library_sink_port = sink.get_pin_name();
    return result;
  }

  if (fallback_cell || raw_source_empty || raw_sink_empty)
    malformed();
  const auto& cell_arc = requireBoundCellArc(arc, *cell);
  if (arc.get_library_source_port() != source.get_pin_name() || arc.get_library_sink_port() != sink.get_pin_name()
      || arc.get_library_source_port() != cell_arc.get_source_port() || arc.get_library_sink_port() != cell_arc.get_sink_port())
    malformed();
  result.library_source_port = arc.get_library_source_port();
  result.library_sink_port = arc.get_library_sink_port();
  result.library_arcs = makeLibrarySignatures(cell_arc);
  return result;
}

ArcKeyBase makeNetArcBase(const GraphBuildState& state, const ista::Arc& arc, const ista::Pin& source, const ista::Pin& sink)
{
  if (arc.get_timing_cell_arc() != nullptr || !arc.get_library_source_port().empty() || !arc.get_library_sink_port().empty())
    malformed();
  if (source.get_net_name().empty() || source.get_net_name() != sink.get_net_name() || arc.get_owner_name() != source.get_net_name())
    malformed();
  const auto net = state.database.get_net_map().find(arc.get_owner_name());
  if (net == state.database.get_net_map().end() || net->first != net->second.get_net_name())
    malformed();
  return {state.snapshot.nodes[requireNodeId(state, source.get_full_name()).value()].key,
          state.snapshot.nodes[requireNodeId(state, sink.get_full_name()).value()].key,
          ArcKind::kNet,
          arc.get_owner_name(),
          {},
          {},
          {}};
}

ArcKeyBase makeArcBase(const GraphBuildState& state, const ista::Arc& arc)
{
  const auto& source = requirePin(state, arc.get_source_pin());
  const auto& sink = requirePin(state, arc.get_sink_pin());
  switch (arc.get_type()) {
    case ista::ArcType::kCell:
      return makeCellArcBase(state, arc, source, sink);
    case ista::ArcType::kNet:
      return makeNetArcBase(state, arc, source, sink);
    case ista::ArcType::kNone:
      malformed();
  }
  malformed();
}

using CellArcCounts = std::map<std::pair<std::string, std::string>, std::size_t>;
using CellArcsByOwner = std::unordered_map<std::string, CellArcCounts>;

CellArcsByOwner indexCellArcsByOwner(const ista::Database& database)
{
  CellArcsByOwner result;
  result.reserve(database.get_instance_map().size());
  for (const auto& arc : database.get_arc_list()) {
    if (arc.get_type() != ista::ArcType::kCell)
      continue;
    auto [owner, inserted] = result.try_emplace(arc.get_owner_name());
    static_cast<void>(inserted);
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
    bases.push_back(makeArcBase(state, arc));
  try {
    state.native_arc_keys = assignParallelOrdinals(bases);
  } catch (const std::invalid_argument&) {
    malformed();
  }
  const auto arcs_by_owner = indexCellArcsByOwner(state.database);
  for (const auto& [name, instance] : state.database.get_instance_map())
    validateFallbackCartesianProduct(state, name, instance, arcs_by_owner);
  assignCanonicalArcs(state);
}

}  // namespace ista::csr::detail
