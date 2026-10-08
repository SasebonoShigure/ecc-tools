#include <algorithm>
#include <iostream>
#include <limits>

#include "AllocationFailureSupport.hpp"
#include "CsrModelTestMutation.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "CudaDualCsrDebugState.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"
#include "CsrCudaPacking.hpp"
#include "CudaDualCsrTestReader.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"

namespace ista::csr::test {
using namespace timing_dual_csr;
using namespace timing_dual_csr::test;
namespace {
void auxReuseAndUndo(CudaLevelizationMode mode)
{
  const auto input = csr::test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater baseline{2, 2, 1}, water{6, 6, 5};
  CudaDualCsrSession owner(input, baseline);
  auto first = patchFor(input, 0, baseline, water);
  for (std::uint64_t i = 0; i < 4; ++i) {
    const auto sink = appendNode(i + 2, "aux_" + std::to_string(i), AppendedPortRole::kSink);
    const NodeId source{i < 2 ? 0U : 1U};
    first.node_appends.push_back(sink);
    first.arc_appends.push_back(appendArc(i + 1, source, sink.id, input.nodes[source.value()].key, sink.key, 0));
    first.appended_base_out_rows.push_back({sink.id, {}});
    first.appended_base_in_rows.push_back({sink.id, {ArcId{i + 1}}});
  }
  first.aux_rows = {{NodeId{0}, {}, {ArcId{1}, ArcId{2}}}, {NodeId{1}, {}, {ArcId{3}, ArcId{4}}}};
  auto created = owner.stage(first, mode);
  require(!created.computationCode() && owner.publish(std::move(created)), "Aux reuse fixture could not publish");
  const auto initial_aux_values = owner.timingValues();
  std::vector<ArcId> order{ArcId{1}, ArcId{2}};
  for (std::uint64_t generation = 1; generation < 6; ++generation) {
    auto reverse = order;
    std::reverse(reverse.begin(), reverse.end());
    auto patch = patchFor(input, generation, water, water);
    patch.aux_rows = {{NodeId{0}, order, reverse}};
    auto candidate = owner.stage(patch, mode);
    require(!candidate.computationCode(), "Aux row replacement failed");
    if (generation >= 3) {
      require(candidate.telemetry().cuda.allocations == 0, "steady Aux row replacement allocated device storage");
      if (mode == CudaLevelizationMode::kFull)
        require(candidate.telemetry().cuda.d2d_bytes == 0, "steady Aux row replacement copied unchanged device graph");
    }
    require(owner.publish(std::move(candidate)), "Aux row replacement did not publish");
    order = std::move(reverse);
  }
  const auto saved = owner.timingValues();
  {
    auto full_abandoned = owner.stageFull(input, baseline);
    require(!full_abandoned.computationCode(), "full rebuild after Aux updates failed");
  }
  require(owner.timingValues() == saved, "abandoned full rebuild discarded Aux state");
  auto cyclic_full = csr::test::makeCpuReferenceCycleFixture();
  csr::test::refreshCpuReferenceIdentity(cyclic_full);
  cyclic_full.source_identity.session_token = input.source_identity.session_token;
  auto full_failed = owner.stageFull(cyclic_full, {cyclic_full.nodes.size(), cyclic_full.nodes.size(), cyclic_full.arcs.size()});
  require(full_failed.computationCode() == CsrComputationCode::kCycle && !owner.poisoned(),
          "full failure after Aux updates changed owner");
  auto reversed = order;
  std::reverse(reversed.begin(), reversed.end());
  auto partial = patchFor(input, 6, water, water);
  partial.aux_rows = {{NodeId{0}, order, reversed}, {NodeId{1}, {ArcId{4}, ArcId{3}}, {ArcId{3}, ArcId{4}}}};
  auto failed = owner.stage(partial, mode);
  require(failed.computationCode() == CsrComputationCode::kInvariantViolation && !owner.poisoned(),
          "second Aux expected-value mismatch was not rejected");
  {
    auto verify = owner.stage(patchFor(input, 6, water, water), mode);
    require(!verify.computationCode() && verify.timingValues() == saved, "partial Aux rollback left changed row values");
  }
  auto growth = patchFor(input, 6, water, {7, 7, 6});
  const auto sink = appendNode(6, "aux_grown", AppendedPortRole::kSink);
  growth.node_appends = {sink};
  growth.arc_appends = {appendArc(5, NodeId{0}, sink.id, input.nodes[0].key, sink.key, 0)};
  growth.appended_base_out_rows = {{sink.id, {}}};
  growth.appended_base_in_rows = {{sink.id, {ArcId{5}}}};
  auto grown = reversed;
  grown.push_back(ArcId{5});
  growth.aux_rows = {{NodeId{0}, order, grown}};
  {
    auto abandoned = owner.stage(growth, mode);
    require(!abandoned.computationCode(), "Aux row capacity growth failed");
  }
  auto verify = owner.stage(patchFor(input, 6, water, water), mode);
  require(!verify.computationCode() && verify.timingValues() == saved && owner.publish(std::move(verify)),
          "abandoned Aux growth lost old row storage or prevented continuation");
  auto full = owner.stageFull(input, baseline);
  require(!full.computationCode() && owner.publish(std::move(full)), "full replacement of Aux graph failed");
  first.expected_generation = owner.generation();
  first.target_generation = timing_edit::TransactionGeneration{owner.generation().value() + 1};
  auto recreated = owner.stage(first, mode);
  require(!recreated.computationCode() && recreated.timingValues() == initial_aux_values && owner.publish(std::move(recreated)),
          "Aux append after full replacement retained stale row counts or bindings");
}

void hostAllocationRollback(CudaLevelizationMode mode)
{
  if constexpr (!incregpusta_debug_checks_enabled)
    return;
  const auto input = csr::test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  auto patch = patchFor(input, 0, water, {4, 4, 3});
  const auto source = appendNode(2, "allocation_source", AppendedPortRole::kSource);
  const auto sink = appendNode(3, "allocation_sink", AppendedPortRole::kSink);
  patch.node_appends = {source, sink};
  patch.arc_appends = {appendArc(1, source.id, NodeId{1}, source.key, input.nodes[1].key, 0),
                       appendArc(2, source.id, sink.id, source.key, sink.key, 0)};
  patch.removes = {{NodeId{0}, 0, ArcId{0}}};
  patch.logical_deactivations = {{ArcId{0}, input.arcs[0].key, NodeId{0}, NodeId{1}, LogicalArcDeactivateReason::kRemoved, true}};
  patch.reverse_changes = {{NodeId{1}, 0, ArcId{0}, ArcId{1}}};
  patch.appended_base_out_rows = {{source.id, {ArcId{1}, ArcId{2}}}, {sink.id, {}}};
  patch.appended_base_in_rows = {{source.id, {}}, {sink.id, {ArcId{2}}}};
  patch.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, {NodeId{0}, source.id}};
  patch.ends = ValueReplacement<std::vector<NodeId>>{{NodeId{1}}, {NodeId{1}, sink.id}};
  patch.appended_endpoints = {{sink.id, "clk", 0.0, {2.0, std::nullopt}}};
  auto seed = input.start_seeds[0];
  seed.seed_node = source.id;
  seed.path_start_node = source.id;
  patch.appended_start_seeds = {seed};
  std::size_t allocations{};
  CsrTimingValues expected;
  {
    CudaDualCsrSession owner(input, water);
    ista_test_allocation::armCountOnly();
    auto candidate = owner.stage(patch, mode);
    allocations = ista_test_allocation::disable();
    require(!candidate.computationCode(), "allocation rollback fixture failed");
    expected = candidate.timingValues();
  }
  for (std::size_t ordinal = 0; ordinal < allocations; ++ordinal) {
    CudaDualCsrSession owner(input, water);
    const auto committed = owner.timingValues();
    bool failed = false;
    ista_test_allocation::armFailure(ordinal);
    try {
      auto candidate = owner.stage(patch, mode);
    } catch (const std::bad_alloc&) {
      failed = true;
    } catch (...) {
      static_cast<void>(ista_test_allocation::disable());
      throw;
    }
    static_cast<void>(ista_test_allocation::disable());
    require(failed && !owner.poisoned() && owner.generation().value() == 0
                && owner.timingValues() == committed,
            "host allocation failure changed committed CUDA owner at " + std::to_string(ordinal));
    auto retry = owner.stage(patch, mode);
    require(!retry.computationCode() && retry.timingValues() == expected && owner.publish(std::move(retry)),
            "host allocation rollback left stale graph/seed storage at " + std::to_string(ordinal));
  }
}

ArcSemanticPayload payloadFor(const ArcRecord& arc)
{
  return {arc.key, arc.source, arc.sink, arc.kind, arc.is_clock_arc, arc.is_disabled,
          arc.is_loop_disabled, arc.eligibility, arc.transfer, arc.output_load, arc.native_cell_arc_ordinal};
}

void sharedModelLoads(CudaLevelizationMode mode)
{
  auto input = csr::test::makeCpuReferencePropertyFixture(2);
  auto model = input.arcs[1].transfer;
  ista::csr::test::mutateChildren(model, [&](auto& children) {
    for (auto& child : children)
    for (auto& table : child.delay_tables) {
      table.variable1 = TableVariable::kOutputCapacitance;
    }
  for (auto& child : children)
    for (auto& signature : child.signature.delay_tables)
      signature.variable1 = TableVariable::kOutputCapacitance;
  });
  for (std::size_t id = 1; id < input.arcs.size(); ++id) {
    input.arcs[id].transfer = model;
    input.arcs[id].key.base.library_arcs.clear();
    for (const auto& child : model.children())
      input.arcs[id].key.base.library_arcs.push_back(child.signature);
    input.arc_ids_by_key[id].key = input.arcs[id].key;
    input.referenced_library_cells.front().cell_arcs[id - 1].transfer = model;
  }
  csr::test::refreshCpuReferenceIdentity(input);
  const timing_edit::AppendOnlyHighWater water{input.nodes.size(), 0, input.arcs.size()};
  CudaDualCsrSession owner(input, water);
  auto expected = payloadFor(input.arcs[1]);
  cuda_detail::ModelStorage packed_models;
  const auto packed = cuda_detail::packArc(expected, packed_models);
  ista_test_allocation::armCountOnly();
  bool same_range = true;
  for (unsigned i = 0; i < 100; ++i)
    same_range = same_range && cuda_detail::packArc(expected, packed_models).child_begin == packed.child_begin;
  const auto packing_allocations = ista_test_allocation::disable();
  require(same_range && packing_allocations == 0, "unchanged model packing allocated another payload");
  // Public expected values may have independent immutable storage.
  expected.transfer.setChildren(expected.transfer.children());
  for (std::uint64_t batch = 0; batch < 5; ++batch) {
    auto replacement = expected;
    replacement.output_load = csr::test::outputLoad(0.125 * (batch + 1));
    auto patch = patchFor(input, batch, water, water);
    patch.payload_changes = {{ArcId{1}, expected, replacement}};
    auto candidate = owner.stage(patch, mode);
    require(!candidate.computationCode(), "shared model load update failed");
    const auto& timing = candidate.timingValues();
    const auto first = timing.arcs.timing_arc_row_offsets[1] * 8;
    const auto second = timing.arcs.timing_arc_row_offsets[2] * 8;
    require(timing.arcs.timing_arc_delays.values[first] > timing.arcs.timing_arc_delays.values[second],
            "instances sharing an immutable model also shared their load-dependent result");
    if constexpr (incregpusta_debug_checks_enabled) {
      const auto state = candidate.debugState();
      require(state.children.size() == model.children().size() && state.arcs[1].child_begin == state.arcs[2].child_begin
                  && state.arcs[1].child_state_begin != state.arcs[2].child_state_begin,
              "load-only update duplicated models or aliased child state");
      input.arcs[1].output_load = replacement.output_load;
      csr::test::refreshCpuReferenceIdentity(input);
      // This unit case deliberately supplies independent arc loads. The
      // public flat-input validator instead derives loads from sink nets.
      const auto cpu = CsrCpuReferenceBackend{}.compute(FlatCsrEdgeView(input), FlatTimingPayloadView(input));
      const auto* reference = std::get_if<CsrTimingResult>(&cpu.outcome);
      require(reference != nullptr, "shared model CPU oracle rejected input: outcome " + std::to_string(cpu.outcome.index()));
      CsrTimingResult actual;
      static_cast<CsrTimingValues&>(actual) = timing;
      actual.schema_version = reference->schema_version;
      actual.source_identity = reference->source_identity;
      std::string mismatch;
      const bool equal = csr::test::exactTimingResultEqual(actual, *reference, &mismatch);
      require(equal, "shared model load update differs from CPU oracle: " + mismatch);
    }
    require(owner.publish(std::move(candidate)), "shared model update did not publish");
    expected = std::move(replacement);
  }
}

void payloadFailures(CudaLevelizationMode mode)
{
  const auto input = csr::test::makeCpuReferenceCellFixture();
  const timing_edit::AppendOnlyHighWater water{input.nodes.size(), 0, input.arcs.size()};
  CudaDualCsrSession owner(input, water);
  const auto found = std::ranges::find(input.arcs, ArcKind::kCell, &ArcRecord::kind);
  require(found != input.arcs.end() && !found->transfer.children().empty(), "cell fixture lacks a timing child");
  const auto& arc = *found;
  const auto expected = payloadFor(arc);
  auto patch = patchFor(input, 0, water, water);
  const auto committed = owner.timingValues();
  const auto old_launches = owner.telemetry().cuda.kernel_launches;
  const auto old_algorithms = owner.telemetry().cuda.device_algorithm_calls;
  auto changed = expected;
  require(changed.output_load.has_value(), "cell transaction fixture lacks load");
  (*changed.output_load)[0][0] += 0.125;
  patch.payload_changes = {{arc.id, expected, changed}};
  const auto other = std::ranges::find(input.arcs, ArcKind::kNet, &ArcRecord::kind);
  require(other != input.arcs.end(), "partial patch fixture lacks a second arc");
  const auto original_net = payloadFor(*other);
  const auto position = std::ranges::find(input.out_arc_ids, other->id);
  require(position != input.out_arc_ids.end(), "partial patch fixture lacks its physical slot");
  const auto slot = static_cast<std::uint64_t>(position - input.out_arc_ids.begin());
  patch.removes = {{other->source, slot, other->id}};
  patch.aux_rows = {{other->source, {}, {other->id}}};
  {
    auto abandoned = owner.stage(patch, mode);
    require(!abandoned.computationCode(), "local undo fixture did not compute");
    if constexpr (incregpusta_debug_checks_enabled) {
      const auto graph = abandoned.debugState();
      require(graph.arcs[arc.id.value()].load[0] == (*changed.output_load)[0][0]
                  && graph.out_arcs[slot] == cuda_detail::absent
                  && graph.aux_rows[other->source.value()] == std::vector<cuda_detail::Index>{other->id.value()},
              "abandonment fixture did not modify the device payload and adjacency");
    }
  }
  require(!owner.poisoned() && owner.timingValues() == committed && owner.telemetry().cuda.kernel_launches == old_launches
              && owner.telemetry().cuda.device_algorithm_calls == old_algorithms,
          "abandoned device transaction changed committed result or charged previous generation");
  if constexpr (incregpusta_debug_checks_enabled) {
    const auto graph = owner.debugState();
    require(graph.out_arcs[slot] == other->id.value() && graph.aux_rows[other->source.value()].empty(),
            "abandonment did not restore the physical adjacency before the failure test");
  }
  auto wrong_expected = original_net;
  wrong_expected.output_load = csr::test::outputLoad(123.0);
  // Base removal completes in an earlier kernel than the payload mismatch.
  // Actual partial mutation must be restored regardless of payload thread order.
  patch.payload_changes.push_back({other->id, wrong_expected, original_net});
  auto partial = owner.stage(patch, mode);
  require(partial.computationCode() == CsrComputationCode::kInvariantViolation && !owner.poisoned()
              && owner.timingValues() == committed && owner.telemetry().cuda.kernel_launches == old_launches
              && owner.telemetry().cuda.device_algorithm_calls == old_algorithms,
          "partial patch failure changed committed results or rollback accounting");
  require(partial.close() && !owner.poisoned() && owner.timingValues() == committed,
          "closing a rejected candidate destroyed the healthy runtime owner");
  {
    auto verify_device = owner.stage(patchFor(input, 0, water, water), mode);
    require(!verify_device.computationCode() && verify_device.timingValues() == committed,
            "partial patch rollback left modified device state");
  }
  patch.removes.clear();
  patch.aux_rows.clear();
  auto malformed = expected;
  ista::csr::test::mutateChildren(malformed.transfer, [&](auto& children) { children[0].delay_tables[0].values.push_back(1.0); });
  patch.payload_changes = {{arc.id, expected, malformed}};
  bool rejected = false;
  try {
    static_cast<void>(owner.stage(patch, mode));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected && !owner.poisoned(), "malformed cell table accepted or poisoned owner");
  auto huge = expected;
  ista::csr::test::mutateChildren(huge.transfer, [&](auto& children) {
    for (auto& child : children)
    for (auto& table : child.delay_tables)
      for (auto& value : table.values)
        value = std::numeric_limits<double>::max();
  });
  patch.payload_changes = {{arc.id, expected, huge}};
  auto failure = owner.stage(patch, mode);
  require(failure.computationCode() == CsrComputationCode::kNumericFailure && !owner.publish(std::move(failure)) && !owner.poisoned(),
          "actual GPU numeric failure changed committed owner");
  require(failure.close() && !owner.poisoned(), "closing numeric failure destroyed the healthy runtime owner");
  auto healthy = owner.stage(patchFor(input, 0, water, water), mode);
  require(!healthy.computationCode() && owner.publish(std::move(healthy)), "healthy replay after numeric rejection failed");
}

void persistentFullRebuild()
{
  auto input = csr::test::makeCpuReferenceCellFixture();
  const timing_edit::AppendOnlyHighWater water{input.nodes.size(), input.nodes.size(), input.arcs.size()};
  CudaDualCsrSession owner(input, water);
  const auto expected = owner.timingValues();
  const auto baseline_upload = owner.telemetry().cuda.h2d_bytes;
  const auto levels_only = sizeof(cuda_detail::Status) + sizeof(cuda_detail::Index)
      + (input.nodes.size() * (incregpusta_debug_checks_enabled ? 2 : 1) + expected.level_offsets.size()) * sizeof(cuda_detail::Index);
  for (unsigned batch = 0; batch < 5; ++batch) {
    auto candidate = owner.stageFull(input, water);
    require(!candidate.computationCode() && candidate.telemetry().cuda.d2h_bytes == levels_only + sizeof(cuda_detail::Status) + candidate.telemetry().cuda.levelization_decision_bytes,
            "full rebuild materialized unrequested timing");
    require(candidate.telemetry().cuda.h2d_bytes < baseline_upload,
            "full rebuild retransmitted unchanged immutable models");
    if (batch >= 2)
      require(candidate.telemetry().cuda.allocations == 0 && candidate.telemetry().cuda.d2d_bytes == 0,
              "steady full rebuild reallocated or copied the device graph");
    require(owner.publish(std::move(candidate)), "persistent full publication failed");
  }
  require(owner.timingValues() == expected, "persistent full rebuild changed timing");
  const auto transferred = owner.telemetry().cuda.d2h_bytes;
  require(owner.timingValues() == expected && owner.telemetry().cuda.d2h_bytes == transferred,
          "repeated full query downloaded timing twice");
  {
    auto abandoned = owner.stageFull(input, water);
    require(!abandoned.computationCode(), "full abandonment preparation failed");
  }
  require(!owner.poisoned() && owner.timingValues() == expected, "abandoned full rebuild changed committed timing");
  auto numeric = input;
  auto& arc = *std::ranges::find(numeric.arcs, ArcKind::kCell, &ArcRecord::kind);
  ista::csr::test::mutateChildren(arc.transfer, [&](auto& children) {
    for (auto& child : children)
    for (std::size_t output = 0; output < kTransitionCount; ++output) {
      child.signature.slew_tables[output] = {true, TableVariable::kNone, TableVariable::kNone};
      child.slew_tables[output] = {true, TableVariable::kNone, TableVariable::kNone, {}, {-1.0}};
    }
  });
  arc.key.base.library_arcs.clear();
  for (const auto& child : arc.transfer.children())
    arc.key.base.library_arcs.push_back(child.signature);
  numeric.arc_ids_by_key[arc.id.value()].key = arc.key;
  numeric.referenced_library_cells.front().cell_arcs.front().transfer = arc.transfer;
  csr::test::refreshCpuReferenceIdentity(numeric);
  numeric.source_identity.session_token = input.source_identity.session_token;
  auto numeric_failure = owner.stageFull(numeric, water);
  require(numeric_failure.computationCode() == CsrComputationCode::kNumericFailure
              && numeric_failure.telemetry().grown_buffers > 0 && !owner.poisoned()
              && owner.timingValues() == expected,
          "full model growth failure changed committed results");
  auto original_model = owner.stageFull(input, water);
  require(!original_model.computationCode() && original_model.timingValues() == expected && owner.publish(std::move(original_model)),
          "old models failed after rejected full model growth");
  auto cyclic = csr::test::makeCpuReferenceCycleFixture();
  csr::test::refreshCpuReferenceIdentity(cyclic);
  cyclic.source_identity.session_token = input.source_identity.session_token;
  auto rejected = owner.stageFull(cyclic, {cyclic.nodes.size(), cyclic.nodes.size(), cyclic.arcs.size()});
  require(rejected.computationCode() == CsrComputationCode::kCycle && !owner.publish(std::move(rejected))
              && !owner.poisoned() && owner.timingValues() == expected,
          "failed full rebuild lost the previous generation");
  auto next = owner.stage(patchFor(input, owner.generation().value(), water, water), CudaLevelizationMode::kMixed);
  require(!next.computationCode() && next.timingValues() == expected && owner.publish(std::move(next)),
          "incremental commit after full rollback used stale graph storage");
}

}  // namespace

void runCudaLazySequence(CudaLevelizationMode mode)
{
  const auto input = csr::test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  CudaDualCsrSession owner(input, water);
  require(!owner.computationCode(), "lazy baseline failed");
  const auto baseline_readback = owner.telemetry().cuda.d2h_bytes;
  for (std::uint64_t generation = 0; generation < 3; ++generation) {
    auto staged = owner.stage(patchFor(input, generation, water, water), mode);
    require(!staged.computationCode() && staged.telemetry().materialize_ms == 0.0,
            "unqueried batch materialized timing");
    if (generation >= 1)
      require(staged.telemetry().cuda.allocations == 0, "steady-size batch reallocated device storage");
    if (mode == CudaLevelizationMode::kFull)
      require(staged.telemetry().cuda.d2d_bytes == 0, "steady-size full propagation copied the device graph");
    require(owner.publish(std::move(staged)), "unqueried batch could not commit");
  }
  const auto before_query = owner.telemetry().cuda;
  const auto& timing = CudaDualCsrTestReader::materializeWithLevelOwnershipCheck(owner);
  const auto first_query = owner.telemetry().cuda;
  require(first_query.d2h_bytes > before_query.d2h_bytes && baseline_readback < first_query.d2h_bytes,
          "first query did not download deferred timing");
  require(&owner.timingValues() == &timing && owner.telemetry().cuda.d2h_bytes == first_query.d2h_bytes
              && owner.telemetry().cuda.kernel_launches == before_query.kernel_launches
              && owner.telemetry().cuda.device_algorithm_calls == before_query.device_algorithm_calls,
          "repeated query transferred or recomputed timing");
  const auto expected = timing;
  auto staged = owner.stage(patchFor(input, 3, water, water), mode);
  require(owner.publish(std::move(staged)), "query then edit failed");
  const auto next_before = owner.telemetry().cuda.d2h_bytes;
  require(owner.timingValues() == expected && owner.telemetry().cuda.d2h_bytes > next_before,
          "new generation reused old timing cache");
}

void runCudaDualCsrStorageContracts(CudaLevelizationMode mode)
{
  runCudaParallelPatchContracts(mode);
  persistentFullRebuild();
  runCudaLazySequence(mode);
  auxReuseAndUndo(mode);
  sharedModelLoads(mode);
  payloadFailures(mode);
  hostAllocationRollback(mode);
}
}  // namespace ista::csr::test
