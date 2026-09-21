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
#include "TimingEditValidation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

#include "CsrFingerprint.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "TimingEditByteOrder.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditValidationInternal.hpp"

namespace ista::timing_edit {
namespace {

using Violations = TimingEditContractViolations;
using Bytes = detail::CanonicalBytes;
using BytesSet = std::set<Bytes, detail::CanonicalBytesLess>;

void reject(Violations& errors, TimingEditContractCode code, std::string field, std::string message)
{
  errors.push_back({code, std::move(field), std::move(message)});
}

void append(Violations& target, Violations source)
{
  target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

bool validText(std::string_view value)
{
  return !value.empty() && value.find('\0') == std::string_view::npos;
}

template <typename Value, typename Encode>
bool sortedUniqueBy(const std::vector<Value>& values, Encode encode)
{
  std::optional<Bytes> previous;
  for (const auto& value : values) {
    Bytes current = encode(value);
    if (previous && !detail::canonicalBytesLess(*previous, current))
      return false;
    previous = std::move(current);
  }
  return true;
}

template <typename Value, typename Encode>
void canonicalSort(std::vector<Value>& values, Encode encode)
{
  std::sort(values.begin(), values.end(),
            [&](const Value& lhs, const Value& rhs) { return detail::canonicalBytesLess(encode(lhs), encode(rhs)); });
}

void validateFingerprints(const ComponentFingerprints& value, std::string_view field, Violations& errors)
{
  const std::string prefix(field);
  if (value.schema_version != csr::kCsrSchemaVersion)
    reject(errors, TimingEditContractCode::kSchemaMismatch, prefix + ".schema_version", "expected CSR fingerprint schema 1.0.0");
  const std::pair<std::string_view, const std::string*> fields[] = {{"topology", &value.topology},
                                                                   {"clock_constraint", &value.clock_constraint},
                                                                   {"library", &value.library},
                                                                   {"corner", &value.corner},
                                                                   {"parasitic", &value.parasitic},
                                                                   {"timing_input", &value.timing_input}};
  for (const auto& [name, fingerprint] : fields)
    if (!csr::isLowercaseSha256(*fingerprint))
      reject(errors, TimingEditContractCode::kInvalidValue, prefix + "." + std::string(name), "expected lowercase SHA-256 integrity value");
}

void validatePreState(const CanonicalPreStateEvidence& value, std::string_view field, Violations& errors)
{
  const std::string prefix(field);
  auto validate_names = [&](const std::vector<std::string>& names, std::string_view suffix) {
    for (std::size_t index = 0; index < names.size(); ++index)
      if (!validText(names[index]))
        reject(errors, TimingEditContractCode::kInvalidValue, prefix + "." + std::string(suffix) + "[" + std::to_string(index) + "]",
               "semantic name must be nonempty and contain no NUL");
    if (!std::is_sorted(names.begin(), names.end()) || std::adjacent_find(names.begin(), names.end()) != names.end())
      reject(errors, TimingEditContractCode::kNotCanonical, prefix + "." + std::string(suffix), "names must be sorted and duplicate-free");
  };
  validate_names(value.instances, "instances");
  validate_names(value.nets, "nets");
  for (std::size_t index = 0; index < value.nodes.size(); ++index)
    if (const auto error = csr::validateNodeKey(value.nodes[index]))
      reject(errors, TimingEditContractCode::kInvalidSemanticRef, prefix + ".nodes[" + std::to_string(index) + "]", *error);
  if (!sortedUniqueBy(value.nodes, csr::canonicalNodeKeyBytes))
    reject(errors, TimingEditContractCode::kNotCanonical, prefix + ".nodes", "node keys must be in canonical byte order and duplicate-free");
  for (std::size_t index = 0; index < value.arcs.size(); ++index)
    if (const auto error = csr::validateArcKeyBase(value.arcs[index].base))
      reject(errors, TimingEditContractCode::kInvalidSemanticRef, prefix + ".arcs[" + std::to_string(index) + "]", *error);
  if (!sortedUniqueBy(value.arcs, csr::canonicalArcKeyBytes))
    reject(errors, TimingEditContractCode::kNotCanonical, prefix + ".arcs", "arc keys must be in canonical byte order and duplicate-free");
}

void validateLibrarySignatures(const std::vector<csr::LibraryArcSignature>& values, std::string_view field, Violations& errors)
{
  if (values.empty()) {
    reject(errors, TimingEditContractCode::kInvalidValue, std::string(field), "at least one ordered Liberty arc signature is required");
    return;
  }
  try {
    if (!sortedUniqueBy(values, csr::canonicalLibraryArcSignatureBytes))
      reject(errors, TimingEditContractCode::kNotCanonical, std::string(field), "signatures must be in canonical byte order and duplicate-free");
  } catch (const std::invalid_argument& error) {
    reject(errors, TimingEditContractCode::kInvalidValue, std::string(field), error.what());
  }
}

void validateLibraryCell(const LibraryCellRef& value, std::string_view field, Violations& errors)
{
  const std::string prefix(field);
  const std::pair<std::string_view, const std::string*> strings[] = {{"library_name", &value.library_name},
                                                                     {"cell_name", &value.cell_name},
                                                                     {"input_port", &value.input_port},
                                                                     {"output_port", &value.output_port}};
  for (const auto& [name, text] : strings)
    if (!validText(*text))
      reject(errors, TimingEditContractCode::kInvalidValue, prefix + "." + std::string(name), "must be nonempty and contain no NUL");
  if (value.input_port == value.output_port)
    reject(errors, TimingEditContractCode::kConflict, prefix + ".output_port", "input and output ports must differ");
  validateLibrarySignatures(value.ordered_arc_signatures, prefix + ".ordered_arc_signatures", errors);
}

void validatePlacement(const PlacementInputs& value, std::string_view field, Violations& errors)
{
  if (const auto* placed = std::get_if<PlacedInputs>(&value))
    if (!std::isfinite(placed->x_microns) || !std::isfinite(placed->y_microns))
      reject(errors, TimingEditContractCode::kInvalidValue, std::string(field), "placed coordinates must be finite");
}

void validateRc(const RcInputs& value, std::string_view field, Violations& errors)
{
  if (const auto* parasitic = std::get_if<ParasiticRcInputs>(&value)) {
    try {
      static_cast<void>(token(parasitic->parasitic_policy));
    } catch (const std::invalid_argument& error) {
      reject(errors, TimingEditContractCode::kInvalidValue, std::string(field) + ".parasitic_policy", error.what());
    }
    if (parasitic->parasitic_policy == ParasiticEditPolicy::kError)
      reject(errors, TimingEditContractCode::kUnsupported, std::string(field), "parasitic mutation is rejected by policy");
  }
}

void validateBuffer(const BufferInsertionMutation& value, std::string_view field, Violations& errors)
{
  const std::string prefix(field);
  append(errors, validateCreationToken(value.creation_token, prefix + ".creation_token"));
  append(errors, validateStableNetRef(value.original_net, prefix + ".original_net"));
  if (value.moved_sinks.empty())
    reject(errors, TimingEditContractCode::kInvalidValue, prefix + ".moved_sinks", "at least one moved sink is required");
  BytesSet sinks;
  for (std::size_t index = 0; index < value.moved_sinks.size(); ++index) {
    append(errors, validateStableNodeRef(value.moved_sinks[index], prefix + ".moved_sinks[" + std::to_string(index) + "]"));
    if (!sinks.insert(canonicalStableNodeRefBytes(value.moved_sinks[index])).second)
      reject(errors, TimingEditContractCode::kDuplicate, prefix + ".moved_sinks[" + std::to_string(index) + "]", "duplicate sink");
    if (const auto* created = std::get_if<CreatedNodeRef>(&value.moved_sinks[index]);
        created && created->creation_token.batch_ordinal >= value.creation_token.batch_ordinal)
      reject(errors, TimingEditContractCode::kConflict, prefix + ".moved_sinks[" + std::to_string(index) + "]", "forward or same-batch created sink reference");
  }
  if (const auto* created = std::get_if<CreatedNetRef>(&value.original_net);
      created && created->creation_token.batch_ordinal >= value.creation_token.batch_ordinal)
    reject(errors, TimingEditContractCode::kConflict, prefix + ".original_net", "created original net must come from an earlier batch");
  validateLibraryCell(value.library_cell, prefix + ".library_cell", errors);
  validatePlacement(value.placement_inputs, prefix + ".placement_inputs", errors);
  validateRc(value.rc_inputs, prefix + ".rc_inputs", errors);
}

template <typename Ref>
void validateAssignmentList(const std::vector<SlotAssignment<Ref>>& values, std::uint64_t before, std::uint64_t after, std::string_view field,
                            auto encode, auto validate, Violations& errors)
{
  BytesSet refs;
  std::set<std::uint64_t> slots;
  for (std::size_t index = 0; index < values.size(); ++index) {
    const std::string item = std::string(field) + "[" + std::to_string(index) + "]";
    append(errors, validate(values[index].ref, item + ".ref"));
    if (!refs.insert(encode(values[index].ref)).second)
      reject(errors, TimingEditContractCode::kDuplicate, item + ".ref", "duplicate stable reference assignment");
    if (!slots.insert(values[index].slot.value()).second)
      reject(errors, TimingEditContractCode::kConflict, item + ".slot", "slot is assigned more than once");
    if (values[index].slot.value() >= after)
      reject(errors, TimingEditContractCode::kCapacityOverflow, item + ".slot", "slot is outside after high-water range");
    if (values[index].tombstone && values[index].slot.value() >= before)
      reject(errors, TimingEditContractCode::kConflict, item, "tombstone must reference a pre-existing slot");
    if (!values[index].tombstone && values[index].slot.value() < before)
      reject(errors, TimingEditContractCode::kConflict, item, "new assignment must append at or above prior high-water");
  }
}

Bytes operationSubject(const TimingGraphEdit& operation)
{
  return std::visit(
      [](const auto& value) -> Bytes {
        using Operation = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Operation, AddInstance> || std::is_same_v<Operation, RemoveInstance>) {
          return canonicalCreationTokenBytes(value.creation_token);
        } else if constexpr (std::is_same_v<Operation, AddTimingPoint> || std::is_same_v<Operation, RemoveTimingPoint>) {
          return canonicalStableNodeRefBytes(StableNodeRef{value.node});
        } else if constexpr (std::is_same_v<Operation, AddCellArc> || std::is_same_v<Operation, RemoveCellArc>) {
          return canonicalStableArcRefBytes(StableArcRef{value.arc});
        } else if constexpr (std::is_same_v<Operation, AddNetArc> || std::is_same_v<Operation, RemoveNetArc>) {
          return canonicalStableArcRefBytes(value.arc);
        } else if constexpr (std::is_same_v<Operation, ReconnectSink>) {
          return canonicalStableNodeRefBytes(value.sink);
        } else if constexpr (std::is_same_v<Operation, InvalidateLoad> || std::is_same_v<Operation, InvalidateParasitic>) {
          return canonicalStableNetRefBytes(value.net);
        } else {
          return canonicalStableArcRefBytes(value.arc);
        }
      },
      operation);
}

void validateOperation(const TimingGraphEdit& operation, std::string_view field, Violations& errors)
{
  std::visit(
      [&](const auto& value) {
        using Operation = std::decay_t<decltype(value)>;
        const std::string prefix(field);
        if constexpr (std::is_same_v<Operation, AddInstance>) {
          append(errors, validateCreationToken(value.creation_token, prefix + ".creation_token"));
          validateLibraryCell(value.library_cell, prefix + ".library_cell", errors);
          validatePlacement(value.placement_inputs, prefix + ".placement_inputs", errors);
        } else if constexpr (std::is_same_v<Operation, RemoveInstance>) {
          append(errors, validateCreationToken(value.creation_token, prefix + ".creation_token"));
        } else if constexpr (std::is_same_v<Operation, AddTimingPoint>) {
          append(errors, validateStableNodeRef(StableNodeRef{value.node}, prefix + ".node"));
          const auto expected = value.node.role == CreatedNodeRole::kInput ? csr::PinDirection::kInput : csr::PinDirection::kOutput;
          if (value.direction != expected)
            reject(errors, TimingEditContractCode::kConflict, prefix + ".direction", "direction does not match created node role");
        } else if constexpr (std::is_same_v<Operation, RemoveTimingPoint>) {
          append(errors, validateStableNodeRef(StableNodeRef{value.node}, prefix + ".node"));
        } else if constexpr (std::is_same_v<Operation, AddCellArc> || std::is_same_v<Operation, RemoveCellArc>) {
          append(errors, validateStableArcRef(StableArcRef{value.arc}, prefix + ".arc"));
        } else if constexpr (std::is_same_v<Operation, AddNetArc> || std::is_same_v<Operation, RemoveNetArc>) {
          append(errors, validateStableArcRef(value.arc, prefix + ".arc"));
          if (std::holds_alternative<CreatedCellArcRef>(value.arc))
            reject(errors, TimingEditContractCode::kConflict, prefix + ".arc", "net operation cannot reference a cell arc");
        } else if constexpr (std::is_same_v<Operation, ReconnectSink>) {
          append(errors, validateStableNodeRef(value.sink, prefix + ".sink"));
          append(errors, validateStableNetRef(value.old_net, prefix + ".old_net"));
          append(errors, validateStableNetRef(value.new_net, prefix + ".new_net"));
          if (canonicalStableNetRefBytes(value.old_net) == canonicalStableNetRefBytes(value.new_net))
            reject(errors, TimingEditContractCode::kConflict, prefix, "reconnect old and new nets must differ");
        } else if constexpr (std::is_same_v<Operation, InvalidateLoad> || std::is_same_v<Operation, InvalidateParasitic>) {
          append(errors, validateStableNetRef(value.net, prefix + ".net"));
        } else {
          append(errors, validateStableArcRef(value.arc, prefix + ".arc"));
        }
      },
      operation);
}

template <typename A, typename R>
void rejectAddRemoveConflicts(const TimingGraphDelta& value, std::string_view field, Violations& errors)
{
  BytesSet added;
  BytesSet removed;
  for (const auto& operation : value.operations) {
    if (std::holds_alternative<A>(operation))
      added.insert(operationSubject(operation));
    if (std::holds_alternative<R>(operation))
      removed.insert(operationSubject(operation));
  }
  std::vector<Bytes> overlap;
  std::set_intersection(added.begin(), added.end(), removed.begin(), removed.end(), std::back_inserter(overlap));
  if (!overlap.empty())
    reject(errors, TimingEditContractCode::kConflict, std::string(field), "same semantic object is both added and removed");
}

template <typename Ref>
void canonicalizeRefs(std::vector<Ref>& values, auto encode)
{
  canonicalSort(values, encode);
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

InvalidationClosure closureFrom(const TimingGraphDelta& delta)
{
  InvalidationClosure result;
  for (const auto& operation : delta.operations)
    std::visit(
        [&](const auto& value) {
          using Operation = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Operation, InvalidateLoad>)
            result.load_nets.push_back(value.net);
          else if constexpr (std::is_same_v<Operation, InvalidateParasitic>)
            result.parasitic_nets.push_back(value.net);
          else if constexpr (std::is_same_v<Operation, InvalidateArcDelay>)
            result.arc_delays.push_back(value.arc);
        },
        operation);
  canonicalizeRefs(result.load_nets, canonicalStableNetRefBytes);
  canonicalizeRefs(result.parasitic_nets, canonicalStableNetRefBytes);
  canonicalizeRefs(result.arc_delays, canonicalStableArcRefBytes);
  return result;
}

}  // namespace

std::string_view token(CreatedNodeRole value)
{
  switch (value) {
    case CreatedNodeRole::kInput:
      return "input";
    case CreatedNodeRole::kOutput:
      return "output";
  }
  throw std::invalid_argument("unknown CreatedNodeRole");
}

std::string_view token(CreatedNetRole value)
{
  if (value == CreatedNetRole::kOutputNet)
    return "output_net";
  throw std::invalid_argument("unknown CreatedNetRole");
}

std::string_view token(ParasiticEditPolicy value)
{
  switch (value) {
    case ParasiticEditPolicy::kError:
      return "error";
    case ParasiticEditPolicy::kFullWithRecord:
      return "full_with_record";
  }
  throw std::invalid_argument("unknown ParasiticEditPolicy");
}

std::string_view token(EditDisposition value)
{
  switch (value) {
    case EditDisposition::kSupported:
      return "supported";
    case EditDisposition::kFullWithRecord:
      return "full_with_record";
  }
  throw std::invalid_argument("unknown EditDisposition");
}

std::string_view token(DeltaInvalidationKind value)
{
  switch (value) {
    case DeltaInvalidationKind::kLoad:
      return "load";
    case DeltaInvalidationKind::kParasitic:
      return "parasitic";
    case DeltaInvalidationKind::kArcDelay:
      return "arc_delay";
  }
  throw std::invalid_argument("unknown DeltaInvalidationKind");
}

std::string_view token(TimingEditContractCode value)
{
  switch (value) {
    case TimingEditContractCode::kSchemaMismatch: return "schema_mismatch";
    case TimingEditContractCode::kInvalidValue: return "invalid_value";
    case TimingEditContractCode::kInvalidToken: return "invalid_token";
    case TimingEditContractCode::kInvalidSemanticRef: return "invalid_semantic_ref";
    case TimingEditContractCode::kDuplicate: return "duplicate";
    case TimingEditContractCode::kConflict: return "conflict";
    case TimingEditContractCode::kGenerationMismatch: return "generation_mismatch";
    case TimingEditContractCode::kCapacityOverflow: return "capacity_overflow";
    case TimingEditContractCode::kUnsupported: return "unsupported";
    case TimingEditContractCode::kNotCanonical: return "not_canonical";
    case TimingEditContractCode::kIdentityMismatch: return "identity_mismatch";
  }
  throw std::invalid_argument("unknown TimingEditContractCode");
}

TimingEditContractViolations validateCreationToken(const CreationToken& value, std::string_view field)
{
  Violations errors;
  const std::string prefix(field);
  if (value.schema_version != kCreationTokenSchemaVersion)
    reject(errors, TimingEditContractCode::kSchemaMismatch, prefix + ".schema_version", "expected creation token schema 1");
  if (!validText(value.case_id))
    reject(errors, TimingEditContractCode::kInvalidToken, prefix + ".case_id", "must be nonempty and contain no NUL");
  if (!validText(value.profile_id))
    reject(errors, TimingEditContractCode::kInvalidToken, prefix + ".profile_id", "must be nonempty and contain no NUL");
  if (value.batch_ordinal == 0)
    reject(errors, TimingEditContractCode::kInvalidToken, prefix + ".batch_ordinal", "batch ordinal must be nonzero");
  return errors;
}

TimingEditContractViolations validateStableNodeRef(const StableNodeRef& value, std::string_view field)
{
  Violations errors;
  std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref, ExistingNodeRef>) {
          if (const auto error = csr::validateNodeKey(ref.baseline_key))
            reject(errors, TimingEditContractCode::kInvalidSemanticRef, std::string(field), *error);
        } else {
          append(errors, validateCreationToken(ref.creation_token, std::string(field) + ".creation_token"));
          try { static_cast<void>(token(ref.role)); } catch (const std::invalid_argument& error) {
            reject(errors, TimingEditContractCode::kInvalidSemanticRef, std::string(field) + ".role", error.what());
          }
        }
      },
      value);
  return errors;
}

TimingEditContractViolations validateStableNetRef(const StableNetRef& value, std::string_view field)
{
  Violations errors;
  std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref, ExistingNetRef>) {
          if (!validText(ref.baseline_name))
            reject(errors, TimingEditContractCode::kInvalidSemanticRef, std::string(field) + ".baseline_name", "must be nonempty and contain no NUL");
        } else {
          append(errors, validateCreationToken(ref.creation_token, std::string(field) + ".creation_token"));
          try { static_cast<void>(token(ref.role)); } catch (const std::invalid_argument& error) {
            reject(errors, TimingEditContractCode::kInvalidSemanticRef, std::string(field) + ".role", error.what());
          }
        }
      },
      value);
  return errors;
}

TimingEditContractViolations validateStableArcRef(const StableArcRef& value, std::string_view field)
{
  Violations errors;
  std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        const std::string prefix(field);
        if constexpr (std::is_same_v<Ref, ExistingArcRef>) {
          if (const auto error = csr::validateArcKeyBase(ref.baseline_key.base))
            reject(errors, TimingEditContractCode::kInvalidSemanticRef, prefix, *error);
        } else if constexpr (std::is_same_v<Ref, CreatedCellArcRef>) {
          append(errors, validateCreationToken(ref.creation_token, prefix + ".creation_token"));
          if (!validText(ref.library_source_port) || !validText(ref.library_sink_port) || ref.library_source_port == ref.library_sink_port)
            reject(errors, TimingEditContractCode::kInvalidSemanticRef, prefix, "cell arc ports must be distinct nonempty names");
          validateLibrarySignatures(ref.ordered_library_arcs, prefix + ".ordered_library_arcs", errors);
        } else if constexpr (std::is_same_v<Ref, CreatedOldNetArcRef>) {
          append(errors, validateStableNetRef(ref.net, prefix + ".net"));
          append(errors, validateStableNodeRef(ref.driver, prefix + ".driver"));
          append(errors, validateStableNodeRef(ref.sink, prefix + ".sink"));
          if (canonicalStableNodeRefBytes(ref.driver) == canonicalStableNodeRefBytes(ref.sink))
            reject(errors, TimingEditContractCode::kConflict, prefix, "net arc driver and sink must differ");
        } else {
          append(errors, validateCreationToken(ref.creation_token, prefix + ".creation_token"));
          append(errors, validateStableNodeRef(ref.driver, prefix + ".driver"));
          append(errors, validateStableNodeRef(ref.sink, prefix + ".sink"));
          if (canonicalStableNodeRefBytes(ref.driver) == canonicalStableNodeRefBytes(ref.sink))
            reject(errors, TimingEditContractCode::kConflict, prefix, "net arc driver and sink must differ");
        }
      },
      value);
  return errors;
}

TimingEditContractViolations validateBaselineSemanticIdentity(const BaselineSemanticIdentity& value, std::string_view field)
{
  Violations errors;
  if (!validText(value.design_name))
    reject(errors, TimingEditContractCode::kInvalidValue, std::string(field) + ".design_name", "must be nonempty and contain no NUL");
  validateFingerprints(value.fingerprints, std::string(field) + ".fingerprints", errors);
  validatePreState(value.canonical_pre_state, std::string(field) + ".canonical_pre_state", errors);
  return errors;
}

TimingEditContractViolations validateCommittedEditIdentity(const CommittedEditIdentity& value, std::string_view field)
{
  Violations errors;
  validateFingerprints(value.current_fingerprints, std::string(field) + ".current_fingerprints", errors);
  validatePreState(value.canonical_pre_state, std::string(field) + ".canonical_pre_state", errors);
  return errors;
}

TimingEditContractViolations validateEditBatch(const EditBatch& value)
{
  Violations errors;
  if (value.edits.empty())
    reject(errors, TimingEditContractCode::kInvalidValue, "plan.edits", "at least one edit is required");
  if (value.expected_generation.value() == std::numeric_limits<std::uint64_t>::max())
    reject(errors, TimingEditContractCode::kCapacityOverflow, "plan.expected_generation", "generation cannot advance");
  std::set<CreationToken> tokens;
  BytesSet original_nets;
  BytesSet moved_sinks;
  for (std::size_t index = 0; index < value.edits.size(); ++index) {
    const auto& edit = value.edits[index];
    const std::string prefix = "plan.edits[" + std::to_string(index) + "]";
    validateBuffer(edit, prefix, errors);
    if (!tokens.insert(edit.creation_token).second)
      reject(errors, TimingEditContractCode::kDuplicate, prefix + ".creation_token", "duplicate creation token");
    if (edit.creation_token.batch_ordinal != value.expected_generation.value() + 1)
      reject(errors, TimingEditContractCode::kGenerationMismatch, prefix + ".creation_token.batch_ordinal", "must equal expected generation plus one");
    if (!original_nets.insert(canonicalStableNetRefBytes(edit.original_net)).second)
      reject(errors, TimingEditContractCode::kConflict, prefix + ".original_net", "only one insertion per original net is allowed in a batch");
    for (const auto& sink : edit.moved_sinks)
      if (!moved_sinks.insert(canonicalStableNodeRefBytes(sink)).second)
        reject(errors, TimingEditContractCode::kConflict, prefix + ".moved_sinks", "sink is moved by more than one edit");
  }
  return errors;
}

EditBatchNormalization normalizeEditBatch(EditBatch value)
{
  for (auto& edit : value.edits)
    canonicalSort(edit.moved_sinks, canonicalStableNodeRefBytes);
  canonicalSort(value.edits, canonicalBufferInsertionMutationBytes);
  if (auto errors = validateEditBatch(value); !errors.empty())
    return errors;
  return value;
}

TimingEditContractViolations validateDesignEditPlan(const DesignEditPlan& value)
{
  Violations errors;
  if (value.schema_version != kDesignEditPlanSchema)
    reject(errors, TimingEditContractCode::kSchemaMismatch, "plan.schema_version", "expected timing-edit-plan/1.0.0");
  append(errors, validateBaselineSemanticIdentity(value.baseline, "plan.baseline"));
  append(errors, validateEditBatch({value.expected_generation, value.edits}));
  return errors;
}

DesignEditPlanNormalization normalizeDesignEditPlan(DesignEditPlan value)
{
  for (auto& edit : value.edits)
    canonicalSort(edit.moved_sinks, canonicalStableNodeRefBytes);
  canonicalSort(value.edits, canonicalBufferInsertionMutationBytes);
  if (auto errors = validateDesignEditPlan(value); !errors.empty())
    return errors;
  return value;
}

TimingEditContractViolations validateTimingGraphDelta(const TimingGraphDelta& value, std::string_view field)
{
  Violations errors;
  if (value.schema_version != kTimingGraphDeltaSchema)
    reject(errors, TimingEditContractCode::kSchemaMismatch, std::string(field) + ".schema_version", "expected timing-graph-delta/1.0.0");
  if (value.operations.empty())
    reject(errors, TimingEditContractCode::kInvalidValue, std::string(field) + ".operations", "delta cannot be empty");
  BytesSet operations;
  BytesSet reconnect_sinks;
  for (std::size_t index = 0; index < value.operations.size(); ++index) {
    const std::string prefix = std::string(field) + ".operations[" + std::to_string(index) + "]";
    validateOperation(value.operations[index], prefix, errors);
    if (!operations.insert(canonicalTimingGraphEditBytes(value.operations[index])).second)
      reject(errors, TimingEditContractCode::kDuplicate, prefix, "duplicate delta operation");
    if (const auto* reconnect = std::get_if<ReconnectSink>(&value.operations[index]);
        reconnect && !reconnect_sinks.insert(canonicalStableNodeRefBytes(reconnect->sink)).second)
      reject(errors, TimingEditContractCode::kConflict, prefix + ".sink", "sink is reconnected more than once");
  }
  rejectAddRemoveConflicts<AddInstance, RemoveInstance>(value, std::string(field) + ".operations", errors);
  rejectAddRemoveConflicts<AddTimingPoint, RemoveTimingPoint>(value, std::string(field) + ".operations", errors);
  rejectAddRemoveConflicts<AddCellArc, RemoveCellArc>(value, std::string(field) + ".operations", errors);
  rejectAddRemoveConflicts<AddNetArc, RemoveNetArc>(value, std::string(field) + ".operations", errors);
  return errors;
}

TimingGraphDeltaNormalization normalizeTimingGraphDelta(TimingGraphDelta value, std::string_view field)
{
  std::sort(value.operations.begin(), value.operations.end(), [](const TimingGraphEdit& lhs, const TimingGraphEdit& rhs) {
    const auto lhs_rank = timingGraphEditCanonicalRank(lhs);
    const auto rhs_rank = timingGraphEditCanonicalRank(rhs);
    if (lhs_rank != rhs_rank)
      return lhs_rank < rhs_rank;
    return detail::canonicalBytesLess(canonicalTimingGraphEditBytes(lhs), canonicalTimingGraphEditBytes(rhs));
  });
  value.operations.erase(std::unique(value.operations.begin(), value.operations.end()), value.operations.end());
  if (auto errors = validateTimingGraphDelta(value, field); !errors.empty())
    return errors;
  return value;
}

TokenMaterialization defaultTokenMaterialization(const CreationToken& token_value, const LibraryCellRef& library_cell)
{
  const std::string suffix = "_b" + std::to_string(token_value.batch_ordinal) + "_e" + std::to_string(token_value.accepted_ordinal);
  TokenMaterialization result;
  result.creation_token = token_value;
  result.instance_name = "__ista_buf" + suffix;
  result.output_net_name = "__ista_net" + suffix;
  result.input_pin_name = result.instance_name + ":" + library_cell.input_port;
  result.output_pin_name = result.instance_name + ":" + library_cell.output_port;
  return result;
}

TimingEditContractViolations validatePreparedBatchData(const PreparedBatchData& value)
{
  Violations errors;
  if (value.session_token.value() == 0)
    reject(errors, TimingEditContractCode::kInvalidToken, "prepared.session_token", "session token must be nonzero");
  if (value.expected_generation.value() == std::numeric_limits<std::uint64_t>::max()
      || value.target_generation.value() != value.expected_generation.value() + 1)
    reject(errors, TimingEditContractCode::kGenerationMismatch, "prepared.target_generation", "must equal expected generation plus one");
  if (!value.expected_pre_committed_identity || value.expected_pre_committed_identity->generation != value.expected_generation)
    reject(errors, TimingEditContractCode::kGenerationMismatch, "prepared.expected_pre_committed_identity.generation", "must equal expected generation");
  if (!value.baseline)
    reject(errors, TimingEditContractCode::kIdentityMismatch, "prepared.baseline", "baseline owner is absent");
  else
    append(errors, validateBaselineSemanticIdentity(*value.baseline, "prepared.baseline"));
  if (value.expected_pre_committed_identity)
    append(errors, validateCommittedEditIdentity(*value.expected_pre_committed_identity, "prepared.expected_pre_committed_identity"));
  EditBatch plan{value.expected_generation, value.normalized_edits};
  auto normalized_plan = normalizeEditBatch(plan);
  if (const auto* plan_errors = std::get_if<Violations>(&normalized_plan))
    append(errors, *plan_errors);
  else if (std::get<EditBatch>(normalized_plan) != plan)
    reject(errors, TimingEditContractCode::kNotCanonical, "prepared.normalized_edits", "edits are not normalized");
  auto normalized_forward = normalizeTimingGraphDelta(value.normalized_forward, "prepared.normalized_forward");
  if (const auto* delta_errors = std::get_if<Violations>(&normalized_forward))
    append(errors, *delta_errors);
  else if (std::get<TimingGraphDelta>(normalized_forward) != value.normalized_forward)
    reject(errors, TimingEditContractCode::kNotCanonical, "prepared.normalized_forward", "delta is not normalized");
  auto normalized_reverse = normalizeTimingGraphDelta(value.normalized_reverse, "prepared.normalized_reverse");
  if (const auto* delta_errors = std::get_if<Violations>(&normalized_reverse))
    append(errors, *delta_errors);
  else if (std::get<TimingGraphDelta>(normalized_reverse) != value.normalized_reverse)
    reject(errors, TimingEditContractCode::kNotCanonical, "prepared.normalized_reverse", "delta is not normalized");

  append(errors, detail::validatePreparedBatchDeltaSemantics(value.normalized_forward, value.normalized_reverse));
  const auto& before = value.slot_plan.before;
  const auto& after = value.slot_plan.after;
  if (after.node_slots < before.node_slots || after.net_slots < before.net_slots || after.arc_slots < before.arc_slots)
    reject(errors, TimingEditContractCode::kCapacityOverflow, "prepared.slot_plan.after", "append-only high-water cannot decrease");
  validateAssignmentList(value.slot_plan.nodes, before.node_slots, after.node_slots, "prepared.slot_plan.nodes", canonicalStableNodeRefBytes,
                         validateStableNodeRef, errors);
  validateAssignmentList(value.slot_plan.nets, before.net_slots, after.net_slots, "prepared.slot_plan.nets", canonicalStableNetRefBytes,
                         validateStableNetRef, errors);
  validateAssignmentList(value.slot_plan.arcs, before.arc_slots, after.arc_slots, "prepared.slot_plan.arcs", canonicalStableArcRefBytes,
                         validateStableArcRef, errors);

  std::set<CreationToken> materialized_tokens;
  std::set<std::string> materialized_names;
  for (std::size_t index = 0; index < value.token_materializations.size(); ++index) {
    const auto& materialization = value.token_materializations[index];
    const std::string field = "prepared.token_materializations[" + std::to_string(index) + "]";
    append(errors, validateCreationToken(materialization.creation_token, field + ".creation_token"));
    if (!materialized_tokens.insert(materialization.creation_token).second)
      reject(errors, TimingEditContractCode::kDuplicate, field + ".creation_token", "token is materialized more than once");
    for (const auto* name : {&materialization.instance_name, &materialization.output_net_name, &materialization.input_pin_name,
                             &materialization.output_pin_name})
      if (!validText(*name) || !materialized_names.insert(*name).second)
        reject(errors, TimingEditContractCode::kConflict, field, "materialized names must be nonempty, NUL-free and globally unique");
    const auto edit = std::find_if(value.normalized_edits.begin(), value.normalized_edits.end(),
                                   [&](const BufferInsertionMutation& candidate) { return candidate.creation_token == materialization.creation_token; });
    if (edit == value.normalized_edits.end() || defaultTokenMaterialization(materialization.creation_token, edit->library_cell) != materialization)
      reject(errors, TimingEditContractCode::kIdentityMismatch, field, "materialization does not match the canonical token-derived names");
  }
  if (materialized_tokens.size() != value.normalized_edits.size())
    reject(errors, TimingEditContractCode::kIdentityMismatch, "prepared.token_materializations", "every normalized edit must be materialized exactly once");

  InvalidationClosure expected_closure = closureFrom(value.normalized_forward);
  InvalidationClosure actual_closure = value.invalidation_closure;
  canonicalizeRefs(actual_closure.load_nets, canonicalStableNetRefBytes);
  canonicalizeRefs(actual_closure.parasitic_nets, canonicalStableNetRefBytes);
  canonicalizeRefs(actual_closure.arc_delays, canonicalStableArcRefBytes);
  if (actual_closure != value.invalidation_closure)
    reject(errors, TimingEditContractCode::kNotCanonical, "prepared.invalidation_closure", "closure lists must be canonical and duplicate-free");
  if (actual_closure != expected_closure)
    reject(errors, TimingEditContractCode::kIdentityMismatch, "prepared.invalidation_closure", "closure must exactly match forward delta invalidations");

  bool requires_full = false;
  for (const auto& edit : value.normalized_edits)
    if (const auto* rc = std::get_if<ParasiticRcInputs>(&edit.rc_inputs))
      requires_full = requires_full || rc->parasitic_policy == ParasiticEditPolicy::kFullWithRecord;
  try {
    static_cast<void>(token(value.policy_receipt.disposition));
  } catch (const std::invalid_argument& error) {
    reject(errors, TimingEditContractCode::kInvalidValue, "prepared.policy_receipt.disposition",
           error.what());
  }
  if (requires_full != (value.policy_receipt.disposition == EditDisposition::kFullWithRecord))
    reject(errors, TimingEditContractCode::kConflict, "prepared.policy_receipt.disposition", "does not match edit fallback policy");
  if (requires_full && value.policy_receipt.reasons.empty())
    reject(errors, TimingEditContractCode::kInvalidValue, "prepared.policy_receipt.reasons", "full_with_record requires an explicit reason");
  if (!requires_full && !value.policy_receipt.reasons.empty())
    reject(errors, TimingEditContractCode::kConflict, "prepared.policy_receipt.reasons", "supported batch cannot carry fallback reasons");
  if (!std::is_sorted(value.policy_receipt.reasons.begin(), value.policy_receipt.reasons.end())
      || std::adjacent_find(value.policy_receipt.reasons.begin(), value.policy_receipt.reasons.end()) != value.policy_receipt.reasons.end())
    reject(errors, TimingEditContractCode::kNotCanonical, "prepared.policy_receipt.reasons", "reasons must be sorted and duplicate-free");
  return errors;
}
PreparedBatch PreparedBatch::create(PreparedBatchData data)
{
  // Public input may retain mutable aliases even when passed as const.
  if (data.baseline)
    data.baseline = std::make_shared<const BaselineSemanticIdentity>(*data.baseline);
  if (auto errors = validatePreparedBatchData(data); !errors.empty())
    throw std::invalid_argument(errors.front().field + ": " + errors.front().message);
  return PreparedBatch(std::move(data));
}



}  // namespace ista::timing_edit
