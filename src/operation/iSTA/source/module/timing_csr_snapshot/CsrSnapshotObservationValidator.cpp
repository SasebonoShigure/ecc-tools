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
#include "CsrSnapshotObservationValidator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "CsrFifoLevelization.hpp"
#include "CsrFingerprint.hpp"
#include "CsrSnapshotInvalidationPolicyInternal.hpp"
#include "CsrSnapshotStorage.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr {
namespace {

using Violations = std::vector<CsrContractViolation>;

void reject(Violations& errors, std::string field, std::string message)
{
  errors.push_back({std::move(field), std::move(message)});
}

void append(Violations& errors, Violations additions)
{
  errors.insert(errors.end(), std::make_move_iterator(additions.begin()), std::make_move_iterator(additions.end()));
}

template <typename Enum>
bool knownEnum(Enum value)
{
  try {
    static_cast<void>(token(value));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool validDuration(double value)
{
  return std::isfinite(value) && value >= 0.0;
}

bool sameFingerprints(const CsrSnapshotIdentity& lhs, const CsrSnapshotIdentity& rhs)
{
  return lhs.topology_fingerprint == rhs.topology_fingerprint && lhs.clock_constraint_fingerprint == rhs.clock_constraint_fingerprint
         && lhs.library_fingerprint == rhs.library_fingerprint && lhs.corner_fingerprint == rhs.corner_fingerprint
         && lhs.parasitic_fingerprint == rhs.parasitic_fingerprint && lhs.timing_input_fingerprint == rhs.timing_input_fingerprint;
}

void validateExtent(const CsrBufferExtent& extent, std::uint64_t expected, std::string_view field, Violations& errors)
{
  if (extent.logical_size != expected)
    reject(errors, std::string(field) + ".logical_size", "logical size does not match the frozen count");
  if (extent.capacity < extent.logical_size)
    reject(errors, std::string(field), "capacity must be at least logical size");
}

void validateSupport(const CsrSupportObservation& support, Violations& errors)
{
  bool enums_known = true;
  for (CsrReferenceUnsupportedReason reason : support.unsupported_reasons)
    if (!knownEnum(reason)) {
      reject(errors, "observation.support.unsupported_reasons", "unsupported reason enum is unknown");
      enums_known = false;
    }
  if (enums_known && (!std::ranges::is_sorted(support.unsupported_reasons, {}, [](auto value) {
        return static_cast<std::uint8_t>(value);
      }) || std::ranges::adjacent_find(support.unsupported_reasons) != support.unsupported_reasons.end()))
    reject(errors, "observation.support.unsupported_reasons", "reasons must follow stable unique enum order");
  if (support.reference_supported != support.unsupported_reasons.empty())
    reject(errors, "observation.support", "reference_supported must match unsupported reasons");
}

void validateRejection(const CsrSnapshotObservation& observation, const CsrScatterRejectionEvent& event, Violations& errors)
{
  const bool reason_known = knownEnum(event.reason);
  const bool allowed = reason_known && event.reason >= CsrScatterCode::kSchemaMismatch && event.reason <= CsrScatterCode::kPredecessorMismatch;
  if (!allowed)
    reject(errors, "observation.last_rejection.reason", "reason must be one of the eight rejection codes");
  if (event.writes_applied != 0)
    reject(errors, "observation.last_rejection.writes_applied", "rejection must apply zero writes");
  append(errors, validateStage2CommittedIdentity(event.current, "observation.last_rejection.current"));
  if (!(event.current == observation.identity))
    reject(errors, "observation.last_rejection.current", "current identity must equal the outer identity");
  if (!allowed)
    return;

  if (event.reason == CsrScatterCode::kSchemaMismatch) {
    if (event.source)
      reject(errors, "observation.last_rejection", "rejection reason and source identity disagree");
    return;
  }
  if (!event.source) {
    reject(errors, "observation.last_rejection", "rejection reason and source identity disagree");
    return;
  }
  append(errors, validateSnapshotIdentitySyntax(*event.source, "observation.last_rejection.source"));
  switch (event.reason) {
    case CsrScatterCode::kSessionMismatch:
      if (event.source->session_token == event.current.session_token)
        reject(errors, "observation.last_rejection", "rejection reason and source identity disagree");
      break;
    case CsrScatterCode::kGenerationMismatch:
      if (event.source->session_token != event.current.session_token || event.source->graph_generation == event.current.graph_generation)
        reject(errors, "observation.last_rejection", "rejection reason and source identity disagree");
      break;
    case CsrScatterCode::kSourceIdentityMismatch:
      if (event.source->session_token != event.current.session_token || event.source->graph_generation != event.current.graph_generation
          || sameFingerprints(*event.source, event.current))
        reject(errors, "observation.last_rejection", "rejection reason and source identity disagree");
      break;
    case CsrScatterCode::kShapeMismatch:
    case CsrScatterCode::kResultNumericInvalid:
    case CsrScatterCode::kIdMappingMismatch:
    case CsrScatterCode::kPredecessorMismatch:
      if (!(*event.source == event.current))
        reject(errors, "observation.last_rejection", "rejection reason and source identity disagree");
      break;
    default:
      break;
  }
}

void validateInvalidation(const CsrSnapshotObservation& observation, const CsrInvalidationEvent& event, Violations& errors)
{
  if (!knownEnum(event.reason))
    reject(errors, "observation.last_invalidation.reason", "invalidation reason enum is unknown");
  if (event.writes_applied != 0)
    reject(errors, "observation.last_invalidation.writes_applied", "invalidation must apply zero writes");
  append(errors, validateStage2CommittedIdentity(event.previous_identity, "observation.last_invalidation.previous_identity"));
  if (!(event.previous_identity == observation.identity))
    reject(errors, "observation.last_invalidation.previous_identity", "previous identity must equal the outer identity");

  const std::array<std::pair<std::string_view, const std::optional<std::string>*>, 6> fingerprints
      = {{{"topology_fingerprint", &event.current_fingerprints.topology_fingerprint},
          {"clock_constraint_fingerprint", &event.current_fingerprints.clock_constraint_fingerprint},
          {"library_fingerprint", &event.current_fingerprints.library_fingerprint},
          {"corner_fingerprint", &event.current_fingerprints.corner_fingerprint},
          {"parasitic_fingerprint", &event.current_fingerprints.parasitic_fingerprint},
          {"timing_input_fingerprint", &event.current_fingerprints.timing_input_fingerprint}}};
  for (const auto& [name, fingerprint] : fingerprints)
    if (*fingerprint && !isLowercaseSha256(**fingerprint))
      reject(errors, "observation.last_invalidation.current_fingerprints." + std::string(name),
             "fingerprint must be null or 64 lowercase hexadecimal characters");

  const auto expected = snapshot_internal::firstInvalidationReason(observation.identity, event.current_fingerprints);
  if (!expected)
    reject(errors, "observation.last_invalidation.current_fingerprints", "at least one fingerprint must be absent or different");
  else if (knownEnum(event.reason) && event.reason != *expected)
    reject(errors, "observation.last_invalidation.reason", "must identify the first changed fingerprint");
}

}  // namespace

std::vector<CsrContractViolation> validateCaptureMetadata(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata)
{
  Violations errors = validateReferenceInput(input);
  if (!errors.empty())
    return errors;

  const std::uint64_t node_count = static_cast<std::uint64_t>(input.nodes.size());
  bool offsets_shape = true;
  if (metadata.level_offsets.empty()) {
    reject(errors, "metadata.level_offsets", "offsets must be nonempty");
    offsets_shape = false;
  } else {
    if (metadata.level_offsets.front() != 0)
      reject(errors, "metadata.level_offsets", "offsets must start at zero");
    if (!std::ranges::is_sorted(metadata.level_offsets))
      reject(errors, "metadata.level_offsets", "offsets must be monotonic");
    if (metadata.level_offsets.back() != node_count)
      reject(errors, "metadata.level_offsets", "final offset must equal node count");
    offsets_shape = metadata.level_offsets.front() == 0 && std::ranges::is_sorted(metadata.level_offsets) && metadata.level_offsets.back() == node_count;
  }
  if (metadata.level_nodes.size() != input.nodes.size())
    reject(errors, "metadata.level_nodes", "level nodes must contain exactly N entries");
  if (metadata.node_to_level.size() != input.nodes.size())
    reject(errors, "metadata.node_to_level", "node-to-level must contain exactly N entries");

  std::vector<std::uint8_t> seen(input.nodes.size(), 0);
  for (NodeId node : metadata.level_nodes) {
    if (node.value() >= node_count)
      reject(errors, "metadata.level_nodes", "NodeId is out of range");
    else if (seen[node.value()] != 0)
      reject(errors, "metadata.level_nodes", "NodeId is duplicated");
    else
      seen[node.value()] = 1;
  }
  if (metadata.level_nodes.size() == input.nodes.size() && std::ranges::find(seen, std::uint8_t{0}) != seen.end())
    reject(errors, "metadata.level_nodes", "level nodes must be a permutation of [0,N)");

  if (offsets_shape && metadata.level_nodes.size() == input.nodes.size() && metadata.node_to_level.size() == input.nodes.size()) {
    for (std::size_t level = 0; level + 1 < metadata.level_offsets.size(); ++level) {
      for (std::uint64_t offset = metadata.level_offsets[level]; offset < metadata.level_offsets[level + 1]; ++offset) {
        const NodeId node = metadata.level_nodes[offset];
        if (node.value() < node_count && metadata.node_to_level[node.value()] != level + 1)
          reject(errors, "metadata.node_to_level", "node-to-level must be the inverse of level buckets");
      }
    }
  }

  const CsrFifoLevelization replay = replayFifoLevelization(input);
  if (!replay.complete)
    reject(errors, "metadata", "FIFO replay must be complete");
  if (metadata.level_offsets != replay.level_offsets)
    reject(errors, "metadata.level_offsets", "must exactly match FIFO replay");
  if (metadata.level_nodes != replay.level_nodes)
    reject(errors, "metadata.level_nodes", "must exactly match FIFO replay");
  if (metadata.node_to_level != replay.node_to_level)
    reject(errors, "metadata.node_to_level", "must exactly match FIFO replay");
  return errors;
}

std::vector<CsrContractViolation> validateSnapshotObservation(const CsrSnapshotObservation& observation)
{
  Violations errors;
  if (observation.schema_version != kCsrSnapshotObservationSchemaVersion)
    reject(errors, "observation.schema_version", "unsupported snapshot observation schema version");
  const bool state_known = knownEnum(observation.state);
  if (!state_known)
    reject(errors, "observation.state", "unknown snapshot state");
  append(errors, validateStage2CommittedIdentity(observation.identity, "observation.identity"));

  const auto& counts = observation.counts;
  const auto& buffers = observation.buffers;
  validateExtent(buffers.node_keys, counts.nodes, "observation.buffers.node_keys", errors);
  validateExtent(buffers.arc_keys, counts.arcs, "observation.buffers.arc_keys", errors);
  if (counts.nodes == std::numeric_limits<std::uint64_t>::max()) {
    reject(errors, "observation.counts.nodes", "node count plus one overflows u64");
  } else {
    validateExtent(buffers.out_row_offsets, counts.nodes + 1, "observation.buffers.out_row_offsets", errors);
    validateExtent(buffers.in_row_offsets, counts.nodes + 1, "observation.buffers.in_row_offsets", errors);
  }
  validateExtent(buffers.out_arc_ids, counts.arcs, "observation.buffers.out_arc_ids", errors);
  validateExtent(buffers.in_arc_ids, counts.arcs, "observation.buffers.in_arc_ids", errors);
  if (counts.levels == std::numeric_limits<std::uint64_t>::max())
    reject(errors, "observation.counts.levels", "level count plus one overflows u64");
  else
    validateExtent(buffers.level_offsets, counts.levels + 1, "observation.buffers.level_offsets", errors);
  validateExtent(buffers.level_nodes, counts.nodes, "observation.buffers.level_nodes", errors);
  validateExtent(buffers.node_to_level, counts.nodes, "observation.buffers.node_to_level", errors);
  if ((counts.nodes == 0) != (counts.levels == 0))
    reject(errors, "observation.counts.levels", "N is zero if and only if L is zero");
  if (counts.nodes == 0 && counts.arcs != 0)
    reject(errors, "observation.counts.arcs", "empty graph cannot contain arcs");
  if (counts.nodes != 0 && (counts.levels == 0 || counts.levels > counts.nodes))
    reject(errors, "observation.counts.levels", "nonempty observations require 1 <= L <= N");

  if (!knownEnum(observation.capacity.last_action))
    reject(errors, "observation.capacity.last_action", "capacity action enum is unknown");
  if (observation.capacity.event_count != 1)
    reject(errors, "observation.capacity.event_count", "schema 1.0.0 requires exactly one initial allocation event");
  validateSupport(observation.support, errors);
  if (!validDuration(observation.timing_ms.export_ms))
    reject(errors, "observation.timing_ms.export", "must be finite and nonnegative");
  if (observation.timing_ms.reference_ms && !validDuration(*observation.timing_ms.reference_ms))
    reject(errors, "observation.timing_ms.reference", "must be finite and nonnegative");
  if (observation.timing_ms.scatter_ms && !validDuration(*observation.timing_ms.scatter_ms))
    reject(errors, "observation.timing_ms.scatter", "must be finite and nonnegative");
  const std::uint64_t root_bytes = static_cast<std::uint64_t>(sizeof(CsrReferenceInput)) + static_cast<std::uint64_t>(sizeof(CsrCaptureMetadata));
  if (observation.host_bytes.logical < root_bytes)
    reject(errors, "observation.host_bytes.logical", "modeled host bytes must include both root objects");
  if (observation.host_bytes.capacity < root_bytes)
    reject(errors, "observation.host_bytes.capacity", "modeled host bytes must include both root objects");
  if (observation.host_bytes.logical > observation.host_bytes.capacity)
    reject(errors, "observation.host_bytes.capacity", "capacity bytes must be at least logical bytes");

  if (observation.last_rejection)
    validateRejection(observation, *observation.last_rejection, errors);
  if (observation.last_invalidation)
    validateInvalidation(observation, *observation.last_invalidation, errors);
  if ((observation.last_rejection || observation.last_invalidation) && !observation.timing_ms.scatter_ms)
    reject(errors, "observation.timing_ms.scatter", "an event requires a scatter duration");

  if (state_known) {
    if (observation.state == CsrSnapshotState::kReady && observation.last_invalidation)
      reject(errors, "observation.last_invalidation", "ready state forbids invalidation history");
    if (observation.state == CsrSnapshotState::kInvalidated && (observation.last_rejection || !observation.last_invalidation))
      reject(errors, "observation.state", "invalidated state requires only an invalidation event");
    if (observation.state == CsrSnapshotState::kReleased && observation.last_rejection && observation.last_invalidation)
      reject(errors, "observation.state", "released state may preserve only one event family");
  }
  return errors;
}

CsrObservationResult makeReadySnapshotObservation(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata, double export_ms)
{
  Violations errors = validateCaptureMetadata(input, metadata);
  if (!errors.empty())
    return errors;
  CsrSnapshotStorageResult storage = snapshotStorage(input, metadata);
  if (const auto* storage_errors = std::get_if<Violations>(&storage))
    return *storage_errors;
  const auto& report = std::get<CsrSnapshotStorageReport>(storage);

  CsrSnapshotObservation observation;
  observation.identity = input.source_identity;
  observation.counts = {report.buffers.node_keys.logical_size, report.buffers.arc_keys.logical_size, report.buffers.level_offsets.logical_size - 1};
  observation.buffers = report.buffers;
  observation.support = {input.unsupported_reasons.empty(), input.unsupported_reasons};
  observation.timing_ms.export_ms = export_ms;
  observation.host_bytes = report.host_bytes;
  errors = validateSnapshotObservation(observation);
  if (!errors.empty())
    return errors;
  return observation;
}

}  // namespace ista::csr
