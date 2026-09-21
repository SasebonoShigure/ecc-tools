// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <vector>

#include "ActiveCsrEdgeView.hpp"

namespace ista::timing_dual_csr {

enum class DualCsrBufferKind : std::uint8_t
{
  kNodes,
  kArcSlots,
  kBaseOutOffsets,
  kBaseOutSlots,
  kBaseInOffsets,
  kBaseInSlots,
  kAuxRows,
  kAuxSources,
  kAuxArcs,
  kAuxSourceMap
};

struct DualCsrCapacityGrowEvent
{
  DualCsrBufferKind buffer{DualCsrBufferKind::kNodes};
  std::uint64_t old_capacity{0};
  std::uint64_t required{0};
  std::uint64_t new_capacity{0};

  friend bool operator==(const DualCsrCapacityGrowEvent&,
                         const DualCsrCapacityGrowEvent&) = default;
};

struct DualCsrTelemetry
{
  std::uint64_t generation_before{0};
  std::uint64_t generation_after{0};
  std::uint64_t logical_edges_before{0};
  std::uint64_t logical_edges_after{0};
  std::uint64_t base_physical_edges{0};
  std::uint64_t base_active_edges{0};
  std::uint64_t base_tombstones{0};
  std::uint64_t aux_rows{0};
  std::uint64_t aux_edges{0};
  std::uint64_t tombstone_reuse_count{0};
  std::uint64_t base_change_count{0};
  std::uint64_t reverse_change_count{0};
  std::uint64_t nodes_appended{0};
  std::uint64_t arcs_appended{0};
  std::uint64_t payload_change_count{0};
  std::uint64_t aux_rebuild_count{0};
  std::uint64_t full_rebuild_count{0};
  std::uint64_t fallback_count{0};
  std::vector<DualCsrCapacityGrowEvent> capacity_grow_events;
  csr::CsrTraversalTelemetry traversal;

  friend bool operator==(const DualCsrTelemetry&, const DualCsrTelemetry&) = default;
};

}  // namespace ista::timing_dual_csr
