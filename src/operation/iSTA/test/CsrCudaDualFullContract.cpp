#include <stdexcept>

#include "AllocationFailureSupport.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "CsrModelTestMutation.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCudaPacking.hpp"
#include "CudaDualCsrTestReader.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"

namespace ista::csr::test {
namespace {
void require(bool value, const std::string& message)
{
  if (!value) throw std::runtime_error(message);
}
void directFullAllocationFailure(const CsrReferenceInput& input, const timing_dual_csr::DualCsrState& graph,
                                 timing_edit::AppendOnlyHighWater water, const CsrTimingValues& original)
{
  if constexpr (!incregpusta_debug_checks_enabled) return;
  std::size_t allocations;
  CsrTimingValues expected;
  {
    CudaDualCsrSession measured(input, water);
    ista_test_allocation::armCountOnly();
    auto candidate = CudaDualCsrTestReader::stageFullCsr(measured, graph);
    allocations = ista_test_allocation::disable();
    require(!candidate.computationCode(), "direct full allocation count fixture failed");
    expected = candidate.timingValues();
  }
  for (std::size_t ordinal = 0; ordinal < allocations; ++ordinal) {
    CudaDualCsrSession measured(input, water);
    bool failed = false;
    std::optional<CudaDualCsrSession> candidate;
    ista_test_allocation::armFailure(ordinal);
    try { candidate.emplace(CudaDualCsrTestReader::stageFullCsr(measured, graph)); }
    catch (const std::bad_alloc&) { failed = true; }
    catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
    const bool injected = ista_test_allocation::failureInjected();
    static_cast<void>(ista_test_allocation::disable());
    require(injected, "direct full allocation scan missed ordinal " + std::to_string(ordinal));
    // std::stable_sort may recover from a nothrow scratch-allocation failure.
    // Such success must still produce the complete result and undo normally.
    if (candidate) {
      require(!failed && !candidate->computationCode() && candidate->timingValues() == expected && candidate->close(),
              "direct full allocation recovery changed the staged result");
    } else {
      require(failed, "direct full allocation failure had no result or exception");
    }
    require(!measured.poisoned() && measured.generation().value() == 0,
            "direct full allocation exception lost committed owner at " + std::to_string(ordinal));
    auto verify = measured.stage(patchFor(input, 0, water, water));
    require(!verify.computationCode() && verify.timingValues() == original
                && measured.publish(std::move(verify)) && measured.close(),
            "direct full allocation exception corrupted restored device graph or retry");
  }
}
}

void runCudaDualFullContracts()
{
  using namespace timing_dual_csr;
  const auto input = makeCpuReferenceCellFixture();
  const timing_edit::AppendOnlyHighWater water{input.nodes.size(), 2, input.arcs.size()};
  auto baseline = DualCsrBaselineFactory::create(input, timing_edit::TimingEditSessionToken{input.source_identity.session_token}, water);
  require(baseline.success(), "direct full fixture baseline failed");
  auto graph = std::move(*baseline.state);
  CudaDualCsrSession owner(input, water), stranger(input, water);
  const auto original = owner.timingValues();
  // Move a live net arc into Aux and retire its original stable slot. The
  // cell arc retains its ID, and full packing must reconstruct both Base rows.
  graph.arc_slots.push_back(graph.arc_slots.front());
  graph.arc_slots.front().active = false;
  graph.high_water.arc_slots = graph.arc_slots.size();
  graph.base_out.physical_slots.front().reset();
  graph.base_in.physical_slots.front() = ArcId{graph.arc_slots.size() - 1};
  graph.aux_out.row_sources = {NodeId{0}};
  graph.aux_out.row_arcs = {{ArcId{graph.arc_slots.size() - 1}}};
  graph.aux_out.source_to_aux_row[0] = 0;
  graph.generation = timing_edit::TransactionGeneration{1};
  directFullAllocationFailure(input, graph, water, original);
  cuda_detail::ModelStorage models;
  const auto packed = cuda_detail::pack(graph, models);
  require(packed.arcs.size() == graph.arc_slots.size() && !packed.arcs[0].active
              && !packed.layout.arcs[0].active && !packed.bindings[0].active
              && packed.out_arcs == std::vector<cuda_detail::Index>{2, 1}
              && packed.in_arcs == std::vector<cuda_detail::Index>{2, 1},
          "full packing lost tombstone or Aux stable IDs");

  CsrTimingValues expected;
  if constexpr (incregpusta_debug_checks_enabled) {
    const auto cpu = CsrCpuReferenceBackend{}.compute(DualCsrEdgeViewImpl(graph), DualCsrTimingPayloadViewImpl(graph));
    const auto* values = std::get_if<CsrTimingResult>(&cpu.outcome);
    require(values != nullptr, "direct full independent CPU oracle failed");
    expected = *values;
  }
  {
    auto abandoned = CudaDualCsrTestReader::stageFullCsr(owner, graph);
    require(!abandoned.computationCode() && abandoned.telemetry().materialize_ms == 0.0,
            "direct full rebuild failed or eagerly materialized timing");
    require(!stranger.canPublish(abandoned), "direct full accepted a foreign owner");
    if constexpr (incregpusta_debug_checks_enabled)
      require(abandoned.timingValues() == expected, "direct full differs from independent CPU result");
  }
  require(!owner.poisoned() && owner.generation().value() == 0 && owner.timingValues() == original,
          "direct full abandonment changed committed state");
  auto malformed = graph;
  malformed.base_in.physical_slots.front() = ArcId{graph.arc_slots.size()};
  bool rejected = false;
  try { auto candidate = CudaDualCsrTestReader::stageFullCsr(owner, malformed); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected && !owner.poisoned() && owner.timingValues() == original,
          "malformed direct full did not preserve committed state");
  auto first = CudaDualCsrTestReader::stageFullCsr(owner, graph);
  require(!first.computationCode() && owner.publish(std::move(first)), "direct full publication failed");
  rejected = false;
  try { auto stale = CudaDualCsrTestReader::stageFullCsr(owner, graph); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "direct full accepted a stale generation");
  const auto first_values = owner.timingValues();
  auto numeric = graph;
  numeric.generation = timing_edit::TransactionGeneration{2};
  auto& cell = numeric.arc_slots[1].payload.transfer;
  mutateChildren(cell, [](auto& children) {
    for (auto& child : children)
      for (unsigned output = 0; output < 2; ++output) {
        child.signature.slew_tables[output] = {true, TableVariable::kNone, TableVariable::kNone};
        child.slew_tables[output] = {true, TableVariable::kNone, TableVariable::kNone, {}, {-1.0}};
      }
  });
  auto failure = CudaDualCsrTestReader::stageFullCsr(owner, numeric);
  require(failure.computationCode() == CsrComputationCode::kNumericFailure
              && failure.telemetry().grown_buffers > 0 && !owner.publish(std::move(failure))
              && failure.close() && !owner.poisoned(),
          "direct full numeric failure did not restore after model/device capacity growth");
  {
    // This computation reads the restored device graph, not the old host cache.
    auto verify = owner.stage(patchFor(input, 1, graph.high_water, graph.high_water));
    require(!verify.computationCode() && verify.timingValues() == first_values,
            "direct full failure corrupted the graph used by the next targeted computation");
  }
  for (std::uint64_t generation = 2; generation <= 4; ++generation) {
    graph.generation = timing_edit::TransactionGeneration{generation};
    auto candidate = CudaDualCsrTestReader::stageFullCsr(owner, graph);
    require(!candidate.computationCode(), "repeated direct full failed");
    if (generation >= 3)
      require(candidate.telemetry().cuda.allocations == 0, "direct full reallocated steady device capacity");
    require(owner.publish(std::move(candidate)), "repeated direct full did not publish");
  }
  require(owner.timingValues() == first_values && owner.close(), "direct full changed repeated results or failed cleanup");
}
}  // namespace ista::csr::test
