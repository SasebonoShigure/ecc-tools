// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "AffectedCapturePlan.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::timing_dual_csr::detail {

struct CapturedNode
{
  timing_edit::StableNodeRef ref;
  csr::NodeId id;
  csr::NodeRecord record;
  friend bool operator==(const CapturedNode&, const CapturedNode&) = default;
};

struct CapturedInstance
{
  timing_edit::CreationToken creation_token;
  csr::InstanceRecord record;
  friend bool operator==(const CapturedInstance&,
                         const CapturedInstance&) = default;
};

struct CapturedArc
{
  timing_edit::StableArcRef ref;
  csr::ArcId id;
  bool active{true};
  csr::ArcSemanticPayload payload;
  friend bool operator==(const CapturedArc&, const CapturedArc&) = default;
};

struct CapturedOutgoingRow
{
  csr::NodeId source;
  std::vector<csr::ArcId> active_arc_ids;
  friend bool operator==(const CapturedOutgoingRow&,
                         const CapturedOutgoingRow&) = default;
};

struct CapturedIncomingRow
{
  csr::NodeId sink;
  std::vector<csr::ArcId> active_arc_ids;
  friend bool operator==(const CapturedIncomingRow&,
                         const CapturedIncomingRow&) = default;
};

struct CapturedNetLoad
{
  timing_edit::StableNetRef ref;
  std::string net_name;
  csr::ArcOutputLoadMatrix output_load{};
  friend bool operator==(const CapturedNetLoad&,
                         const CapturedNetLoad&) = default;
};

struct CapturedLibraryCell
{
  timing_edit::LibraryCellRef requested;
  csr::ReferencedLibraryCellModel model;
  friend bool operator==(const CapturedLibraryCell&,
                         const CapturedLibraryCell&) = default;
};

struct CapturedNodeRole
{
  AffectedNodeModelCapture requested;
  csr::NodeId id;
  csr::NodeRecord record;
  friend bool operator==(const CapturedNodeRole&,
                         const CapturedNodeRole&) = default;
};

struct CapturedCellPinCapacitance
{
  AffectedNodeModelCapture requested;
  csr::CellPinCapacitanceModel model;
  friend bool operator==(const CapturedCellPinCapacitance&,
                         const CapturedCellPinCapacitance&) = default;
};

struct NarrowTimingRecords
{
  std::vector<CapturedNode> nodes;
  std::vector<CapturedInstance> instances;
  std::vector<CapturedArc> arcs;
  std::vector<CapturedOutgoingRow> outgoing_rows;
  std::vector<CapturedIncomingRow> incoming_rows;
  std::vector<CapturedNetLoad> net_loads;
  std::vector<CapturedLibraryCell> library_cells;
  std::vector<CapturedNodeRole> node_roles;
  std::vector<CapturedCellPinCapacitance> cell_pin_capacitances;
  DualCsrReferencePayloadDelta reference_payload;
  NarrowCaptureAudit audit;
};

struct NativeCaptureResult
{
  std::optional<NarrowTimingRecords> records;
  std::optional<NarrowCaptureError> error;
  NarrowCaptureAudit audit;

  [[nodiscard]] bool success() const noexcept
  {
    return records.has_value() && !error
           && (!incregpusta_debug_checks_enabled
               || (audit.full_export_guard_active && !audit.full_export_observed && audit.native_full_graph_exports == 0
                   && audit.native_full_reference_exports == 0));
  }
};

struct CandidateCaptureStorage
{
  AffectedCapturePlan bound_plan;
  NativeCaptureResult result;
};

}  // namespace ista::timing_dual_csr::detail
