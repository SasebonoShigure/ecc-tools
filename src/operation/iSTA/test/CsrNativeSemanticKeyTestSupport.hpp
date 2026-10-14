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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Arc.hpp"
#include "CsrReferencePrimitives.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "Database.hpp"
#include "Pin.hpp"
#include "TimingArc.hpp"
#include "TimingCellArc.hpp"
#include "TimingTable.hpp"

namespace ista::csr::test {

[[nodiscard]] inline NodeKey nativeSemanticNodeKey(const Pin& pin)
{
  if (pin.get_is_port()) {
    return NodeKey{NodeKind::kPort, pin.get_full_name(), {}, {}};
  }
  return NodeKey{NodeKind::kInstancePin, pin.get_full_name(), pin.get_instance_name(), pin.get_pin_name()};
}

[[nodiscard]] inline NodeKey nativeSemanticNodeKey(const Database& database, const std::string& full_name)
{
  return nativeSemanticNodeKey(database.get_pin_map().at(full_name));
}

namespace native_semantic_key_detail {

[[nodiscard]] inline ArcKind convertArcKind(ista::ArcType type)
{
  switch (type) {
    case ista::ArcType::kCell:
      return ArcKind::kCell;
    case ista::ArcType::kNet:
      return ArcKind::kNet;
    case ista::ArcType::kNone:
      break;
  }
  throw std::runtime_error("direct projector: unknown arc type");
}

[[nodiscard]] inline TimingSense convertSense(ista::TimingArcSense sense)
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
  throw std::runtime_error("direct projector: unknown timing sense");
}

[[nodiscard]] inline Transition convertTransition(ista::TransType transition)
{
  switch (transition) {
    case ista::TransType::kRise:
      return Transition::kRise;
    case ista::TransType::kFall:
      return Transition::kFall;
    case ista::TransType::kNone:
      return Transition::kNone;
  }
  throw std::runtime_error("direct projector: unknown transition");
}

[[nodiscard]] inline TableVariable convertVariable(ista::TimingTableVariableType variable)
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
  throw std::runtime_error("direct projector: unknown table variable");
}

[[nodiscard]] inline TimingTableSignature makeTableSignature(const std::map<ista::TransType, TimingTable>& tables, ista::TransType transition)
{
  const auto found = tables.find(transition);
  if (found == tables.end()) {
    return {};
  }
  return TimingTableSignature{true, convertVariable(found->second.get_variable_type1()), convertVariable(found->second.get_variable_type2())};
}

[[nodiscard]] inline LibraryArcSignature makeLibrarySignature(const TimingArc& timing_arc)
{
  LibraryArcSignature signature;
  signature.library_name = timing_arc.get_library_name();
  signature.arc_index = timing_arc.get_arc_idx();
  signature.sense = convertSense(timing_arc.get_sense());
  signature.trigger_transition = convertTransition(timing_arc.get_trigger_trans_type());
  signature.check_transition = convertTransition(timing_arc.get_check_trans_type());
  signature.sdf_condition = timing_arc.get_sdf_cond();
  signature.delay_tables = {makeTableSignature(timing_arc.get_delay_table_map(), ista::TransType::kRise),
                            makeTableSignature(timing_arc.get_delay_table_map(), ista::TransType::kFall)};
  signature.slew_tables = {makeTableSignature(timing_arc.get_slew_table_map(), ista::TransType::kRise),
                           makeTableSignature(timing_arc.get_slew_table_map(), ista::TransType::kFall)};
  return signature;
}

[[nodiscard]] inline ArcKeyBase makeArcBase(const Database& database, const Arc& arc)
{
  ArcKeyBase base;
  base.source = nativeSemanticNodeKey(database, arc.get_source_pin());
  base.sink = nativeSemanticNodeKey(database, arc.get_sink_pin());
  base.kind = convertArcKind(arc.get_type());
  base.owner_name = arc.get_owner_name();
  if (base.kind == ArcKind::kCell) {
    const TimingCellArc* timing_cell_arc = arc.get_timing_cell_arc();
    if (timing_cell_arc == nullptr) {
      base.library_source_port = database.get_pin_map().at(arc.get_source_pin()).get_pin_name();
      base.library_sink_port = database.get_pin_map().at(arc.get_sink_pin()).get_pin_name();
    } else {
      base.library_source_port = arc.get_library_source_port();
      base.library_sink_port = arc.get_library_sink_port();
      for (const TimingArc& child : timing_cell_arc->get_timing_arc_list()) {
        base.library_arcs.push_back(makeLibrarySignature(child));
      }
    }
  }
  return base;
}

}  // namespace native_semantic_key_detail

[[nodiscard]] inline std::vector<ArcKey> nativeSemanticArcKeys(const Database& database)
{
  const auto bytes_less = [](const auto& lhs, const auto& rhs) {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
  };
  std::map<std::vector<std::uint8_t>, std::uint64_t, decltype(bytes_less)> next_ordinal;
  std::vector<ArcKey> keys;
  keys.reserve(database.get_arc_list().size());
  for (const Arc& arc : database.get_arc_list()) {
    ArcKey key{native_semantic_key_detail::makeArcBase(database, arc), 0};
    // Count complete identities in native order without rescanning prior arcs.
    auto& ordinal = next_ordinal[canonicalArcKeyBytes(key)];
    key.parallel_ordinal = ordinal++;
    keys.push_back(std::move(key));
  }
  return keys;
}

}  // namespace ista::csr::test
