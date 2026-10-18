#include <algorithm>
#include <array>
#include <new>
#include <utility>

#include "AllocationFailureSupport.hpp"
#include "CudaDualCsrInternal.cuh"
#include "CudaDualCsrTestReader.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "CsrValidatorTestFixture.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"

namespace ista::csr {
void CudaDualCsrTestReader::clockWorkReuseContracts()
{
  using test::require;
  using test::patchFor;
  auto input = test::makeCpuReferenceCellFixture();
  input.clocks = {{"clk", "clk", {{input.nodes[0].key.full_name, NodeId{0}},
                                  {input.nodes[1].key.full_name, NodeId{1}}}, 10.0, {0.0, 5.0}, true}};
  input.clock_owner_by_node.assign(input.nodes.size(), std::string{"clk"});
  input.arcs[1].eligibility.clock = true;
  test::refreshCpuReferenceIdentity(input);
  const timing_edit::AppendOnlyHighWater water{3, 3, 2};
  // Component fixtures isolate clock scheduling; native export is covered by propagated-clock traces.
  auto owner = computeRc(input, 256, water);
  require(!owner.computationCode(), "clock work baseline failed");
  const std::array candidate_ids{NodeId{2}, NodeId{1}, NodeId{0}, NodeId{2}};
  const auto& gathered = readCandidateTiming(owner, candidate_ids);
  require(std::ranges::any_of(gathered, [](const auto& point) { return point.clock_slew > 0.0; }),
          "clock gather fixture lacks a positive computed clock slew");
  const auto baseline = owner.timingValues();
  for (std::size_t i = 0; i < candidate_ids.size(); ++i) {
    const auto point = bufferCandidateTiming(baseline, candidate_ids[i]);
    require(gathered[i].clock_slew == point.clock_slew && gathered[i].data_slew == point.data_slew
                && gathered[i].clock_point == point.clock_point && gathered[i].setup_slack == point.setup_slack,
            "sparse clock gather differs from the full committed result");
  }
  const auto groups = owner._impl->node_groups;
  auto& storage = *owner._impl->storage;
  require(storage.clock_work.size() == 3 && storage.clock_group_offsets.size() == 4,
          "clock fixture did not create three scheduled groups");
  std::uint64_t generation = 0;
  const auto verify = [&] {
    auto candidate = owner.stage(patchFor(input, generation, water, water), CudaLevelizationMode::kMixed);
    require(!candidate.computationCode() && candidate.timingValues() == baseline,
            "clock work reuse changed a repeated computation");
    require(storage.clock_group_owner.lock() == groups, "mixed clock work lost stable membership");
    require(owner.publish(std::move(candidate)), "clock work mixed publish failed");
    ++generation;
  };
  verify();
  verify();
  for (const bool fail_query : {false, true}) {
    if (fail_query && !incregpusta_debug_checks_enabled) continue;
    {
      auto abandoned = owner.stage(patchFor(input, generation, water, water), CudaLevelizationMode::kFull);
      require(!abandoned.computationCode() && storage.clock_group_owner.lock() == abandoned._impl->node_groups
                  && abandoned._impl->node_groups != groups, "full clock work did not replace the schedule");
      if (fail_query) {
        bool failed = false;
        ista_test_allocation::armFailure(0);
        try { static_cast<void>(abandoned.timingValues()); }
        catch (const std::bad_alloc&) { failed = true; }
        catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
        const bool injected = ista_test_allocation::failureInjected();
        static_cast<void>(ista_test_allocation::disable());
        require(failed && injected, "clock work query failure was not injected");
      }
    }
    require(storage.clock_group_owner.expired(), "abandoned full schedule retained its node membership");
    verify();
  }

  // Split the existing data-only net; old clock groups keep their relative order.
  auto insertion = patchFor(input, generation, water, {4, 4, 4});
  const auto middle = timing_dual_csr::test::appendNode(3, "clock_layer_data", timing_dual_csr::test::AppendedPortRole::kSink);
  insertion.node_appends = {middle};
  insertion.arc_appends = {
      timing_dual_csr::test::appendArc(2, NodeId{0}, middle.id, input.nodes[0].key, middle.key, 0),
      timing_dual_csr::test::appendArc(3, middle.id, NodeId{1}, middle.key, input.nodes[1].key, 0)};
  insertion.changes = {{NodeId{0}, 0, ArcId{0}, ArcId{2}}};
  insertion.reverse_changes = {{NodeId{1}, 0, ArcId{0}, ArcId{3}}};
  insertion.logical_deactivations = {{ArcId{0}, input.arcs[0].key, NodeId{0}, NodeId{1},
                                     timing_dual_csr::LogicalArcDeactivateReason::kRemoved, true}};
  insertion.appended_base_out_rows = {{middle.id, {ArcId{3}}}};
  insertion.appended_base_in_rows = {{middle.id, {ArcId{2}}}};
  auto independent = computeRc(input, 256, water);
  auto full_patch = insertion;
  full_patch.expected_generation = timing_edit::TransactionGeneration{0};
  full_patch.target_generation = timing_edit::TransactionGeneration{1};
  auto full = independent.stage(full_patch, CudaLevelizationMode::kFull);
  require(!full.computationCode(), "shifted clock fixture full computation failed");
  {
    auto shifted = owner.stage(insertion, CudaLevelizationMode::kMixed);
    require(!shifted.computationCode() && shifted.timingValues().node_to_level[1] == 3
                && shifted.timingValues() == full.timingValues(),
            "shifted mixed layers changed clock computation");
    require(storage.clock_group_owner.lock() == groups, "layer shift rebuilt stable clock grouping");
  }
  verify();

  if constexpr (incregpusta_debug_checks_enabled) {
    auto growth_input = test::makeCpuReferenceDagFixture(5, {{0, 1}, {1, 2}, {2, 3}, {3, 4}});
    growth_input.ordered_root_ids.clear();
    growth_input.clocks = {{"clk", "clk", {}, 10.0, {0.0, 5.0}, true}};
    growth_input.clock_owner_by_node.assign(5, std::string{"clk"});
    for (const auto& node : growth_input.nodes) {
      growth_input.ordered_root_ids.push_back(node.id);
      growth_input.clocks[0].ordered_sources.push_back({node.key.full_name, node.id});
    }
    for (auto& edge : growth_input.arcs) edge.eligibility = {};
    test::refreshCpuReferenceIdentity(growth_input);
    const timing_edit::AppendOnlyHighWater growth_water{5, 5, 4};
    auto growth_owner = computeRc(growth_input, 256, growth_water);
    const auto saved = growth_owner.timingValues();
    auto split = patchFor(growth_input, 0, growth_water, growth_water);
    for (const auto& edge : growth_input.arcs) {
      ArcSemanticPayload old{edge.key, edge.source, edge.sink, edge.kind, edge.is_clock_arc,
          edge.is_disabled, edge.is_loop_disabled, edge.eligibility, edge.transfer, edge.output_load, edge.native_cell_arc_ordinal};
      auto next = old;
      next.eligibility.levelization = true;
      split.payload_changes.push_back({edge.id, old, next});
    }
    split.roots = timing_dual_csr::ValueReplacement<std::vector<NodeId>>{growth_input.ordered_root_ids, {NodeId{0}}};
    auto& growth_storage = *growth_owner._impl->storage;
    ista_test_allocation::armFailureWhen([](void* state) noexcept {
      return static_cast<cuda_detail::DeviceStorage*>(state)->clock_group_owner.expired();
    }, &growth_storage);
    bool failed = false;
    try { auto candidate = growth_owner.stage(split, CudaLevelizationMode::kFull); }
    catch (const std::bad_alloc&) { failed = true; }
    catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
    const bool injected = ista_test_allocation::failureInjected();
    static_cast<void>(ista_test_allocation::disable());
    require(failed && injected && !growth_owner.poisoned(),
            "clock group growth did not exercise recoverable host allocation failure");
    auto retry = growth_owner.stage(patchFor(growth_input, 0, growth_water, growth_water), CudaLevelizationMode::kMixed);
    require(!retry.computationCode() && retry.timingValues() == saved && growth_owner.publish(std::move(retry)),
            "failed clock group rebuild damaged mixed retry");
  }

  const auto& arc = input.arcs[1];
  const ArcSemanticPayload before{arc.key, arc.source, arc.sink, arc.kind, arc.is_clock_arc,
      arc.is_disabled, arc.is_loop_disabled, arc.eligibility, arc.transfer, arc.output_load, arc.native_cell_arc_ordinal};
  for (const bool enable : {false, true}) {
    auto updated = input;
    updated.arcs[1].eligibility.clock = enable;
    if (enable) {
      auto children = updated.arcs[1].transfer.children();
      for (auto& child : children)
        for (auto& table : child.delay_tables)
          for (auto& value : table.values) value += 0.5;
      updated.arcs[1].transfer.setChildren(children);
      updated.referenced_library_cells[0].cell_arcs[0].transfer = updated.arcs[1].transfer;
    }
    test::refreshIdentity(updated);
    auto fresh = computeRc(updated, 256, water);
    require(!fresh.computationCode(), "clock replacement fresh computation failed");
    auto after = before;
    after.eligibility = updated.arcs[1].eligibility;
    after.transfer = updated.arcs[1].transfer;
    auto patch = patchFor(input, generation, water, water);
    patch.payload_changes = {{arc.id, before, after}};
    {
      auto candidate = owner.stage(patch, CudaLevelizationMode::kMixed);
      require(!candidate.computationCode() && candidate.timingValues() == fresh.timingValues(),
              "clock work cache reused stale clock timing after payload change");
      require(storage.clock_group_owner.lock() == groups, "clock payload unnecessarily changed grouping");
    }
    verify();
  }
}
}  // namespace ista::csr
