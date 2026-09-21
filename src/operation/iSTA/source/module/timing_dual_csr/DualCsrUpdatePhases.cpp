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
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "DualCsrUpdaterInternal.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

template <typename T>
void reserveFor(std::vector<T>& values, std::size_t required,
                DualCsrBufferKind kind, DualCsrTelemetry& telemetry)
{
  if (required <= values.capacity())
    return;
  if (required > values.max_size())
    throw std::length_error("Dual-CSR capacity exceeds vector maximum");
  const auto old = values.capacity();
  const auto grown = old > values.max_size() / 2 ? values.max_size()
                                                : std::max<std::size_t>(1, old * 2);
  values.reserve(std::max(required, grown));
  if constexpr (incregpusta_debug_checks_enabled)
    telemetry.capacity_grow_events.push_back({kind, old, required, values.capacity()});
}

template <typename T>
void reserveAppend(std::vector<T>& values, std::size_t count)
{
  if (count > values.max_size() - values.size())
    throw std::length_error("Dual-CSR reference append exceeds vector maximum");
  const auto required = values.size() + count;
  if (required > values.capacity()) {
    const auto old = values.capacity();
    values.reserve(std::max(required, old > values.max_size() / 2
                                         ? values.max_size() : std::max<std::size_t>(1, old * 2)));
  }
}

template <typename T>
void appendReserved(std::vector<T>& target, std::vector<T>& values) noexcept
{
  static_assert(std::is_nothrow_move_constructible_v<T>);
  for (auto& value : values)
    target.push_back(std::move(value));
}

template <typename T>
void refreshExtent(csr::CsrBufferExtent& extent, const std::vector<T>& values) noexcept
{
  extent = {values.size(), values.capacity()};
}

void refreshExtents(DualCsrState& state) noexcept
{
  refreshExtent(state.base_out.offsets_extent, state.base_out.row_offsets);
  refreshExtent(state.base_out.edge_extent, state.base_out.physical_slots);
  refreshExtent(state.base_in.offsets_extent, state.base_in.row_offsets);
  refreshExtent(state.base_in.edge_extent, state.base_in.physical_slots);
  refreshExtent(state.aux_out.rows_extent, state.aux_out.row_arcs);
  refreshExtent(state.aux_out.source_extent, state.aux_out.row_sources);
  refreshExtent(state.aux_out.mapping_extent, state.aux_out.source_to_aux_row);
}

template <typename T>
void swapReplacement(T& target, std::optional<T>& value) noexcept
{
  static_assert(std::is_nothrow_swappable_v<T>);
  if (value) {
    using std::swap;
    swap(target, *value);
  }
}

template <typename T>
void swapSlots(std::vector<T>& target, std::vector<std::pair<std::size_t, T>>& values) noexcept
{
  static_assert(std::is_nothrow_swappable_v<T>);
  for (auto& [slot, value] : values) {
    using std::swap;
    swap(target[slot], value);
  }
}

}  // namespace

struct PreparedDualCsrGraphPatch::Data
{
  DualCsrState* state;
  timing_edit::TransactionGeneration before_generation;
  timing_edit::TransactionGeneration after_generation;
  timing_edit::AppendOnlyHighWater before;
  timing_edit::AppendOnlyHighWater after;
  std::size_t old_out_slots;
  std::size_t old_in_slots;
  std::size_t old_aux_rows;
  DualCsrTelemetry telemetry;
  std::vector<csr::NodeRecord> nodes;
  std::vector<DualCsrArcSlot> arcs;
  std::vector<std::pair<std::size_t, std::optional<csr::ArcId>>> out_slots, in_slots;
  std::vector<std::optional<csr::ArcId>> appended_out, appended_in;
  std::vector<std::uint64_t> out_offsets, in_offsets;
  std::vector<std::pair<std::size_t, std::string>> node_nets;
  std::vector<std::pair<std::size_t, csr::ArcSemanticPayload>> payloads;
  std::vector<std::pair<std::size_t, bool>> activity;
  struct AuxRow
  {
    csr::NodeId source;
    std::size_t row;
    bool appended;
    std::vector<csr::ArcId> before;
    std::vector<csr::ArcId> after;
  };
  std::vector<AuxRow> aux_rows;
  std::optional<decltype(DualCsrReferencePayload::schema_version)> schema_version;
  std::optional<decltype(DualCsrReferencePayload::source_identity)> source_identity;
  std::optional<decltype(DualCsrReferencePayload::analysis_context)> analysis_context;
  std::optional<decltype(DualCsrReferencePayload::analysis_order)> analysis_order;
  std::optional<decltype(DualCsrReferencePayload::path_source_order)> path_source_order;
  std::optional<decltype(DualCsrReferencePayload::transition_order)> transition_order;
  std::optional<decltype(DualCsrReferencePayload::ordered_root_ids)> ordered_root_ids;
  std::optional<decltype(DualCsrReferencePayload::ordered_start_ids)> ordered_start_ids;
  std::optional<decltype(DualCsrReferencePayload::ordered_end_ids)> ordered_end_ids;
  std::optional<decltype(DualCsrReferencePayload::clock_tags)> clock_tags;
  std::optional<decltype(DualCsrReferencePayload::clocks)> clocks;
  std::optional<decltype(DualCsrReferencePayload::port_constraints)> port_constraints;
  std::optional<decltype(DualCsrReferencePayload::case_analysis_constraints)> case_analysis_constraints;
  std::optional<decltype(DualCsrReferencePayload::timing_check_endpoints)> timing_check_endpoints;
  std::optional<decltype(DualCsrReferencePayload::parasitics)> parasitics;
  std::optional<decltype(DualCsrReferencePayload::start_seeds)> start_seeds;
  std::optional<decltype(DualCsrReferencePayload::endpoint_required)> endpoint_required;
  std::optional<decltype(DualCsrReferencePayload::unsupported_reasons)> unsupported_reasons;
  decltype(DualCsrReferencePayload::instances) instances;
  std::size_t old_instances;
  decltype(DualCsrReferencePayload::referenced_library_cells) referenced_library_cells;
  std::size_t old_referenced_library_cells;
  decltype(DualCsrReferencePayload::cell_pin_capacitances) cell_pin_capacitances;
  std::size_t old_cell_pin_capacitances;
  decltype(DualCsrReferencePayload::clock_owner_by_node) clock_owner_by_node;
  std::size_t old_clock_owner_by_node;
  enum class Phase { kPrepared, kApplied, kFinished };
  Phase phase{Phase::kPrepared};

  Data(DualCsrState& graph, const DualCsrBatchDescriptor& descriptor)
      : state(&graph), before_generation(graph.generation), after_generation(descriptor.target_generation),
        before(graph.high_water), after(descriptor.after),
        old_out_slots(graph.base_out.physical_slots.size()), old_in_slots(graph.base_in.physical_slots.size()),
        old_aux_rows(graph.aux_out.row_arcs.size())
  {
    const auto active = graph.telemetry.logical_edges_after;
    if (descriptor.logical_deactivations.size() > active
        || descriptor.arc_appends.size() > std::numeric_limits<std::uint64_t>::max() - active)
      throw std::invalid_argument("Dual-CSR active edge count would overflow or underflow");
    telemetry.logical_edges_after = active - descriptor.logical_deactivations.size() + descriptor.arc_appends.size();
    if constexpr (incregpusta_debug_checks_enabled) {
      telemetry.generation_before = graph.generation.value();
      telemetry.generation_after = descriptor.target_generation.value();
      telemetry.logical_edges_before = active;
      telemetry.nodes_appended = descriptor.node_appends.size();
      telemetry.arcs_appended = descriptor.arc_appends.size();
      telemetry.tombstone_reuse_count = std::ranges::count_if(descriptor.changes, [](const auto& value) { return !value.expected_arc; });
      telemetry.base_change_count = descriptor.changes.size();
      telemetry.reverse_change_count = descriptor.reverse_changes.size();
      telemetry.payload_change_count = descriptor.payload_changes.size();
      telemetry.aux_rebuild_count = descriptor.aux_rows.empty() ? 0 : 1;
    }
    for (const auto& item : descriptor.node_appends)
      nodes.push_back(item.payload);
    for (const auto& item : descriptor.arc_appends)
      arcs.push_back({item.payload, true});
    for (const auto& item : descriptor.removes)
      out_slots.emplace_back(item.physical_slot, std::nullopt);
    for (const auto& item : descriptor.changes)
      out_slots.emplace_back(item.physical_slot, item.replacement_arc);
    for (const auto& item : descriptor.reverse_changes)
      in_slots.emplace_back(item.physical_slot, item.replacement_arc);
    for (const auto& item : descriptor.node_payload_changes)
      node_nets.emplace_back(item.id.value(), item.replacement_net_name);
    for (const auto& item : descriptor.payload_changes)
      payloads.emplace_back(item.id.value(), item.replacement);
    for (const auto& item : descriptor.logical_deactivations)
      activity.emplace_back(item.id.value(), false);
    for (std::uint64_t node = before.node_slots; node < after.node_slots; ++node) {
      const auto out = std::ranges::find(descriptor.appended_base_out_rows, csr::NodeId{node}, &AppendedBaseOutRowDescriptor::source);
      if (out != descriptor.appended_base_out_rows.end())
        for (const auto id : out->arc_ids)
          appended_out.emplace_back(id);
      out_offsets.push_back(old_out_slots + appended_out.size());
      const auto in = std::ranges::find(descriptor.affected_base_in_rows, csr::NodeId{node}, &AffectedBaseInRowDescriptor::sink);
      if (in != descriptor.affected_base_in_rows.end())
        for (const auto id : in->replacement_arc_ids)
          appended_in.emplace_back(id);
      in_offsets.push_back(old_in_slots + appended_in.size());
    }
    std::size_t appended_rows = 0;
    for (const auto& item : descriptor.aux_rows) {
      const auto mapped = item.source.value() < graph.aux_out.source_to_aux_row.size()
                              ? graph.aux_out.source_to_aux_row[item.source.value()] : -1;
      if (mapped < 0) {
        if (!item.replacement_arc_ids.empty())
          aux_rows.push_back({item.source, old_aux_rows + appended_rows++, true, {}, item.replacement_arc_ids});
      } else {
        const auto row = static_cast<std::size_t>(mapped);
        aux_rows.push_back({item.source, row, false, graph.aux_out.row_arcs.at(row), item.replacement_arc_ids});
      }
    }
    const auto& delta = descriptor.reference_payload;
    if (delta.schema_version)
      schema_version = delta.schema_version->replacement;
    if (delta.source_identity)
      source_identity = delta.source_identity->replacement;
    if (delta.analysis_context)
      analysis_context = delta.analysis_context->replacement;
    if (delta.analysis_order)
      analysis_order = delta.analysis_order->replacement;
    if (delta.path_source_order)
      path_source_order = delta.path_source_order->replacement;
    if (delta.transition_order)
      transition_order = delta.transition_order->replacement;
    if (delta.ordered_root_ids)
      ordered_root_ids = delta.ordered_root_ids->replacement;
    if (delta.ordered_start_ids)
      ordered_start_ids = delta.ordered_start_ids->replacement;
    if (delta.ordered_end_ids)
      ordered_end_ids = delta.ordered_end_ids->replacement;
    if (delta.clock_tags)
      clock_tags = delta.clock_tags->replacement;
    if (delta.clocks)
      clocks = delta.clocks->replacement;
    if (delta.port_constraints)
      port_constraints = delta.port_constraints->replacement;
    if (delta.case_analysis_constraints)
      case_analysis_constraints = delta.case_analysis_constraints->replacement;
    if (delta.timing_check_endpoints)
      timing_check_endpoints = delta.timing_check_endpoints->replacement;
    if (delta.parasitics)
      parasitics = delta.parasitics->replacement;
    if (delta.start_seeds)
      start_seeds = delta.start_seeds->replacement;
    if (delta.endpoint_required)
      endpoint_required = delta.endpoint_required->replacement;
    if (delta.unsupported_reasons)
      unsupported_reasons = delta.unsupported_reasons->replacement;
    old_instances = graph.reference_payload.instances.size();
    if (delta.instances)
      instances = delta.instances->values;
    old_referenced_library_cells = graph.reference_payload.referenced_library_cells.size();
    if (delta.referenced_library_cells)
      referenced_library_cells = delta.referenced_library_cells->values;
    old_cell_pin_capacitances = graph.reference_payload.cell_pin_capacitances.size();
    if (delta.cell_pin_capacitances)
      cell_pin_capacitances = delta.cell_pin_capacitances->values;
    old_clock_owner_by_node = graph.reference_payload.clock_owner_by_node.size();
    if (delta.clock_owner_by_node)
      clock_owner_by_node = delta.clock_owner_by_node->values;
    if (!delta.clock_owner_by_node && old_clock_owner_by_node == before.node_slots)
      clock_owner_by_node.resize(after.node_slots - before.node_slots);

    // Logical state is untouched if any reserve fails. Capacity changes are
    // retained, and declared extents must describe that real capacity.
    try {
      reserveFor(graph.nodes, after.node_slots, DualCsrBufferKind::kNodes, telemetry);
      reserveFor(graph.arc_slots, after.arc_slots, DualCsrBufferKind::kArcSlots, telemetry);
      reserveFor(graph.base_out.row_offsets, graph.base_out.row_offsets.size() + out_offsets.size(), DualCsrBufferKind::kBaseOutOffsets, telemetry);
      reserveFor(graph.base_in.row_offsets, graph.base_in.row_offsets.size() + in_offsets.size(), DualCsrBufferKind::kBaseInOffsets, telemetry);
      reserveFor(graph.base_out.physical_slots, old_out_slots + appended_out.size(), DualCsrBufferKind::kBaseOutSlots, telemetry);
      reserveFor(graph.base_in.physical_slots, old_in_slots + appended_in.size(), DualCsrBufferKind::kBaseInSlots, telemetry);
      reserveFor(graph.aux_out.row_arcs, old_aux_rows + appended_rows, DualCsrBufferKind::kAuxRows, telemetry);
      reserveFor(graph.aux_out.row_sources, old_aux_rows + appended_rows, DualCsrBufferKind::kAuxSources, telemetry);
      reserveFor(graph.aux_out.source_to_aux_row, after.node_slots, DualCsrBufferKind::kAuxSourceMap, telemetry);
      for (auto& row : aux_rows)
        if (!row.appended)
          reserveFor(graph.aux_out.row_arcs[row.row], row.after.size(), DualCsrBufferKind::kAuxArcs, telemetry);
      reserveAppend(graph.reference_payload.instances, instances.size());
      reserveAppend(graph.reference_payload.referenced_library_cells, referenced_library_cells.size());
      reserveAppend(graph.reference_payload.cell_pin_capacitances, cell_pin_capacitances.size());
      reserveAppend(graph.reference_payload.clock_owner_by_node, clock_owner_by_node.size());
    } catch (...) {
      refreshExtents(graph);
      throw;
    }
    refreshExtents(graph);
  }

  void swapPayloads() noexcept
  {
    for (auto& [id, value] : node_nets)
      state->nodes[id].net_name.swap(value);
    for (auto& [id, value] : payloads) {
      using std::swap;
      swap(state->arc_slots[id].payload, value);
    }
    swapReplacement(state->reference_payload.schema_version, schema_version);
    swapReplacement(state->reference_payload.source_identity, source_identity);
    swapReplacement(state->reference_payload.analysis_context, analysis_context);
    swapReplacement(state->reference_payload.analysis_order, analysis_order);
    swapReplacement(state->reference_payload.path_source_order, path_source_order);
    swapReplacement(state->reference_payload.transition_order, transition_order);
    swapReplacement(state->reference_payload.ordered_root_ids, ordered_root_ids);
    swapReplacement(state->reference_payload.ordered_start_ids, ordered_start_ids);
    swapReplacement(state->reference_payload.ordered_end_ids, ordered_end_ids);
    swapReplacement(state->reference_payload.clock_tags, clock_tags);
    swapReplacement(state->reference_payload.clocks, clocks);
    swapReplacement(state->reference_payload.port_constraints, port_constraints);
    swapReplacement(state->reference_payload.case_analysis_constraints, case_analysis_constraints);
    swapReplacement(state->reference_payload.timing_check_endpoints, timing_check_endpoints);
    swapReplacement(state->reference_payload.parasitics, parasitics);
    swapReplacement(state->reference_payload.start_seeds, start_seeds);
    swapReplacement(state->reference_payload.endpoint_required, endpoint_required);
    swapReplacement(state->reference_payload.unsupported_reasons, unsupported_reasons);
  }

  void apply() noexcept
  {
    if (phase != Phase::kPrepared)
      return;
    auto& graph = *state;
    appendReserved(graph.nodes, nodes);
    appendReserved(graph.arc_slots, arcs);
    appendReserved(graph.base_out.physical_slots, appended_out);
    appendReserved(graph.base_in.physical_slots, appended_in);
    appendReserved(graph.base_out.row_offsets, out_offsets);
    appendReserved(graph.base_in.row_offsets, in_offsets);
    swapSlots(graph.base_out.physical_slots, out_slots);
    swapSlots(graph.base_in.physical_slots, in_slots);
    for (auto& [id, value] : activity)
      std::swap(graph.arc_slots[id].active, value);
    graph.aux_out.source_to_aux_row.resize(after.node_slots, -1);
    for (auto& row : aux_rows) {
      if (row.appended) {
        graph.aux_out.row_sources.push_back(row.source);
        graph.aux_out.row_arcs.push_back(std::move(row.after));
        graph.aux_out.source_to_aux_row[row.source.value()] = static_cast<std::int64_t>(row.row);
      } else {
        auto& target = graph.aux_out.row_arcs[row.row];
        target.assign(row.after.begin(), row.after.end());
      }
    }
    swapPayloads();
    appendReserved(graph.reference_payload.instances, instances);
    appendReserved(graph.reference_payload.referenced_library_cells, referenced_library_cells);
    appendReserved(graph.reference_payload.cell_pin_capacitances, cell_pin_capacitances);
    appendReserved(graph.reference_payload.clock_owner_by_node, clock_owner_by_node);
    graph.generation = after_generation;
    graph.high_water = after;
    if constexpr (incregpusta_debug_checks_enabled) {
      telemetry.base_physical_edges = graph.base_out.physical_slots.size();
      telemetry.base_active_edges = std::ranges::count_if(graph.base_out.physical_slots, [](const auto& arc) { return arc.has_value(); });
      telemetry.base_tombstones = telemetry.base_physical_edges - telemetry.base_active_edges;
      telemetry.aux_rows = graph.aux_out.row_arcs.size();
      for (const auto& row : graph.aux_out.row_arcs)
        telemetry.aux_edges += row.size();
    }
    using std::swap;
    swap(graph.telemetry, telemetry);
    refreshExtents(graph);
    phase = Phase::kApplied;
  }

  void rollback() noexcept
  {
    if (phase != Phase::kApplied)
      return;
    auto& graph = *state;
    swapPayloads();
    swapSlots(graph.base_out.physical_slots, out_slots);
    swapSlots(graph.base_in.physical_slots, in_slots);
    for (auto& [id, value] : activity)
      std::swap(graph.arc_slots[id].active, value);
    for (const auto& row : aux_rows) {
      if (row.appended) {
        graph.aux_out.source_to_aux_row[row.source.value()] = -1;
      } else {
        auto& target = graph.aux_out.row_arcs[row.row];
        target.assign(row.before.begin(), row.before.end());
      }
    }
    graph.aux_out.row_arcs.resize(old_aux_rows);
    graph.aux_out.row_sources.resize(old_aux_rows);
    graph.aux_out.source_to_aux_row.resize(before.node_slots);
    graph.base_out.physical_slots.resize(old_out_slots);
    graph.base_in.physical_slots.resize(old_in_slots);
    graph.base_out.row_offsets.resize(before.node_slots + 1);
    graph.base_in.row_offsets.resize(before.node_slots + 1);
    graph.nodes.resize(before.node_slots);
    graph.arc_slots.resize(before.arc_slots);
    graph.reference_payload.instances.resize(old_instances);
    graph.reference_payload.referenced_library_cells.resize(old_referenced_library_cells);
    graph.reference_payload.cell_pin_capacitances.resize(old_cell_pin_capacitances);
    graph.reference_payload.clock_owner_by_node.resize(old_clock_owner_by_node);
    graph.generation = before_generation;
    graph.high_water = before;
    using std::swap;
    swap(graph.telemetry, telemetry);
    refreshExtents(graph);
    phase = Phase::kFinished;
  }
};

PreparedDualCsrGraphPatch::PreparedDualCsrGraphPatch(DualCsrState& state, const DualCsrBatchDescriptor& descriptor)
    : _data(std::make_unique<Data>(state, descriptor))
{
}
PreparedDualCsrGraphPatch::PreparedDualCsrGraphPatch(PreparedDualCsrGraphPatch&&) noexcept = default;
PreparedDualCsrGraphPatch::~PreparedDualCsrGraphPatch() { rollback(); }
void PreparedDualCsrGraphPatch::apply() noexcept { _data->apply(); }
void PreparedDualCsrGraphPatch::rollback() noexcept { if (_data) _data->rollback(); }
void PreparedDualCsrGraphPatch::commit() noexcept { if (_data) _data->phase = Data::Phase::kFinished; }

}  // namespace ista::timing_dual_csr::detail
