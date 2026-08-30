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
#include "TimingSemanticKey.hpp"

#include <algorithm>
#include <bit>
#include <compare>
#include <numeric>
#include <stdexcept>

namespace ista::csr {

std::string_view token(NodeKind value)
{
  switch (value) {
    case NodeKind::kPort:
      return "port";
    case NodeKind::kInstancePin:
      return "instance_pin";
  }
  throw std::invalid_argument("unknown NodeKind");
}

std::string_view token(ArcKind value)
{
  switch (value) {
    case ArcKind::kCell:
      return "cell";
    case ArcKind::kNet:
      return "net";
  }
  throw std::invalid_argument("unknown ArcKind");
}

std::string_view token(PinDirection value)
{
  switch (value) {
    case PinDirection::kNone:
      return "none";
    case PinDirection::kInput:
      return "input";
    case PinDirection::kOutput:
      return "output";
    case PinDirection::kInout:
      return "inout";
  }
  throw std::invalid_argument("unknown PinDirection");
}

std::string_view token(AnalysisMode value)
{
  switch (value) {
    case AnalysisMode::kMax:
      return "max";
    case AnalysisMode::kMin:
      return "min";
  }
  throw std::invalid_argument("unknown AnalysisMode");
}

std::string_view token(TimingSense value)
{
  switch (value) {
    case TimingSense::kNoneUsesPositiveFallback:
      return "none_positive_fallback";
    case TimingSense::kPositiveUnate:
      return "positive_unate";
    case TimingSense::kNegativeUnate:
      return "negative_unate";
    case TimingSense::kNonUnate:
      return "non_unate";
  }
  throw std::invalid_argument("unknown TimingSense");
}

std::string_view token(Transition value)
{
  switch (value) {
    case Transition::kRise:
      return "rise";
    case Transition::kFall:
      return "fall";
    case Transition::kNone:
      return "none";
  }
  throw std::invalid_argument("unknown Transition");
}

std::string_view token(PathSource value)
{
  switch (value) {
    case PathSource::kInput:
      return "input";
    case PathSource::kRegister:
      return "register";
  }
  throw std::invalid_argument("unknown PathSource");
}

std::string_view token(StartSeedAvailability value)
{
  switch (value) {
    case StartSeedAvailability::kMaterialized:
      return "materialized";
    case StartSeedAvailability::kUnsupportedSequential:
      return "unsupported_sequential";
  }
  throw std::invalid_argument("unknown StartSeedAvailability");
}

std::string_view token(TableVariable value)
{
  switch (value) {
    case TableVariable::kNone:
      return "none";
    case TableVariable::kInputTransition:
      return "input_transition";
    case TableVariable::kOutputCapacitance:
      return "output_capacitance";
    case TableVariable::kConstrainedTransition:
      return "constrained_transition";
  }
  throw std::invalid_argument("unknown TableVariable");
}

std::string_view token(TimingCheckKind value)
{
  switch (value) {
    case TimingCheckKind::kNone:
      return "none";
    case TimingCheckKind::kSetup:
      return "setup";
    case TimingCheckKind::kHold:
      return "hold";
    case TimingCheckKind::kRecovery:
      return "recovery";
    case TimingCheckKind::kRemoval:
      return "removal";
    case TimingCheckKind::kWidth:
      return "width";
    case TimingCheckKind::kPeriod:
      return "period";
  }
  throw std::invalid_argument("unknown TimingCheckKind");
}

std::string_view token(CsrCaptureCode value)
{
  switch (value) {
    case CsrCaptureCode::kMalformedTopology:
      return "malformed_topology";
    case CsrCaptureCode::kSemanticKeyCollision:
      return "semantic_key_collision";
    case CsrCaptureCode::kIllegalNumeric:
      return "illegal_numeric";
    case CsrCaptureCode::kInvalidTableShape:
      return "invalid_table_shape";
    case CsrCaptureCode::kInvalidAnalysisContext:
      return "invalid_analysis_context";
    case CsrCaptureCode::kActiveCycle:
      return "active_cycle";
  }
  throw std::invalid_argument("unknown CsrCaptureCode");
}

std::string_view token(CsrReferenceUnsupportedReason value)
{
  switch (value) {
    case CsrReferenceUnsupportedReason::kParasiticNet:
      return "parasitic_net";
    case CsrReferenceUnsupportedReason::kSequentialInstance:
      return "sequential_instance";
    case CsrReferenceUnsupportedReason::kSequentialLaunch:
      return "sequential_launch";
    case CsrReferenceUnsupportedReason::kTimingCheck:
      return "timing_check";
    case CsrReferenceUnsupportedReason::kCrpr:
      return "crpr";
    case CsrReferenceUnsupportedReason::kPropagatedClock:
      return "propagated_clock";
    case CsrReferenceUnsupportedReason::kClockTreeFanout:
      return "clock_tree_fanout";
    case CsrReferenceUnsupportedReason::kClockGatingOrDivider:
      return "clock_gating_or_divider";
    case CsrReferenceUnsupportedReason::kConditionalTimingArc:
      return "conditional_timing_arc";
    case CsrReferenceUnsupportedReason::kCaseAnalysis:
      return "case_analysis";
  }
  throw std::invalid_argument("unknown CsrReferenceUnsupportedReason");
}

std::string_view token(CsrComputationCode value)
{
  switch (value) {
    case CsrComputationCode::kCycle:
      return "cycle";
    case CsrComputationCode::kInvariantViolation:
      return "invariant_violation";
    case CsrComputationCode::kNumericFailure:
      return "numeric_failure";
  }
  throw std::invalid_argument("unknown CsrComputationCode");
}

std::string_view token(CsrScatterCode value)
{
  switch (value) {
    case CsrScatterCode::kSuccess:
      return "success";
    case CsrScatterCode::kInvalidState:
      return "invalid_state";
    case CsrScatterCode::kSnapshotInvalidated:
      return "snapshot_invalidated";
    case CsrScatterCode::kSchemaMismatch:
      return "schema_mismatch";
    case CsrScatterCode::kSessionMismatch:
      return "session_mismatch";
    case CsrScatterCode::kGenerationMismatch:
      return "generation_mismatch";
    case CsrScatterCode::kSourceIdentityMismatch:
      return "source_identity_mismatch";
    case CsrScatterCode::kShapeMismatch:
      return "shape_mismatch";
    case CsrScatterCode::kResultNumericInvalid:
      return "result_numeric_invalid";
    case CsrScatterCode::kIdMappingMismatch:
      return "id_mapping_mismatch";
    case CsrScatterCode::kPredecessorMismatch:
      return "predecessor_mismatch";
  }
  throw std::invalid_argument("unknown CsrScatterCode");
}

std::string_view token(CsrInvalidationReason value)
{
  switch (value) {
    case CsrInvalidationReason::kTopologyChanged:
      return "topology_changed";
    case CsrInvalidationReason::kClockConstraintChanged:
      return "clock_constraint_changed";
    case CsrInvalidationReason::kLibraryChanged:
      return "library_changed";
    case CsrInvalidationReason::kCornerChanged:
      return "corner_changed";
    case CsrInvalidationReason::kParasiticChanged:
      return "parasitic_changed";
    case CsrInvalidationReason::kTimingInputChanged:
      return "timing_input_changed";
  }
  throw std::invalid_argument("unknown CsrInvalidationReason");
}

std::string_view token(CsrSnapshotState value)
{
  switch (value) {
    case CsrSnapshotState::kReady:
      return "ready";
    case CsrSnapshotState::kInvalidated:
      return "invalidated";
    case CsrSnapshotState::kReleased:
      return "released";
  }
  throw std::invalid_argument("unknown CsrSnapshotState");
}

std::string_view token(CapacityAction value)
{
  switch (value) {
    case CapacityAction::kInitialAllocate:
      return "initial_allocate";
  }
  throw std::invalid_argument("unknown CapacityAction");
}

namespace {

template <typename Enum>
bool hasKnownToken(Enum value)
{
  try {
    static_cast<void>(token(value));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

std::optional<std::string> validateTableSignature(const TimingTableSignature& table, std::string_view family, std::size_t library_arc_index,
                                                  std::size_t table_index)
{
  const std::string location = "library arc[" + std::to_string(library_arc_index) + "] " + std::string(family) + " table[" + std::to_string(table_index) + "]";
  if (!hasKnownToken(table.variable1))
    return location + " has unknown variable1";
  if (!hasKnownToken(table.variable2))
    return location + " has unknown variable2";
  if (!table.present && (table.variable1 != TableVariable::kNone || table.variable2 != TableVariable::kNone))
    return location + " is absent but retains table variables";
  if (table.present && table.variable1 == TableVariable::kNone && table.variable2 != TableVariable::kNone)
    return location + " has variable2 without variable1";
  return std::nullopt;
}

}  // namespace

std::optional<std::string> validateNodeKey(const NodeKey& key)
{
  if (!hasKnownToken(key.kind))
    return "node kind is unknown";
  if (key.full_name.empty())
    return "node full_name is empty";
  if (key.kind == NodeKind::kPort && (!key.instance_name.empty() || !key.pin_name.empty()))
    return "port key must not contain instance or pin names";
  if (key.kind == NodeKind::kInstancePin && (key.instance_name.empty() || key.pin_name.empty()))
    return "instance-pin key requires instance and pin names";
  if (key.kind == NodeKind::kInstancePin && key.full_name != key.instance_name + ":" + key.pin_name)
    return "instance-pin full_name must equal instance_name + ':' + pin_name";
  return std::nullopt;
}

std::optional<std::string> validateArcKeyBase(const ArcKeyBase& key)
{
  if (auto error = validateNodeKey(key.source))
    return "source " + *error;
  if (auto error = validateNodeKey(key.sink))
    return "sink " + *error;
  if (!hasKnownToken(key.kind))
    return "arc kind is unknown";
  if (key.owner_name.empty())
    return "arc owner_name is empty";
  if (key.kind == ArcKind::kNet && (!key.library_source_port.empty() || !key.library_sink_port.empty() || !key.library_arcs.empty()))
    return "net arc must not contain library identity";
  if (key.kind == ArcKind::kCell && (key.library_source_port.empty() || key.library_sink_port.empty()))
    return "cell arc requires library ports";
  for (std::size_t index = 0; index < key.library_arcs.size(); ++index) {
    const auto& signature = key.library_arcs[index];
    if (!hasKnownToken(signature.sense))
      return "library arc[" + std::to_string(index) + "] has unknown timing sense";
    if (!hasKnownToken(signature.trigger_transition))
      return "library arc[" + std::to_string(index) + "] has unknown trigger transition";
    if (!hasKnownToken(signature.check_transition))
      return "library arc[" + std::to_string(index) + "] has unknown check transition";
    for (std::size_t table_index = 0; table_index < kTransitionCount; ++table_index) {
      if (auto error = validateTableSignature(signature.delay_tables[table_index], "delay", index, table_index))
        return error;
      if (auto error = validateTableSignature(signature.slew_tables[table_index], "slew", index, table_index))
        return error;
    }
  }
  return std::nullopt;
}

namespace {

std::strong_ordering compareU32(std::uint32_t lhs, std::uint32_t rhs)
{
  if (lhs < rhs)
    return std::strong_ordering::less;
  if (lhs > rhs)
    return std::strong_ordering::greater;
  return std::strong_ordering::equal;
}

std::strong_ordering compareU64(std::uint64_t lhs, std::uint64_t rhs)
{
  if (lhs < rhs)
    return std::strong_ordering::less;
  if (lhs > rhs)
    return std::strong_ordering::greater;
  return std::strong_ordering::equal;
}

std::strong_ordering compareEncodedString(std::string_view lhs, std::string_view rhs)
{
  // CanonicalEncoder emits an eight-byte big-endian length before the bytes.
  // For a fixed-width unsigned integer, byte order and numeric order agree.
  if (auto order = compareU64(static_cast<std::uint64_t>(lhs.size()), static_cast<std::uint64_t>(rhs.size())); order != 0)
    return order;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const auto lhs_byte = static_cast<unsigned char>(lhs[index]);
    const auto rhs_byte = static_cast<unsigned char>(rhs[index]);
    if (lhs_byte < rhs_byte)
      return std::strong_ordering::less;
    if (lhs_byte > rhs_byte)
      return std::strong_ordering::greater;
  }
  return std::strong_ordering::equal;
}

std::strong_ordering compareNodeKey(const NodeKey& lhs, const NodeKey& rhs)
{
  if (auto order = compareEncodedString(token(lhs.kind), token(rhs.kind)); order != 0)
    return order;
  if (auto order = compareEncodedString(lhs.full_name, rhs.full_name); order != 0)
    return order;
  if (auto order = compareEncodedString(lhs.instance_name, rhs.instance_name); order != 0)
    return order;
  return compareEncodedString(lhs.pin_name, rhs.pin_name);
}

std::strong_ordering compareTableSignature(const TimingTableSignature& lhs, const TimingTableSignature& rhs)
{
  if (lhs.present != rhs.present)
    return lhs.present ? std::strong_ordering::greater : std::strong_ordering::less;
  if (auto order = compareEncodedString(token(lhs.variable1), token(rhs.variable1)); order != 0)
    return order;
  return compareEncodedString(token(lhs.variable2), token(rhs.variable2));
}

std::strong_ordering compareLibraryArc(const LibraryArcSignature& lhs, const LibraryArcSignature& rhs)
{
  if (auto order = compareEncodedString(lhs.library_name, rhs.library_name); order != 0)
    return order;
  if (auto order = compareU32(std::bit_cast<std::uint32_t>(lhs.arc_index), std::bit_cast<std::uint32_t>(rhs.arc_index)); order != 0)
    return order;
  if (auto order = compareEncodedString(token(lhs.sense), token(rhs.sense)); order != 0)
    return order;
  if (auto order = compareEncodedString(token(lhs.trigger_transition), token(rhs.trigger_transition)); order != 0)
    return order;
  if (auto order = compareEncodedString(token(lhs.check_transition), token(rhs.check_transition)); order != 0)
    return order;
  if (auto order = compareEncodedString(lhs.sdf_condition, rhs.sdf_condition); order != 0)
    return order;
  for (std::size_t index = 0; index < kTransitionCount; ++index) {
    if (auto order = compareTableSignature(lhs.delay_tables[index], rhs.delay_tables[index]); order != 0)
      return order;
  }
  for (std::size_t index = 0; index < kTransitionCount; ++index) {
    if (auto order = compareTableSignature(lhs.slew_tables[index], rhs.slew_tables[index]); order != 0)
      return order;
  }
  return std::strong_ordering::equal;
}

std::strong_ordering compareArcBase(const ArcKeyBase& lhs, const ArcKeyBase& rhs)
{
  if (auto order = compareNodeKey(lhs.source, rhs.source); order != 0)
    return order;
  if (auto order = compareNodeKey(lhs.sink, rhs.sink); order != 0)
    return order;
  if (auto order = compareEncodedString(token(lhs.kind), token(rhs.kind)); order != 0)
    return order;
  if (auto order = compareEncodedString(lhs.owner_name, rhs.owner_name); order != 0)
    return order;
  if (auto order = compareEncodedString(lhs.library_source_port, rhs.library_source_port); order != 0)
    return order;
  if (auto order = compareEncodedString(lhs.library_sink_port, rhs.library_sink_port); order != 0)
    return order;
  if (auto order = compareU64(static_cast<std::uint64_t>(lhs.library_arcs.size()), static_cast<std::uint64_t>(rhs.library_arcs.size())); order != 0)
    return order;
  for (std::size_t index = 0; index < lhs.library_arcs.size(); ++index) {
    if (auto order = compareLibraryArc(lhs.library_arcs[index], rhs.library_arcs[index]); order != 0)
      return order;
  }
  return std::strong_ordering::equal;
}

}  // namespace

bool nodeKeyLess(const NodeKey& lhs, const NodeKey& rhs)
{
  return compareNodeKey(lhs, rhs) < 0;
}

bool arcKeyLess(const ArcKey& lhs, const ArcKey& rhs)
{
  if (auto order = compareArcBase(lhs.base, rhs.base); order != 0)
    return order < 0;
  return compareU64(lhs.parallel_ordinal, rhs.parallel_ordinal) < 0;
}

std::vector<NodeKey> sortedUniqueNodeKeys(std::vector<NodeKey> keys)
{
  for (std::size_t index = 0; index < keys.size(); ++index) {
    if (auto error = validateNodeKey(keys[index]))
      throw std::invalid_argument("invalid NodeKey at input index " + std::to_string(index) + ": " + *error);
  }
  std::ranges::sort(keys, nodeKeyLess);
  if (std::ranges::adjacent_find(keys, [](const NodeKey& lhs, const NodeKey& rhs) { return compareNodeKey(lhs, rhs) == 0; }) != keys.end())
    throw std::invalid_argument("semantic NodeKey collision");
  return keys;
}

std::vector<ArcKey> assignParallelOrdinals(const std::vector<ArcKeyBase>& arcs_in_native_order)
{
  std::vector<std::size_t> canonical_order(arcs_in_native_order.size());
  std::iota(canonical_order.begin(), canonical_order.end(), 0);

  std::vector<ArcKey> result;
  result.reserve(arcs_in_native_order.size());
  for (std::size_t index = 0; index < arcs_in_native_order.size(); ++index) {
    if (auto error = validateArcKeyBase(arcs_in_native_order[index]))
      throw std::invalid_argument("invalid ArcKeyBase at native index " + std::to_string(index) + ": " + *error);
    result.push_back(ArcKey{arcs_in_native_order[index], 0});
  }

  std::ranges::sort(canonical_order, [&](std::size_t lhs, std::size_t rhs) {
    if (auto order = compareArcBase(arcs_in_native_order[lhs], arcs_in_native_order[rhs]); order != 0)
      return order < 0;
    return lhs < rhs;
  });

  std::uint64_t ordinal = 0;
  for (std::size_t position = 0; position < canonical_order.size(); ++position) {
    if (position == 0 || compareArcBase(arcs_in_native_order[canonical_order[position - 1]], arcs_in_native_order[canonical_order[position]]) != 0) {
      ordinal = 0;
    } else {
      ++ordinal;
    }
    result[canonical_order[position]].parallel_ordinal = ordinal;
  }
  return result;
}

}  // namespace ista::csr
