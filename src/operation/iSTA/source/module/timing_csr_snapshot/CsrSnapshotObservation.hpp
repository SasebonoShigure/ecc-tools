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
#include <optional>
#include <string>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

inline constexpr const char* kCsrSnapshotObservationSchemaVersion = "1.0.0";

struct CsrSnapshotCounts
{
  std::uint64_t nodes{0};
  std::uint64_t arcs{0};
  std::uint64_t levels{0};

  friend bool operator==(const CsrSnapshotCounts&, const CsrSnapshotCounts&) = default;
};

struct CsrSnapshotBufferMetrics
{
  CsrBufferExtent node_keys;
  CsrBufferExtent arc_keys;
  CsrBufferExtent out_row_offsets;
  CsrBufferExtent out_arc_ids;
  CsrBufferExtent in_row_offsets;
  CsrBufferExtent in_arc_ids;
  CsrBufferExtent level_offsets;
  CsrBufferExtent level_nodes;
  CsrBufferExtent node_to_level;

  friend bool operator==(const CsrSnapshotBufferMetrics&, const CsrSnapshotBufferMetrics&) = default;
};

struct CsrCapacityObservation
{
  CapacityAction last_action{CapacityAction::kInitialAllocate};
  std::uint64_t event_count{1};

  friend bool operator==(const CsrCapacityObservation&, const CsrCapacityObservation&) = default;
};

struct CsrSupportObservation
{
  bool reference_supported{true};
  std::vector<CsrReferenceUnsupportedReason> unsupported_reasons;

  friend bool operator==(const CsrSupportObservation&, const CsrSupportObservation&) = default;
};

struct CsrTimingObservation
{
  double export_ms{0.0};
  std::optional<double> reference_ms;
  std::optional<double> scatter_ms;

  friend bool operator==(const CsrTimingObservation&, const CsrTimingObservation&) = default;
};

struct CsrModeledHostBytes
{
  std::uint64_t logical{0};
  std::uint64_t capacity{0};

  friend bool operator==(const CsrModeledHostBytes&, const CsrModeledHostBytes&) = default;
};

struct CsrScatterRejectionEvent
{
  CsrScatterCode reason{CsrScatterCode::kShapeMismatch};
  std::uint64_t writes_applied{0};
  std::optional<CsrSnapshotIdentity> source;
  CsrSnapshotIdentity current;

  friend bool operator==(const CsrScatterRejectionEvent&, const CsrScatterRejectionEvent&) = default;
};

struct CsrInvalidationEvent
{
  CsrInvalidationReason reason{CsrInvalidationReason::kTopologyChanged};
  std::uint64_t writes_applied{0};
  CsrSnapshotIdentity previous_identity;
  CsrCurrentFingerprints current_fingerprints;

  friend bool operator==(const CsrInvalidationEvent&, const CsrInvalidationEvent&) = default;
};

struct CsrSnapshotObservation
{
  std::string schema_version{kCsrSnapshotObservationSchemaVersion};
  CsrSnapshotState state{CsrSnapshotState::kReady};
  CsrSnapshotIdentity identity;
  CsrSnapshotCounts counts;
  CsrSnapshotBufferMetrics buffers;
  CsrCapacityObservation capacity;
  CsrSupportObservation support;
  CsrTimingObservation timing_ms;
  CsrModeledHostBytes host_bytes;
  std::optional<CsrScatterRejectionEvent> last_rejection;
  std::optional<CsrInvalidationEvent> last_invalidation;

  friend bool operator==(const CsrSnapshotObservation&, const CsrSnapshotObservation&) = default;
};

using CsrOptionalObservation = std::optional<CsrSnapshotObservation>;

}  // namespace ista::csr
