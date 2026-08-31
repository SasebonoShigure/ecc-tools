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
#include "TimingEditCanonicalEncoding.hpp"

#include <algorithm>
#include <string_view>
#include <type_traits>

#include "CsrReferenceCanonicalEncoder.hpp"
#include "TimingEditByteOrder.hpp"

namespace ista::timing_edit {
namespace {

using csr::CanonicalEncoder;

template <typename Values, typename Encoder>
void appendSortedEncoded(CanonicalEncoder& encoder, const Values& values, Encoder encode)
{
  std::vector<std::vector<std::uint8_t>> encoded;
  encoded.reserve(values.size());
  for (const auto& value : values)
    encoded.push_back(encode(value));
  std::sort(encoded.begin(), encoded.end(), detail::CanonicalBytesLess{});
  encoder.appendU64(encoded.size());
  for (const auto& bytes : encoded)
    encoder.appendBytes(bytes);
}

void appendCreationToken(CanonicalEncoder& encoder, const CreationToken& value)
{
  encoder.appendU32(value.schema_version);
  encoder.appendString(value.case_id);
  encoder.appendString(value.profile_id);
  encoder.appendU64(value.seed);
  encoder.appendU64(value.batch_ordinal);
  encoder.appendU64(value.accepted_ordinal);
}

void appendNodeRef(CanonicalEncoder& encoder, const StableNodeRef& value)
{
  std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref, ExistingNodeRef>) {
          encoder.appendString("existing");
          encoder.appendBytes(csr::canonicalNodeKeyBytes(ref.baseline_key));
        } else {
          encoder.appendString("created");
          appendCreationToken(encoder, ref.creation_token);
          encoder.appendString(token(ref.role));
        }
      },
      value);
}

void appendNetRef(CanonicalEncoder& encoder, const StableNetRef& value)
{
  std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref, ExistingNetRef>) {
          encoder.appendString("existing");
          encoder.appendString(ref.baseline_name);
        } else {
          encoder.appendString("created");
          appendCreationToken(encoder, ref.creation_token);
          encoder.appendString(token(ref.role));
        }
      },
      value);
}

void appendLibrarySignatures(CanonicalEncoder& encoder, const std::vector<csr::LibraryArcSignature>& values)
{
  encoder.appendU64(values.size());
  for (const auto& value : values)
    encoder.appendBytes(csr::canonicalLibraryArcSignatureBytes(value));
}

void appendCellArcRef(CanonicalEncoder& encoder, const CreatedCellArcRef& ref)
{
  appendCreationToken(encoder, ref.creation_token);
  encoder.appendString(ref.library_source_port);
  encoder.appendString(ref.library_sink_port);
  appendLibrarySignatures(encoder, ref.ordered_library_arcs);
  encoder.appendU64(ref.stable_equivalence_ordinal);
}

void appendArcRef(CanonicalEncoder& encoder, const StableArcRef& value)
{
  std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref, ExistingArcRef>) {
          encoder.appendString("existing");
          encoder.appendBytes(csr::canonicalArcKeyBytes(ref.baseline_key));
        } else if constexpr (std::is_same_v<Ref, CreatedCellArcRef>) {
          encoder.appendString("created_cell");
          appendCellArcRef(encoder, ref);
        } else if constexpr (std::is_same_v<Ref, CreatedOldNetArcRef>) {
          encoder.appendString("created_old_net");
          appendNetRef(encoder, ref.net);
          appendNodeRef(encoder, ref.driver);
          appendNodeRef(encoder, ref.sink);
          encoder.appendU64(ref.stable_ordinal);
        } else {
          encoder.appendString("created_new_net");
          appendCreationToken(encoder, ref.creation_token);
          appendNodeRef(encoder, ref.driver);
          appendNodeRef(encoder, ref.sink);
          encoder.appendU64(ref.stable_ordinal);
        }
      },
      value);
}

void appendFingerprints(CanonicalEncoder& encoder, const ComponentFingerprints& value)
{
  encoder.appendString(value.schema_version);
  encoder.appendString(value.topology);
  encoder.appendString(value.clock_constraint);
  encoder.appendString(value.library);
  encoder.appendString(value.corner);
  encoder.appendString(value.parasitic);
  encoder.appendString(value.timing_input);
}

void appendPreState(CanonicalEncoder& encoder, const CanonicalPreStateEvidence& value)
{
  appendSortedEncoded(encoder, value.instances, [](const std::string& item) {
    CanonicalEncoder nested;
    nested.appendString(item);
    return nested.bytes();
  });
  appendSortedEncoded(encoder, value.nets, [](const std::string& item) {
    CanonicalEncoder nested;
    nested.appendString(item);
    return nested.bytes();
  });
  appendSortedEncoded(encoder, value.nodes, csr::canonicalNodeKeyBytes);
  appendSortedEncoded(encoder, value.arcs, csr::canonicalArcKeyBytes);
}

void appendBaseline(CanonicalEncoder& encoder, const BaselineSemanticIdentity& value)
{
  encoder.appendString(value.design_name);
  appendFingerprints(encoder, value.fingerprints);
  appendPreState(encoder, value.canonical_pre_state);
}

void appendCommitted(CanonicalEncoder& encoder, const CommittedEditIdentity& value)
{
  encoder.appendU64(value.generation.value());
  appendFingerprints(encoder, value.current_fingerprints);
  appendPreState(encoder, value.canonical_pre_state);
}

void appendLibraryCell(CanonicalEncoder& encoder, const LibraryCellRef& value)
{
  encoder.appendString(value.library_name);
  encoder.appendString(value.cell_name);
  encoder.appendString(value.input_port);
  encoder.appendString(value.output_port);
  appendLibrarySignatures(encoder, value.ordered_arc_signatures);
}

void appendPlacement(CanonicalEncoder& encoder, const PlacementInputs& value)
{
  std::visit(
      [&](const auto& placement) {
        using Placement = std::decay_t<decltype(placement)>;
        if constexpr (std::is_same_v<Placement, UnplacedInputs>) {
          encoder.appendString("unplaced");
        } else {
          encoder.appendString("placed");
          encoder.appendDouble(placement.x_microns);
          encoder.appendDouble(placement.y_microns);
        }
      },
      value);
}

void appendBufferInsertion(CanonicalEncoder& encoder, const BufferInsertionMutation& value)
{
  appendNetRef(encoder, value.original_net);
  appendSortedEncoded(encoder, value.moved_sinks, canonicalStableNodeRefBytes);
  appendLibraryCell(encoder, value.library_cell);
  appendPlacement(encoder, value.placement_inputs);
  std::visit(
      [&](const auto& rc_inputs) {
        using Rc = std::decay_t<decltype(rc_inputs)>;
        if constexpr (std::is_same_v<Rc, NoParasiticRcInputs>) {
          encoder.appendString("no_parasitic");
        } else {
          encoder.appendString("parasitic");
          encoder.appendString(token(rc_inputs.parasitic_policy));
        }
      },
      value.rc_inputs);
  appendCreationToken(encoder, value.creation_token);
}

std::string_view operationToken(const TimingGraphEdit& value)
{
  return std::visit(
      [](const auto& operation) -> std::string_view {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, AddInstance>)
          return "add_instance";
        if constexpr (std::is_same_v<Operation, RemoveInstance>)
          return "remove_instance";
        if constexpr (std::is_same_v<Operation, AddTimingPoint>)
          return "add_timing_point";
        if constexpr (std::is_same_v<Operation, RemoveTimingPoint>)
          return "remove_timing_point";
        if constexpr (std::is_same_v<Operation, AddCellArc>)
          return "add_cell_arc";
        if constexpr (std::is_same_v<Operation, RemoveCellArc>)
          return "remove_cell_arc";
        if constexpr (std::is_same_v<Operation, AddNetArc>)
          return "add_net_arc";
        if constexpr (std::is_same_v<Operation, RemoveNetArc>)
          return "remove_net_arc";
        if constexpr (std::is_same_v<Operation, ReconnectSink>)
          return "reconnect_sink";
        if constexpr (std::is_same_v<Operation, InvalidateLoad>)
          return "invalidate_load";
        if constexpr (std::is_same_v<Operation, InvalidateParasitic>)
          return "invalidate_parasitic";
        return "invalidate_arc_delay";
      },
      value);
}

void appendOperation(CanonicalEncoder& encoder, const TimingGraphEdit& value)
{
  encoder.appendString(operationToken(value));
  std::visit(
      [&](const auto& operation) {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, AddInstance>) {
          appendCreationToken(encoder, operation.creation_token);
          appendLibraryCell(encoder, operation.library_cell);
          appendPlacement(encoder, operation.placement_inputs);
        } else if constexpr (std::is_same_v<Operation, RemoveInstance>) {
          appendCreationToken(encoder, operation.creation_token);
        } else if constexpr (std::is_same_v<Operation, AddTimingPoint>) {
          appendCreationToken(encoder, operation.node.creation_token);
          encoder.appendString(token(operation.node.role));
          encoder.appendString(csr::token(operation.direction));
        } else if constexpr (std::is_same_v<Operation, RemoveTimingPoint>) {
          appendCreationToken(encoder, operation.node.creation_token);
          encoder.appendString(token(operation.node.role));
        } else if constexpr (std::is_same_v<Operation, AddCellArc> || std::is_same_v<Operation, RemoveCellArc>) {
          appendCellArcRef(encoder, operation.arc);
        } else if constexpr (std::is_same_v<Operation, AddNetArc> || std::is_same_v<Operation, RemoveNetArc>) {
          appendArcRef(encoder, operation.arc);
        } else if constexpr (std::is_same_v<Operation, ReconnectSink>) {
          appendNodeRef(encoder, operation.sink);
          appendNetRef(encoder, operation.old_net);
          appendNetRef(encoder, operation.new_net);
        } else if constexpr (std::is_same_v<Operation, InvalidateLoad> || std::is_same_v<Operation, InvalidateParasitic>) {
          appendNetRef(encoder, operation.net);
        } else {
          appendArcRef(encoder, operation.arc);
        }
      },
      value);
}

template <typename Ref>
void appendAssignments(CanonicalEncoder& encoder, const std::vector<SlotAssignment<Ref>>& values, auto encode_ref)
{
  std::vector<std::vector<std::uint8_t>> encoded;
  encoded.reserve(values.size());
  for (const auto& value : values) {
    CanonicalEncoder nested;
    nested.appendBytes(encode_ref(value.ref));
    nested.appendU64(value.slot.value());
    nested.appendBool(value.tombstone);
    encoded.push_back(nested.bytes());
  }
  std::sort(encoded.begin(), encoded.end(), detail::CanonicalBytesLess{});
  encoder.appendU64(encoded.size());
  for (const auto& value : encoded)
    encoder.appendBytes(value);
}

void appendSlotPlan(CanonicalEncoder& encoder, const AppendOnlySlotPlan& value)
{
  encoder.appendU64(value.before.node_slots);
  encoder.appendU64(value.before.net_slots);
  encoder.appendU64(value.before.arc_slots);
  encoder.appendU64(value.after.node_slots);
  encoder.appendU64(value.after.net_slots);
  encoder.appendU64(value.after.arc_slots);
  appendAssignments(encoder, value.nodes, canonicalStableNodeRefBytes);
  appendAssignments(encoder, value.nets, canonicalStableNetRefBytes);
  appendAssignments(encoder, value.arcs, canonicalStableArcRefBytes);
}

void appendMaterialization(CanonicalEncoder& encoder, const TokenMaterialization& value)
{
  appendCreationToken(encoder, value.creation_token);
  encoder.appendString(value.instance_name);
  encoder.appendString(value.output_net_name);
  encoder.appendString(value.input_pin_name);
  encoder.appendString(value.output_pin_name);
}

void appendPolicyReceipt(CanonicalEncoder& encoder, const PolicyReceipt& value)
{
  encoder.appendString(token(value.disposition));
  appendSortedEncoded(encoder, value.reasons, [](const std::string& reason) {
    CanonicalEncoder nested;
    nested.appendString(reason);
    return nested.bytes();
  });
}

template <typename Build>
std::vector<std::uint8_t> encode(std::string_view domain, Build build)
{
  CanonicalEncoder encoder;
  encoder.appendDomain(domain);
  build(encoder);
  return encoder.bytes();
}

}  // namespace

std::vector<std::uint8_t> canonicalCreationTokenBytes(const CreationToken& value)
{
  return encode("timing-edit-creation-token", [&](CanonicalEncoder& encoder) { appendCreationToken(encoder, value); });
}

std::vector<std::uint8_t> canonicalStableNodeRefBytes(const StableNodeRef& value)
{
  return encode("timing-edit-node-ref", [&](CanonicalEncoder& encoder) { appendNodeRef(encoder, value); });
}

std::vector<std::uint8_t> canonicalStableNetRefBytes(const StableNetRef& value)
{
  return encode("timing-edit-net-ref", [&](CanonicalEncoder& encoder) { appendNetRef(encoder, value); });
}

std::vector<std::uint8_t> canonicalStableArcRefBytes(const StableArcRef& value)
{
  return encode("timing-edit-arc-ref", [&](CanonicalEncoder& encoder) { appendArcRef(encoder, value); });
}

std::vector<std::uint8_t> canonicalComponentFingerprintsBytes(const ComponentFingerprints& value)
{
  return encode("timing-edit-component-fingerprints", [&](CanonicalEncoder& encoder) { appendFingerprints(encoder, value); });
}

std::vector<std::uint8_t> canonicalPreStateEvidenceBytes(const CanonicalPreStateEvidence& value)
{
  return encode("timing-edit-pre-state", [&](CanonicalEncoder& encoder) { appendPreState(encoder, value); });
}

std::vector<std::uint8_t> canonicalBaselineSemanticIdentityBytes(const BaselineSemanticIdentity& value)
{
  return encode("timing-edit-baseline-identity", [&](CanonicalEncoder& encoder) { appendBaseline(encoder, value); });
}

std::vector<std::uint8_t> canonicalCommittedEditIdentityBytes(const CommittedEditIdentity& value)
{
  return encode("timing-edit-committed-identity", [&](CanonicalEncoder& encoder) { appendCommitted(encoder, value); });
}

std::vector<std::uint8_t> canonicalLibraryCellRefBytes(const LibraryCellRef& value)
{
  return encode("timing-edit-library-cell-ref", [&](CanonicalEncoder& encoder) { appendLibraryCell(encoder, value); });
}

std::vector<std::uint8_t> canonicalBufferInsertionMutationBytes(const BufferInsertionMutation& value)
{
  return encode("timing-edit-buffer-insertion", [&](CanonicalEncoder& encoder) { appendBufferInsertion(encoder, value); });
}

std::vector<std::uint8_t> canonicalDesignEditPlanBytes(const DesignEditPlan& value)
{
  return encode("timing-edit-plan", [&](CanonicalEncoder& encoder) {
    encoder.appendString(value.schema_version);
    appendBaseline(encoder, value.baseline);
    encoder.appendU64(value.expected_generation.value());
    appendSortedEncoded(encoder, value.edits, canonicalBufferInsertionMutationBytes);
  });
}

std::vector<std::uint8_t> canonicalTimingGraphEditBytes(const TimingGraphEdit& value)
{
  return encode("timing-graph-edit", [&](CanonicalEncoder& encoder) { appendOperation(encoder, value); });
}

std::uint8_t timingGraphEditCanonicalRank(const TimingGraphEdit& value)
{
  return std::visit(
      [](const auto& operation) -> std::uint8_t {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, AddInstance>) return 0;
        else if constexpr (std::is_same_v<Operation, AddTimingPoint>) return 1;
        else if constexpr (std::is_same_v<Operation, AddCellArc>) return 2;
        else if constexpr (std::is_same_v<Operation, RemoveNetArc>) return 3;
        else if constexpr (std::is_same_v<Operation, AddNetArc>) return 4;
        else if constexpr (std::is_same_v<Operation, ReconnectSink>) return 5;
        else if constexpr (std::is_same_v<Operation, InvalidateLoad>) return 6;
        else if constexpr (std::is_same_v<Operation, InvalidateParasitic>) return 7;
        else if constexpr (std::is_same_v<Operation, InvalidateArcDelay>) return 8;
        else if constexpr (std::is_same_v<Operation, RemoveCellArc>) return 9;
        else if constexpr (std::is_same_v<Operation, RemoveTimingPoint>) return 10;
        else return 11;
      },
      value);
}

std::vector<std::uint8_t> canonicalTimingGraphDeltaBytes(const TimingGraphDelta& value)
{
  return encode("timing-graph-delta", [&](CanonicalEncoder& encoder) {
    encoder.appendString(value.schema_version);
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> operations;
    operations.reserve(value.operations.size());
    for (const auto& operation : value.operations)
      operations.emplace_back(timingGraphEditCanonicalRank(operation), canonicalTimingGraphEditBytes(operation));
    std::sort(operations.begin(), operations.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first)
        return lhs.first < rhs.first;
      return detail::canonicalBytesLess(lhs.second, rhs.second);
    });
    encoder.appendU64(operations.size());
    for (const auto& [rank, bytes] : operations) {
      encoder.appendU8(rank);
      encoder.appendBytes(bytes);
    }
  });
}

std::vector<std::uint8_t> canonicalPreparedBatchDataBytes(const PreparedBatchData& value)
{
  return encode("timing-edit-prepared-batch", [&](CanonicalEncoder& encoder) {
    encoder.appendU64(value.session_token.value());
    encoder.appendU64(value.expected_generation.value());
    encoder.appendU64(value.target_generation.value());
    appendBaseline(encoder, value.baseline);
    appendCommitted(encoder, value.expected_pre_committed_identity);
    appendSortedEncoded(encoder, value.normalized_edits, canonicalBufferInsertionMutationBytes);
    encoder.appendBytes(canonicalTimingGraphDeltaBytes(value.normalized_forward));
    encoder.appendBytes(canonicalTimingGraphDeltaBytes(value.normalized_reverse));
    appendSlotPlan(encoder, value.slot_plan);
    appendSortedEncoded(encoder, value.token_materializations, [](const TokenMaterialization& materialization) {
      CanonicalEncoder nested;
      appendMaterialization(nested, materialization);
      return nested.bytes();
    });
    appendSortedEncoded(encoder, value.invalidation_closure.load_nets, canonicalStableNetRefBytes);
    appendSortedEncoded(encoder, value.invalidation_closure.parasitic_nets, canonicalStableNetRefBytes);
    appendSortedEncoded(encoder, value.invalidation_closure.arc_delays, canonicalStableArcRefBytes);
    appendPolicyReceipt(encoder, value.policy_receipt);
  });
}

bool operator<(const CreatedCellArcRef& lhs, const CreatedCellArcRef& rhs)
{
  return detail::canonicalBytesLess(canonicalStableArcRefBytes(StableArcRef{lhs}), canonicalStableArcRefBytes(StableArcRef{rhs}));
}

bool operator<(const CreatedOldNetArcRef& lhs, const CreatedOldNetArcRef& rhs)
{
  return detail::canonicalBytesLess(canonicalStableArcRefBytes(StableArcRef{lhs}), canonicalStableArcRefBytes(StableArcRef{rhs}));
}

bool operator<(const CreatedNewNetArcRef& lhs, const CreatedNewNetArcRef& rhs)
{
  return detail::canonicalBytesLess(canonicalStableArcRefBytes(StableArcRef{lhs}), canonicalStableArcRefBytes(StableArcRef{rhs}));
}

bool operator<(const LibraryCellRef& lhs, const LibraryCellRef& rhs)
{
  return detail::canonicalBytesLess(canonicalLibraryCellRefBytes(lhs), canonicalLibraryCellRefBytes(rhs));
}

}  // namespace ista::timing_edit
