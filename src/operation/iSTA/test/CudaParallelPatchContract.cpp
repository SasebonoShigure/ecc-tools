#include <algorithm>
#include <optional>

#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "CudaDualCsrTestReader.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"

namespace ista::csr::test {
using namespace timing_dual_csr;
using namespace timing_dual_csr::test;

namespace {
void launchShapeTiming(CudaLevelizationMode mode)
{
  constexpr std::size_t width = 1025;
  std::vector<DagArc> edges;
  for (std::size_t node = width; node > 0; --node)
    edges.push_back({0, node});
  for (std::size_t node = 1; node <= width; ++node) {
    edges.push_back({node, width + 1});
    if (node % 64 == 0)
      edges.push_back({node, width + 1});  // Parallel arcs retain distinct identity.
  }
  std::vector<CsrReferenceInput> cases;
  cases.push_back(makeCpuReferenceDagFixture(width + 2, edges));
  cases.push_back(makeCpuReferenceDagFixture(5, {{0, 3}, {0, 2}, {0, 1}, {1, 4}, {2, 4}, {3, 4}}));
  for (auto& arc : cases.back().arcs)
    if (arc.sink == NodeId{4})
      arc.transfer.delay = arc.transfer.delay_min = arc.transfer.delay_max =
          1.0 + static_cast<double>(3 - arc.source.value()) * 0.75e-9;
  for (std::size_t i = 0; i < cases.back().arcs.size(); ++i)
    cases.back().referenced_library_cells.front().cell_arcs[i].transfer = cases.back().arcs[i].transfer;
  refreshCpuReferenceIdentity(cases.back());
  cases.push_back(makeCpuReferencePropertyFixture(42));
  for (const auto& input : cases) {
    const timing_edit::AppendOnlyHighWater water{input.nodes.size(), 0, input.arcs.size()};
    const auto reference = CsrCpuReferenceBackend{}.compute(input);
    const auto* timing = std::get_if<CsrTimingResult>(&reference.outcome);
    require(timing != nullptr, "parallel timing fixture CPU reference failed");
    for (unsigned threads : {1U, 37U, 256U}) {
      CudaDualCsrSession owner(input, water);
      CudaDualCsrTestReader::setKernelThreads(owner, threads);
      for (unsigned repeat = 0; repeat < 3; ++repeat) {
        auto next = mode == CudaLevelizationMode::kFull
            ? owner.stageFull(input, water)
            : owner.stage(patchFor(input, owner.generation().value(), water, water), mode);
        require(!next.computationCode() && next.timingValues() == static_cast<const CsrTimingValues&>(*timing)
                    && owner.publish(std::move(next)), "parallel timing/winner differs by launch shape or repetition");
      }
    }
  }
  const auto& input = cases.front();
  const timing_edit::AppendOnlyHighWater water{input.nodes.size(), 0, input.arcs.size()};
  CudaDualCsrSession owner(input, water);
  const auto expected = CudaDualCsrTestReader::capture(owner);
  const auto expected_timing = owner.timingValues();
  auto migration = patchFor(input, 0, water, water);
  std::vector<ArcId> aux;
  for (std::size_t i = input.out_row_offsets[0]; i < input.out_row_offsets[1]; ++i) {
    migration.removes.push_back({NodeId{0}, i, input.out_arc_ids[i]});
    aux.push_back(input.out_arc_ids[i]);
  }
  migration.aux_rows = {{NodeId{0}, {}, aux}};
  {
    auto abandoned = owner.stage(migration, mode);
    require(!abandoned.computationCode() && abandoned.timingValues() == expected_timing,
            "wide Aux migration changed FIFO, timing or winner");
  }
  require(CudaDualCsrTestReader::capture(owner) == expected, "wide Aux abandonment lost real Base/row state");
}
}  // namespace

void runCudaParallelPatchContracts(CudaLevelizationMode mode)
{
  launchShapeTiming(mode);
  constexpr std::uint64_t width = 513;
  const auto input = makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater baseline{2, 2, 1}, water{2 + 2 * width, 2, 1 + width};
  auto first = patchFor(input, 0, baseline, water);
  std::vector<NodeId> roots{NodeId{0}}, ends{NodeId{1}};
  for (std::uint64_t i = 0; i < width; ++i) {
    const auto source = appendNode(2 + 2 * i, "parallel_source_" + std::to_string(i), AppendedPortRole::kSource);
    const auto sink = appendNode(3 + 2 * i, "parallel_sink_" + std::to_string(i), AppendedPortRole::kSink);
    first.node_appends.push_back(source);
    first.node_appends.push_back(sink);
    first.arc_appends.push_back(appendArc(i + 1, source.id, sink.id, source.key, sink.key, 0));
    first.appended_base_out_rows.push_back({source.id, {ArcId{i + 1}}});
    first.appended_base_out_rows.push_back({sink.id, {}});
    first.appended_base_in_rows.push_back({source.id, {}});
    first.appended_base_in_rows.push_back({sink.id, {ArcId{i + 1}}});
    roots.push_back(source.id);
    ends.push_back(sink.id);
    auto seed = input.start_seeds.front();
    seed.seed_node = seed.path_start_node = source.id;
    for (auto& analysis : seed.arrival)
      for (auto& value : analysis)
        value = 0.25 + i * 0.001;
    first.appended_start_seeds.push_back(std::move(seed));
  }
  first.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, roots};
  first.ends = ValueReplacement<std::vector<NodeId>>{{NodeId{1}}, ends};
  std::optional<nlohmann::json> expected;
  for (unsigned threads : {32U, 128U, 256U}) {
    CudaDualCsrSession owner(input, baseline);
    CudaDualCsrTestReader::setKernelThreads(owner, threads);
    auto duplicate = first;
    duplicate.ends->replacement.push_back(NodeId{1});
    bool rejected = false;
    try {
      static_cast<void>(owner.stage(duplicate, mode));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected && !owner.poisoned() && owner.generation().value() == 0,
            "duplicate ends accepted or changed the owner");
    auto created = owner.stage(first, mode);
    require(!created.computationCode() && owner.publish(std::move(created)), "parallel patch fixture failed");
    const auto committed = CudaDualCsrTestReader::capture(owner);
    auto payloads = patchFor(input, 1, water, water);
    for (const auto& arc : first.arc_appends) {
      auto replacement = arc.payload;
      replacement.is_loop_disabled = true;
      replacement.eligibility = {};
      payloads.payload_changes.push_back({arc.id, arc.payload, replacement});
    }
    auto disabled_roots = roots;
    for (const auto& node : first.node_appends)
      if (node.id.value() % 2 == 1) disabled_roots.push_back(node.id);
    payloads.roots = ValueReplacement<std::vector<NodeId>>{roots, disabled_roots};
    {
      auto abandoned = owner.stage(payloads, mode);
      require(!abandoned.computationCode(), "cross-block payload update failed");
      require(CudaDualCsrTestReader::capture(abandoned)["arcs"] != committed["arcs"],
              "payload update did not modify device arcs");
    }
    require(CudaDualCsrTestReader::capture(owner) == committed, "cross-block payload abandonment did not restore arcs");

    // A previous kernel really mutates BaseOut before many payload lanes fail.
    payloads.removes = {{NodeId{0}, 0, ArcId{0}}};
    payloads.aux_rows = {{NodeId{0}, {}, {ArcId{0}}}};
    for (std::size_t i = 0; i < payloads.payload_changes.size(); i += 2)
      payloads.payload_changes[i].expected.eligibility.data_arrival = false;
    auto failed = owner.stage(payloads, mode);
    require(failed.computationCode() == CsrComputationCode::kInvariantViolation && !owner.poisoned()
                && CudaDualCsrTestReader::capture(owner) == committed,
            "concurrent patch errors lost status or real partial rollback");

    const timing_edit::AppendOnlyHighWater replaced_water{water.node_slots, water.net_slots, water.arc_slots + width};
    auto replace = patchFor(input, 1, water, replaced_water);
    for (std::uint64_t i = 0; i < width; ++i) {
      const auto& old = first.arc_appends[i];
      const auto arc = appendArc(water.arc_slots + i, old.source, old.sink,
                                 first.node_appends[2 * i].key, first.node_appends[2 * i + 1].key, 1);
      replace.arc_appends.push_back(arc);
      replace.logical_deactivations.push_back({old.id, old.key, old.source, old.sink, LogicalArcDeactivateReason::kRemoved, true});
      replace.changes.push_back({old.source, i + 1, old.id, arc.id});
      replace.reverse_changes.push_back({old.sink, i + 1, old.id, arc.id});
    }
    {
      auto abandoned = owner.stage(replace, mode);
      require(!abandoned.computationCode(), "cross-block replacement/deactivation failed");
    }
    require(CudaDualCsrTestReader::capture(owner) == committed, "cross-block slot undo lost committed state");
    auto replaced = owner.stage(replace, mode);
    require(!replaced.computationCode() && owner.publish(std::move(replaced)), "replacement after rollback failed");
    auto to_aux = patchFor(input, 2, replaced_water, replaced_water);
    for (std::uint64_t i = 0; i < width; ++i) {
      const auto& arc = replace.arc_appends[i];
      to_aux.removes.push_back({arc.source, i + 1, arc.id});
      to_aux.aux_rows.push_back({arc.source, {}, {arc.id}});
    }
    auto moved = owner.stage(to_aux, mode);
    require(!moved.computationCode() && owner.publish(std::move(moved)), "parallel Base-to-Aux migration failed");
    const auto actual = CudaDualCsrTestReader::capture(owner);
    if (expected)
      require(actual == *expected, "patch/undo/seed results depend on launch shape");
    else
      expected = actual;
    const auto timing = owner.timingValues();
    for (std::uint64_t i = 0; i < width; ++i)
      require(timing.nodes.arrivals.values[3 + 2 * i] == 0.25 + i * 0.001,
              "parallel seed initialization lost a source arrival");
  }
}
}  // namespace ista::csr::test
