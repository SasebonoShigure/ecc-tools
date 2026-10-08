// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "ActiveCsrEdgeView.hpp"
#include "DualCsrTelemetry.hpp"
#include "DualCsrTypes.hpp"

namespace ista::timing_dual_csr {

struct BaseCsrState
{
  std::vector<std::uint64_t> row_offsets;
  std::vector<std::optional<csr::ArcId>> physical_slots;
  csr::CsrBufferExtent offsets_extent;
  csr::CsrBufferExtent edge_extent;

  friend bool operator==(const BaseCsrState&, const BaseCsrState&) = default;
};

struct AuxOutCsrState
{
  // Stable row slots retain their allocation when a row becomes empty.
  std::vector<std::vector<csr::ArcId>> row_arcs;
  std::vector<csr::NodeId> row_sources;
  std::vector<std::int64_t> source_to_aux_row;
  csr::CsrBufferExtent rows_extent;
  csr::CsrBufferExtent source_extent;
  csr::CsrBufferExtent mapping_extent;

  friend bool operator==(const AuxOutCsrState&, const AuxOutCsrState&) = default;
};

struct DualCsrArcSlot
{
  csr::ArcSemanticPayload payload;
  bool active{false};

  friend bool operator==(const DualCsrArcSlot&, const DualCsrArcSlot&) = default;
};

struct DualCsrTimingResult
{
  csr::CsrTimingResult values;

  friend bool operator==(const DualCsrTimingResult&,
                         const DualCsrTimingResult&) = default;
};

struct DualCsrState
{
  timing_edit::TimingEditSessionToken session_token;
  timing_edit::TransactionGeneration generation;
  timing_edit::AppendOnlyHighWater high_water;
  std::vector<csr::NodeRecord> nodes;
  std::vector<DualCsrArcSlot> arc_slots;
  BaseCsrState base_out;
  BaseCsrState base_in;
  AuxOutCsrState aux_out;
  DualCsrReferencePayload reference_payload;
  // Stable raw-RC slots; built once and extended with each committed net.
  std::map<std::string, std::size_t, std::less<>> parasitic_net_slots;
  DualCsrTelemetry telemetry;

  friend bool operator==(const DualCsrState&, const DualCsrState&) = default;
};

}  // namespace ista::timing_dual_csr
