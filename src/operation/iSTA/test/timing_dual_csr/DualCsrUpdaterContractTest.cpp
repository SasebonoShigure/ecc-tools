// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrUpdaterInternal.hpp"
#include "AllocationFailureSupport.hpp"
#include "DualCsrResourceObservation.hpp"
#include "DualCsrTestSupport.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::timing_dual_csr::test {
namespace {

DualCsrState tombstoneBaselineState()
{
  auto input = baselineInput();
  auto& isolated_after_removal = input.nodes[0];
  isolated_after_removal.direction = csr::PinDirection::kInout;
  isolated_after_removal.is_endpoint = true;
  input.ordered_end_ids = {csr::NodeId{0}, csr::NodeId{1}};

  csr::EndpointRequiredModel endpoint;
  endpoint.node = csr::NodeId{0};
  endpoint.clock_name = "clk";
  input.endpoint_required.insert(input.endpoint_required.begin(),
                                 std::move(endpoint));
  csr::test::refreshIdentity(input, kSessionValue);
  auto attempt = DualCsrBaselineFactory::create(
      input, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{input.nodes.size(), 0,
                                       input.arcs.size()});
  require(attempt.success(), "Dual tombstone baseline rejected fixture");
  return std::move(*attempt.state);
}

DualCsrBatchDescriptor tombstoneBatch(const DualCsrState& state)
{
  auto descriptor = descriptorFor(state, 1, 1);
  const auto appended_node = appendNode(
      2, "replacement_source", AppendedPortRole::kIsolated);
  descriptor.node_appends = {appended_node};
  descriptor.arc_appends = {appendArc(
      1, csr::NodeId{2}, csr::NodeId{1}, appended_node.key,
      state.nodes[1].key, 0)};
  descriptor.removes = {{csr::NodeId{0}, 0, csr::ArcId{0}}};
  descriptor.reverse_changes = {
      {csr::NodeId{1}, 0, csr::ArcId{0}, csr::ArcId{1}}};
  descriptor.logical_deactivations = {
      deactivate(state, csr::ArcId{0},
                 LogicalArcDeactivateReason::kRemoved)};
  replaceRoots(descriptor, state,
               {csr::NodeId{0}, csr::NodeId{2}});
  completeReferencePayload(descriptor, state);
  return descriptor;
}

DualCsrBatchDescriptor reuseBatch(const DualCsrState& state)
{
  auto descriptor = descriptorFor(state, 0, 1);
  descriptor.arc_appends = {appendArc(
      2, csr::NodeId{0}, csr::NodeId{1}, state.nodes[0].key,
      state.nodes[1].key, 0)};
  descriptor.changes = {{csr::NodeId{0}, 0, std::nullopt,
                         csr::ArcId{2}}};
  descriptor.removes = {{
      csr::NodeId{2}, state.base_out.row_offsets[2], csr::ArcId{1}}};
  descriptor.reverse_changes = {{
      csr::NodeId{1}, state.base_in.row_offsets[1], csr::ArcId{1},
      csr::ArcId{2}}};
  descriptor.logical_deactivations = {
      deactivate(state, csr::ArcId{1},
                 LogicalArcDeactivateReason::kReplaced)};
  completeReferencePayload(descriptor, state);
  return descriptor;
}

void referenceAppendContracts()
{
  const auto baseline = tombstoneBaselineState();
  const auto check = [&](DualCsrReferencePayloadDelta delta) {
    auto descriptor = tombstoneBatch(baseline);
    descriptor.reference_payload = std::move(delta);
    require(!validateDualCsrDescriptor(baseline, descriptor).empty(), "malformed reference append was accepted");
  };
  auto delta = tombstoneBatch(baseline).reference_payload;
  delta.clock_owner_by_node = RecordAppend<std::optional<std::string>>{baseline.nodes.size() + 1, {std::nullopt}};
  check(delta);
  delta = tombstoneBatch(baseline).reference_payload;
  delta.clock_owner_by_node = RecordAppend<std::optional<std::string>>{baseline.nodes.size(), {"missing-clock"}};
  check(delta);
  delta = tombstoneBatch(baseline).reference_payload;
  csr::CellPinCapacitanceModel cap;
  cap.node = csr::NodeId{std::numeric_limits<std::uint64_t>::max()};
  delta.cell_pin_capacitances = RecordAppend<csr::CellPinCapacitanceModel>{0, {cap}};
  check(delta);
  cap.node = csr::NodeId{2};
  cap.default_capacitance = std::numeric_limits<double>::quiet_NaN();
  delta.cell_pin_capacitances = RecordAppend<csr::CellPinCapacitanceModel>{0, {cap}};
  check(delta);
  delta = tombstoneBatch(baseline).reference_payload;
  csr::ReferencedLibraryCellModel cell;
  cell.cell_name = "new-model";
  cell.present = true;
  delta.referenced_library_cells = RecordAppend<csr::ReferencedLibraryCellModel>{0, {cell, cell}};
  check(delta);
}

void tombstoneAndReuseContracts()
{
  const DualCsrState baseline = tombstoneBaselineState();
  const auto baseline_out_offsets = baseline.base_out.row_offsets;
  const auto baseline_in_offsets = baseline.base_in.row_offsets;
  const auto first_descriptor = tombstoneBatch(baseline);
  const DualCsrState first = apply(baseline, first_descriptor);

  requireOffsetPrefix(baseline_out_offsets, first.base_out.row_offsets,
                      "BaseOut offsets after removal");
  requireOffsetPrefix(baseline_in_offsets, first.base_in.row_offsets,
                      "BaseIn offsets after reverse replacement");
  require(!first.base_out.physical_slots[0]
              && first.base_out.physical_slots[1]
                     == std::optional<csr::ArcId>{csr::ArcId{1}},
          "remove did not leave a stable BaseOut tombstone");
  require(!first.arc_slots[0].active && first.arc_slots[1].active,
          "typed logical deactivation did not preserve stable ArcId slots");
  require(first.base_in.physical_slots[0]
              == std::optional<csr::ArcId>{csr::ArcId{1}},
          "reverse change did not update BaseIn");
  if constexpr (incregpusta_debug_checks_enabled) {
    require(first.telemetry.base_physical_edges == first.base_out.physical_slots.size() && first.telemetry.base_active_edges == 1
                && first.telemetry.base_tombstones == 1,
            "batch telemetry changed outgoing-Base semantics");
    require(!first.telemetry.capacity_grow_events.empty(), "detached append did not report any capacity growth");
  }

  const auto first_out_offsets = first.base_out.row_offsets;
  const auto first_in_offsets = first.base_in.row_offsets;
  const auto second_descriptor = reuseBatch(first);
  const DualCsrState second = apply(first, second_descriptor);
  requireOffsetPrefix(first_out_offsets, second.base_out.row_offsets,
                      "BaseOut offsets after tombstone reuse");
  requireOffsetPrefix(first_in_offsets, second.base_in.row_offsets,
                      "BaseIn offsets after new sink append");
  require(second.base_out.physical_slots.size()
              == first.base_out.physical_slots.size()
              && second.base_out.physical_slots[0]
                     == std::optional<csr::ArcId>{csr::ArcId{2}},
          "old-row tombstone was not reused in place");
  if constexpr (incregpusta_debug_checks_enabled) {
    require(second.telemetry.tombstone_reuse_count == 1 && second.telemetry.base_tombstones == 1, "tombstone reuse telemetry is incorrect");
  }
  require(!second.arc_slots[0].active
              && second.arc_slots[0].payload.key
                     == baseline.arc_slots[0].payload.key,
          "inactive stable ArcId/key was overwritten during reuse");
  require(validateDualCsrState(second).empty(),
          "reuse batch emitted an invalid state");
}

void resourceObservationContracts()
{
  require(dualCsrTombstoneRatio(0, 0) == 0.0,
          "zero physical storage did not report a zero tombstone ratio");
  require(dualCsrTombstoneRatio(4, 1) == 0.25,
          "non-empty tombstone ratio is not physical-slot based");
  require(!detail::checkedShallowVectorStorageBytes(
               std::numeric_limits<std::uint64_t>::max(), 2)
               .has_value(),
          "shallow vector byte estimate accepted uint64_t overflow");
  require(detail::checkedShallowVectorStorageBytes(0, 8)
                  == std::optional<std::uint64_t>{0}
              && detail::checkedShallowVectorStorageBytes(8, 0)
                     == std::nullopt,
          "shallow vector byte estimate changed zero-input semantics");

  const DualCsrState baseline = tombstoneBaselineState();
  const DualCsrState state = apply(baseline, tombstoneBatch(baseline));
  auto attempt = observeDualCsrResources(state);
  require(attempt.success(),
          "valid committed state did not produce a resource observation");
  const auto& observation = *attempt.observation;
  require(observation.generation == state.generation
              && observation.tombstone_ratio
                     == dualCsrTombstoneRatio(
                         state.telemetry.base_physical_edges,
                         state.telemetry.base_tombstones)
              && observation.requested_strategy
                     == kDualCsrRequestedStrategy
              && observation.executed_strategy
                     == kDualCsrExecutedStrategy
              && observation.vector_memory.estimate_kind
                     == DualCsrMemoryEstimateKind::kShallowVectorStorage,
          "resource observation metadata differs from committed execution");

  const auto sizeAndCapacity = [](const auto& values) {
    return std::pair{static_cast<std::uint64_t>(values.size()),
                     static_cast<std::uint64_t>(values.capacity())};
  };
  const std::array expected_extents{
      sizeAndCapacity(state.nodes),
      sizeAndCapacity(state.arc_slots),
      sizeAndCapacity(state.base_out.row_offsets),
      sizeAndCapacity(state.base_out.physical_slots),
      sizeAndCapacity(state.base_in.row_offsets),
      sizeAndCapacity(state.base_in.physical_slots),
      sizeAndCapacity(state.aux_out.row_arcs),
      sizeAndCapacity(state.aux_out.row_sources),
      [&] {
        std::pair<std::uint64_t, std::uint64_t> total{};
        for (const auto& row : state.aux_out.row_arcs) {
          total.first += row.size();
          total.second += row.capacity();
        }
        return total;
      }(),
      sizeAndCapacity(state.aux_out.source_to_aux_row)};
  const std::array<std::uint64_t, kDualCsrObservedVectorCount>
      element_sizes{
          sizeof(csr::NodeRecord), sizeof(DualCsrArcSlot),
          sizeof(std::uint64_t), sizeof(std::optional<csr::ArcId>),
          sizeof(std::uint64_t), sizeof(std::optional<csr::ArcId>),
          sizeof(std::vector<csr::ArcId>), sizeof(csr::NodeId), sizeof(csr::ArcId),
          sizeof(std::int64_t)};
  for (std::size_t index = 0;
       index < kDualCsrObservedBufferOrder.size(); ++index) {
    const auto& item = observation.vector_memory.buffers[index];
    require(item.buffer == kDualCsrObservedBufferOrder[index]
                && item.logical_elements == expected_extents[index].first
                && item.capacity_elements == expected_extents[index].second
                && item.element_size_bytes == element_sizes[index]
                && item.estimated_reserved_bytes
                       == item.capacity_elements * item.element_size_bytes,
            "resource observation omitted or misreported a state vector");
  }
  require(validateDualCsrResourceObservation(state, observation).empty(),
          "exact resource observation failed validation");
  auto corrupted = observation;
  ++corrupted.vector_memory.buffers[0].estimated_reserved_bytes;
  require(hasCode(validateDualCsrResourceObservation(state, corrupted),
                  DualCsrViolationCode::kExpectedValue),
          "resource observation validator accepted a byte-estimate tamper");
}

void payloadAndDescriptorContracts()
{
  const DualCsrState baseline = tombstoneBaselineState();
  const DualCsrState first = apply(baseline, tombstoneBatch(baseline));
  const DualCsrState second = apply(first, reuseBatch(first));

  auto invalid_reason = tombstoneBatch(baseline);
  invalid_reason.logical_deactivations[0].reason
      = static_cast<LogicalArcDeactivateReason>(255);
  const auto invalid_reason_attempt
      = validateDualCsrDescriptor(baseline, invalid_reason);
  require(hasCode(invalid_reason_attempt,
                         DualCsrViolationCode::kSchema),
          "descriptor accepted an out-of-domain deactivation reason");

  auto append_isolated = descriptorFor(second, 1, 0);
  const auto isolated = appendNode(
      3, "isolated", AppendedPortRole::kIsolated);
  append_isolated.node_appends = {isolated};
  completeReferencePayload(append_isolated, second);
  const DualCsrState third = apply(second, append_isolated);

  auto descriptor = descriptorFor(third, 0, 0);
  const auto& current_node = third.nodes[3];
  descriptor.node_payload_changes = {{
      csr::NodeId{3}, current_node.key, current_node.net_name,
      "replacement_net"}};
  auto replacement = third.arc_slots[2].payload;
  replacement.output_load = csr::test::outputLoad(4.0);
  descriptor.payload_changes = {{csr::ArcId{2},
                                 third.arc_slots[2].payload,
                                 replacement}};
  auto constraints = third.reference_payload.port_constraints;
  const auto output_constraint = std::ranges::find_if(
      constraints, [](const csr::PortConstraintModel& constraint) {
        return constraint.map_key == "out";
      });
  require(output_constraint != constraints.end(),
          "fixture lost the output port constraint");
  output_constraint->output_load = 4.0;
  replaceReference(descriptor.reference_payload.port_constraints,
                   third.reference_payload.port_constraints,
                   std::move(constraints));
  const DualCsrState changed = apply(third, descriptor);
  require(changed.nodes[3].net_name == "replacement_net" && (*changed.arc_slots[2].payload.output_load)[0][0] == 4.0
              && (!incregpusta_debug_checks_enabled || changed.telemetry.payload_change_count == 1),
          "node/arc payload changes were not applied");

  auto missing_identity = descriptorFor(second, 0, 0);
  missing_identity.reference_payload.source_identity.reset();
  const auto missing_attempt
      = validateDualCsrDescriptor(second, missing_identity);
  require(hasCode(missing_attempt,
                         DualCsrViolationCode::kGeneration),
          "descriptor accepted an undeclared generation identity change");

  auto conflict = descriptorFor(second, 0, 0);
  conflict.payload_changes = {{csr::ArcId{2},
                               second.arc_slots[2].payload,
                               replacement}};
  conflict.logical_deactivations = {
      deactivate(second, csr::ArcId{2},
                 LogicalArcDeactivateReason::kReplaced)};
  const auto conflict_attempt = validateDualCsrDescriptor(second, conflict);
  require(hasCode(conflict_attempt,
                         DualCsrViolationCode::kConflict),
          "descriptor accepted payload-change/deactivate overlap");
}

void localPatchAtomicity()
{
  auto state = tombstoneBaselineState();
  const auto before = state;
  const auto descriptor = tombstoneBatch(state);
  const auto restored = [&] {
    require(state.generation == before.generation && state.high_water == before.high_water
                && state.nodes == before.nodes && state.arc_slots == before.arc_slots
                && state.base_out.row_offsets == before.base_out.row_offsets
                && state.base_out.physical_slots == before.base_out.physical_slots
                && state.base_in.row_offsets == before.base_in.row_offsets
                && state.base_in.physical_slots == before.base_in.physical_slots
                && state.aux_out.row_arcs == before.aux_out.row_arcs
                && state.aux_out.row_sources == before.aux_out.row_sources
                && state.aux_out.source_to_aux_row == before.aux_out.source_to_aux_row
                && state.reference_payload == before.reference_payload
                && state.telemetry == before.telemetry && validateDualCsrState(state).empty(),
            "local patch did not restore the actual old graph");
  };
  std::size_t prepare_allocations = 0;
  {
    ista_test_allocation::armCountOnly();
    detail::PreparedDualCsrGraphPatch patch(state, descriptor);
    prepare_allocations = ista_test_allocation::disable();
    const auto* nodes = state.nodes.data();
    const auto* arcs = state.arc_slots.data();
    ista_test_allocation::armFailure(1);
    patch.apply();
    const auto apply_allocations = ista_test_allocation::disable();
    require(apply_allocations == 0 && state.nodes.data() == nodes && state.arc_slots.data() == arcs,
            "prepared apply allocated or replaced canonical buffers");
    require(validateDualCsrState(state).empty(), "local patch produced an invalid graph");
    ista_test_allocation::armFailure(1);
    patch.rollback();
    const auto undo_allocations = ista_test_allocation::disable();
    require(undo_allocations == 0, "local graph rollback allocated");
  }
  restored();
  // Enumerate actual allocation failures in preparation, before any logical
  // modification. Grown capacities may remain available after failure.
  for (std::size_t ordinal = 1; ordinal <= prepare_allocations; ++ordinal) {
    try {
      ista_test_allocation::armFailure(ordinal);
      detail::PreparedDualCsrGraphPatch patch(state, descriptor);
      static_cast<void>(ista_test_allocation::disable());
      patch.apply();
      // Abandoning the patch must roll it back.
    } catch (const std::bad_alloc&) {
      static_cast<void>(ista_test_allocation::disable());
    }
    restored();
  }
  {
    detail::PreparedDualCsrGraphPatch patch(state, descriptor);
    patch.apply();
    patch.commit();
  }
  require(state.generation == descriptor.target_generation && validateDualCsrState(state).empty(),
          "committed local patch was undone on destruction");
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_dual_csr::test::referenceAppendContracts();
    ista::timing_dual_csr::test::tombstoneAndReuseContracts();
    if constexpr (ista::incregpusta_debug_checks_enabled)
      ista::timing_dual_csr::test::resourceObservationContracts();
    ista::timing_dual_csr::test::payloadAndDescriptorContracts();
    ista::timing_dual_csr::test::localPatchAtomicity();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
