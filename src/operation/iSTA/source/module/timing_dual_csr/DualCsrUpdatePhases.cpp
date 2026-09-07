// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "DualCsrUpdaterInternal.hpp"
#include "DualCsrValidationInternal.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

template <typename T>
std::uint64_t sizeOf(const std::vector<T>& values) noexcept
{
  return static_cast<std::uint64_t>(values.size());
}

template <typename T>
void reserveFor(std::vector<T>& values, std::size_t required,
                DualCsrBufferKind kind, DualCsrTelemetry& telemetry)
{
  if (required <= values.capacity())
    return;
  const std::size_t old = values.capacity();
  const std::size_t doubled
      = old > std::numeric_limits<std::size_t>::max() / 2
            ? std::numeric_limits<std::size_t>::max()
            : old * 2;
  const std::size_t requested
      = std::max(required, std::max<std::size_t>(1, doubled));
  values.reserve(requested);
  if constexpr (incregpusta_debug_checks_enabled)
    telemetry.capacity_grow_events.push_back({kind, old, required, values.capacity()});
}

template <typename T>
void refreshExtent(csr::CsrBufferExtent& extent,
                   const std::vector<T>& values) noexcept
{
  extent.logical_size = values.size();
  extent.capacity = values.capacity();
}

template <typename T>
void applyReplacement(T& target,
                      const std::optional<ValueReplacement<T>>& change)
{
  if (change)
    target = change->replacement;
}

std::map<std::uint64_t, std::vector<csr::ArcId>> unpackAux(
    const AuxOutCsrState& aux)
{
  std::map<std::uint64_t, std::vector<csr::ArcId>> rows;
  for (std::size_t row = 0; row < aux.row_sources.size(); ++row) {
    const std::uint64_t begin = aux.row_offsets[row];
    const std::uint64_t end = aux.row_offsets[row + 1];
    rows.emplace(
        aux.row_sources[row].value(),
        std::vector<csr::ArcId>(
            aux.arc_ids.begin() + static_cast<std::ptrdiff_t>(begin),
            aux.arc_ids.begin() + static_cast<std::ptrdiff_t>(end)));
  }
  return rows;
}

void refreshAdjacencyExtents(DualCsrState& state) noexcept
{
  refreshExtent(state.base_out.offsets_extent, state.base_out.row_offsets);
  refreshExtent(state.base_out.edge_extent, state.base_out.physical_slots);
  refreshExtent(state.base_in.offsets_extent, state.base_in.row_offsets);
  refreshExtent(state.base_in.edge_extent, state.base_in.physical_slots);
  refreshExtent(state.aux_out.offsets_extent, state.aux_out.row_offsets);
  refreshExtent(state.aux_out.source_extent, state.aux_out.row_sources);
  refreshExtent(state.aux_out.edge_extent, state.aux_out.arc_ids);
  refreshExtent(state.aux_out.mapping_extent,
                state.aux_out.source_to_aux_row);
}

}  // namespace

void initializeUpdateTelemetry(DualCsrState& staged,
                               const DualCsrState& committed,
                               const DualCsrBatchDescriptor& descriptor)
{
  staged.telemetry = {};
  if constexpr (!incregpusta_debug_checks_enabled)
    return;
  staged.telemetry.generation_before = committed.generation.value();
  staged.telemetry.generation_after = descriptor.target_generation.value();
  staged.telemetry.logical_edges_before = activeArcCount(committed);
  staged.telemetry.nodes_appended = descriptor.node_appends.size();
  staged.telemetry.arcs_appended = descriptor.arc_appends.size();
  staged.telemetry.tombstone_reuse_count
      = std::ranges::count_if(descriptor.changes,
                             [](const BaseOutChangeDescriptor& change) {
                               return !change.expected_arc.has_value();
                             });
  staged.telemetry.base_change_count = descriptor.changes.size();
  staged.telemetry.reverse_change_count = descriptor.reverse_changes.size();
  staged.telemetry.payload_change_count
      = descriptor.payload_changes.size();
  staged.telemetry.aux_rebuild_count = descriptor.aux_rows.empty() ? 0 : 1;
}

void growDetachedCapacity(DualCsrState& staged,
                          const DualCsrBatchDescriptor& descriptor)
{
  const std::size_t appended_nodes = descriptor.node_appends.size();
  const auto source_is_new = [&](const ArcAppendRecord& arc) {
    return arc.source.value() >= descriptor.before.node_slots;
  };
  const auto sink_is_new = [&](const ArcAppendRecord& arc) {
    return arc.sink.value() >= descriptor.before.node_slots;
  };
  const std::size_t new_out_slots
      = std::ranges::count_if(descriptor.arc_appends, source_is_new);
  const std::size_t new_in_slots
      = std::ranges::count_if(descriptor.arc_appends, sink_is_new);
  std::size_t replacement_aux_edges = 0;
  for (const auto& row : descriptor.aux_rows)
    replacement_aux_edges += row.replacement_arc_ids.size();

  auto& telemetry = staged.telemetry;
  reserveFor(staged.nodes, descriptor.after.node_slots,
             DualCsrBufferKind::kNodes, telemetry);
  reserveFor(staged.arc_slots, descriptor.after.arc_slots,
             DualCsrBufferKind::kArcSlots, telemetry);
  reserveFor(staged.base_out.row_offsets,
             staged.base_out.row_offsets.size() + appended_nodes,
             DualCsrBufferKind::kBaseOutOffsets, telemetry);
  reserveFor(staged.base_out.physical_slots,
             staged.base_out.physical_slots.size() + new_out_slots,
             DualCsrBufferKind::kBaseOutSlots, telemetry);
  reserveFor(staged.base_in.row_offsets,
             staged.base_in.row_offsets.size() + appended_nodes,
             DualCsrBufferKind::kBaseInOffsets, telemetry);
  reserveFor(staged.base_in.physical_slots,
             staged.base_in.physical_slots.size() + new_in_slots,
             DualCsrBufferKind::kBaseInSlots, telemetry);
  reserveFor(staged.aux_out.row_offsets,
             staged.aux_out.row_offsets.size() + descriptor.aux_rows.size(),
             DualCsrBufferKind::kAuxOffsets, telemetry);
  reserveFor(staged.aux_out.row_sources,
             staged.aux_out.row_sources.size() + descriptor.aux_rows.size(),
             DualCsrBufferKind::kAuxSources, telemetry);
  reserveFor(staged.aux_out.arc_ids,
             staged.aux_out.arc_ids.size() + replacement_aux_edges,
             DualCsrBufferKind::kAuxArcs, telemetry);
  reserveFor(staged.aux_out.source_to_aux_row,
             descriptor.after.node_slots,
             DualCsrBufferKind::kAuxSourceMap, telemetry);
}

void applyLogicalDeactivations(
    DualCsrState& staged, const DualCsrBatchDescriptor& descriptor)
{
  for (const auto& item : descriptor.logical_deactivations)
    staged.arc_slots[item.id.value()].active = false;
}

void applyBaseChanges(DualCsrState& staged,
                      const DualCsrBatchDescriptor& descriptor)
{
  for (const auto& remove : descriptor.removes)
    staged.base_out.physical_slots[remove.physical_slot].reset();
  for (const auto& change : descriptor.changes)
    staged.base_out.physical_slots[change.physical_slot]
        = change.replacement_arc;
  for (const auto& change : descriptor.reverse_changes)
    staged.base_in.physical_slots[change.physical_slot]
        = change.replacement_arc;
}

void appendLogicalRecordsAndRows(
    DualCsrState& staged, const DualCsrBatchDescriptor& descriptor)
{
  for (const auto& append : descriptor.node_appends)
    staged.nodes.push_back(append.payload);
  for (const auto& append : descriptor.arc_appends)
    staged.arc_slots.push_back({append.payload, true});

  for (std::uint64_t node = descriptor.before.node_slots;
       node < descriptor.after.node_slots; ++node) {
    const auto row = std::ranges::find(
        descriptor.appended_base_out_rows, csr::NodeId{node},
        &AppendedBaseOutRowDescriptor::source);
    if (row != descriptor.appended_base_out_rows.end())
      for (const auto id : row->arc_ids)
        staged.base_out.physical_slots.emplace_back(id);
    staged.base_out.row_offsets.push_back(
        staged.base_out.physical_slots.size());

    const auto incoming = std::ranges::find(
        descriptor.affected_base_in_rows, csr::NodeId{node},
        &AffectedBaseInRowDescriptor::sink);
    if (incoming != descriptor.affected_base_in_rows.end())
      for (const auto id : incoming->replacement_arc_ids)
        staged.base_in.physical_slots.emplace_back(id);
    staged.base_in.row_offsets.push_back(
        staged.base_in.physical_slots.size());
  }
}

void rebuildAffectedAuxRows(
    DualCsrState& staged, const DualCsrBatchDescriptor& descriptor)
{
  auto rows = unpackAux(staged.aux_out);
  for (const auto& descriptor_row : descriptor.aux_rows) {
    if (descriptor_row.replacement_arc_ids.empty())
      rows.erase(descriptor_row.source.value());
    else
      rows[descriptor_row.source.value()]
          = descriptor_row.replacement_arc_ids;
  }

  staged.aux_out.row_offsets.assign(1, 0);
  staged.aux_out.row_sources.clear();
  staged.aux_out.arc_ids.clear();
  staged.aux_out.source_to_aux_row.assign(staged.nodes.size(), -1);
  for (const auto& [source_value, arcs] : rows) {
    const auto row = staged.aux_out.row_sources.size();
    staged.aux_out.row_sources.emplace_back(source_value);
    staged.aux_out.source_to_aux_row[source_value]
        = static_cast<std::int64_t>(row);
    staged.aux_out.arc_ids.insert(staged.aux_out.arc_ids.end(),
                                  arcs.begin(), arcs.end());
    staged.aux_out.row_offsets.push_back(staged.aux_out.arc_ids.size());
  }
}

void applyPayloadChanges(DualCsrState& staged,
                         const DualCsrBatchDescriptor& descriptor)
{
  for (const auto& change : descriptor.node_payload_changes)
    staged.nodes[change.id.value()].net_name
        = change.replacement_net_name;
  for (const auto& change : descriptor.payload_changes)
    staged.arc_slots[change.id.value()].payload = change.replacement;

  auto& target = staged.reference_payload;
  const auto& delta = descriptor.reference_payload;
  applyReplacement(target.schema_version, delta.schema_version);
  applyReplacement(target.source_identity, delta.source_identity);
  applyReplacement(target.analysis_context, delta.analysis_context);
  applyReplacement(target.analysis_order, delta.analysis_order);
  applyReplacement(target.path_source_order, delta.path_source_order);
  applyReplacement(target.transition_order, delta.transition_order);
  applyReplacement(target.instances, delta.instances);
  applyReplacement(target.ordered_root_ids, delta.ordered_root_ids);
  applyReplacement(target.ordered_start_ids, delta.ordered_start_ids);
  applyReplacement(target.ordered_end_ids, delta.ordered_end_ids);
  applyReplacement(target.clock_tags, delta.clock_tags);
  applyReplacement(target.clocks, delta.clocks);
  applyReplacement(target.clock_owner_by_node, delta.clock_owner_by_node);
  applyReplacement(target.port_constraints, delta.port_constraints);
  applyReplacement(target.case_analysis_constraints,
                   delta.case_analysis_constraints);
  applyReplacement(target.timing_check_endpoints,
                   delta.timing_check_endpoints);
  applyReplacement(target.referenced_library_cells,
                   delta.referenced_library_cells);
  applyReplacement(target.parasitics, delta.parasitics);
  applyReplacement(target.start_seeds, delta.start_seeds);
  applyReplacement(target.endpoint_required, delta.endpoint_required);
  applyReplacement(target.cell_pin_capacitances,
                   delta.cell_pin_capacitances);
  applyReplacement(target.unsupported_reasons, delta.unsupported_reasons);
}

void finalizeUpdateState(DualCsrState& staged,
                         const DualCsrBatchDescriptor& descriptor)
{
  staged.generation = descriptor.target_generation;
  staged.high_water = descriptor.after;
  if (!descriptor.reference_payload.clock_owner_by_node
      && staged.reference_payload.clock_owner_by_node.size()
             == descriptor.before.node_slots)
    staged.reference_payload.clock_owner_by_node.resize(staged.nodes.size());

  refreshAdjacencyExtents(staged);
  // The edge view consumes this count to run levelization; it is functional state.
  staged.telemetry.logical_edges_after = activeArcCount(staged);
  if constexpr (!incregpusta_debug_checks_enabled)
    return;
  staged.telemetry.base_physical_edges
      = sizeOf(staged.base_out.physical_slots);
  staged.telemetry.base_active_edges = std::ranges::count_if(
      staged.base_out.physical_slots,
      [](const std::optional<csr::ArcId>& arc) { return arc.has_value(); });
  staged.telemetry.base_tombstones
      = staged.telemetry.base_physical_edges
        - staged.telemetry.base_active_edges;
  staged.telemetry.aux_rows = sizeOf(staged.aux_out.row_sources);
  staged.telemetry.aux_edges = sizeOf(staged.aux_out.arc_ids);
}

}  // namespace ista::timing_dual_csr::detail
