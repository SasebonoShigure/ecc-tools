// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <array>
#include <set>
#include "AllocationFailureSupport.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrValidatorTestFixture.hpp"
#include "CudaDualCsrInternal.cuh"
#include "CudaDualCsrTestReader.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr {
void CudaDualCsrTestReader::reportChainContracts()
{
  using namespace cuda_detail;
  using test::requireCpuReference;
  // Component seeds exercise path propagation; strict native inputs use the production trace tests.
  const auto withPaths = [](CsrReferenceInput input) {
    for (auto& seed : input.start_seeds) {
      seed.path_source = PathSource::kInput;
      for (auto& lane : seed.arrival) for (auto& value : lane) value = 0.0;
    }
    test::refreshCpuReferenceIdentity(input);
    return input;
  };
  const auto input = withPaths(test::makeCpuReferenceDagFixture(7, {{0, 1}, {1, 2}, {2, 3}, {4, 5}, {5, 6}}));
  const std::array endpoints{NodeId{3}, NodeId{3}};
  const std::array chains{
      CsrPathChainSeed{NodeId{3}, AnalysisMode::kMax, PathSource::kInput, Transition::kRise, ClockTagId{0}},
      CsrPathChainSeed{NodeId{3}, AnalysisMode::kMax, PathSource::kInput, Transition::kRise, ClockTagId{0}},
      CsrPathChainSeed{NodeId{6}, AnalysisMode::kMax, PathSource::kInput, Transition::kRise, ClockTagId{999}}};
  const std::array clocks{CsrClockChainSeed{NodeId{5}, AnalysisMode::kMin, Transition::kNone}};
  const CsrReportRequest request{{}, endpoints, clocks, chains};
  const auto query = [&](CudaDualCsrSession& owner) {
    const auto keys = owner.prepareReport(request);
    const auto& cache = owner._impl->report_values;
    std::set<Index> ids;
    for (const auto& [id, value] : cache.nodes) { (void) value; ids.insert(id); }
    requireCpuReference(ids == std::set<Index>{0, 1, 2, 3, 5, 6} && !cache.complete && !owner._impl->values,
                        "report closure omitted predecessors or materialized unrelated nodes");
    std::set<Index> path_nodes;
    for (const auto key : keys) {
      requireCpuReference(key.tag == 0 && cache.paths.at(key.slot).present,
                          "report closure returned a missing tag or invalid path");
      path_nodes.insert(key.row / 8);
    }
    requireCpuReference(path_nodes == std::set<Index>{0, 1, 2, 3}, "report closure missed a fixed-tag predecessor");
    return keys;
  };
  for (const unsigned threads : {64U, 256U}) {
    auto owner = computeRc(input, threads);
    static_cast<void>(owner.prepareReport({{}, endpoints, {}, {}}));
    const auto partial = owner.telemetry().cuda;
    static_cast<void>(query(owner));
    const auto missing_values = 5 * sizeof(value_detail::NodeValue) + 3 * sizeof(value_detail::PathState);
    const auto closure_metadata = sizeof(Index) + sizeof(Status) + 6 * sizeof(ReportChainStep);
    requireCpuReference(owner.telemetry().cuda.result_d2h_bytes - partial.result_d2h_bytes
                            == missing_values + closure_metadata,
                        "partial report chain reread its cached endpoint values");
    const auto before = owner.telemetry().cuda;
    static_cast<void>(query(owner));
    requireCpuReference(owner.telemetry().cuda.h2d_bytes == before.h2d_bytes
        && owner.telemetry().cuda.d2h_bytes == before.d2h_bytes
        && owner.telemetry().cuda.kernel_launches == before.kernel_launches
        && owner.telemetry().cuda.stream_waits == before.stream_waits,
        "cached report chains repeated device work");
    const auto& full = owner.timingValues();
    requireCpuReference(full.nodes.arrivals.values[3] == 3.0 && full.nodes.arrivals.values[6] == 2.0,
                        "report closure changed committed results");
    const auto full_before = owner.telemetry().cuda;
    requireCpuReference(owner.prepareReport(request).empty()
        && owner.telemetry().cuda.d2h_bytes == full_before.d2h_bytes
        && owner.telemetry().cuda.stream_waits == full_before.stream_waits,
        "complete host results did not satisfy report preparation");
    if constexpr (incregpusta_debug_checks_enabled) {
      const auto oracle = CsrCpuReferenceBackend{}.compute(FlatCsrEdgeView(input), FlatTimingPayloadView(input));
      const auto* expected = std::get_if<CsrTimingResult>(&oracle.outcome);
      requireCpuReference(expected && full == static_cast<const CsrTimingValues&>(*expected),
                          "chain preparation/full upgrade differs from independent CPU");
    }
  }

  // More roots than a block and overlapping deep paths exercise scan/fill ownership.
  std::vector<test::DagArc> edges;
  std::vector<CsrPathChainSeed> roots;
  for (Index node = 1; node < 130; ++node) edges.push_back({node - 1, node});
  const auto deep = withPaths(test::makeCpuReferenceDagFixture(130, edges));
  for (Index node = 0; node < 130; ++node)
    roots.push_back({NodeId{node}, AnalysisMode::kMax, PathSource::kInput, Transition::kRise, ClockTagId{0}});
  for (const unsigned threads : {64U, 256U}) {
    auto owner = computeRc(deep, threads);
    static_cast<void>(owner.prepareReport({{}, {}, {}, roots}));
    const auto& cache = owner._impl->report_values;
    requireCpuReference(cache.nodes.size() == 130 && cache.paths.size() == 130 && !owner._impl->values,
                        "cross-block report roots lost or duplicated cached identities");
    for (Index node = 0; node < 130; ++node)
      requireCpuReference(cache.nodes.at(node).timing.arrival == static_cast<double>(node), "deep report result differs");
  }

  for (const bool propagated : {false, true}) {
    auto clock_input = test::makeCpuReferenceCellFixture();
    clock_input.clocks = {{"clk", "clk", {{clock_input.nodes[0].key.full_name, NodeId{0}},
                                          {clock_input.nodes[1].key.full_name, NodeId{1}}}, 10.0, {0.0, 5.0}, propagated}};
    clock_input.clock_owner_by_node.assign(clock_input.nodes.size(), std::string{"clk"});
    clock_input.arcs[1].eligibility.clock = true;
    test::refreshCpuReferenceIdentity(clock_input);
    auto owner = computeRc(clock_input, 64);
    const std::array clock_roots{CsrClockChainSeed{NodeId{2}, AnalysisMode::kMax, Transition::kRise}};
    static_cast<void>(owner.prepareReport({{}, {}, clock_roots, {}}));
    const auto& cache = owner._impl->report_values;
    requireCpuReference(cache.nodes.contains(2) && cache.nodes.contains(1) == propagated && cache.nodes.size() == (propagated ? 2U : 1U),
                        "clock report followed physical predecessors for an ideal clock or omitted propagated predecessors");
  }

  if constexpr (incregpusta_debug_checks_enabled) {
    {
      auto owner = computeRc(input, 64);
      auto& state = *owner._impl;
      const auto row = state.storage->domain.host.rows[3 * 8];
      requireCpuReference(row.begin < row.end, "absent-path injection needs a real structural slot");
      PathState path;
      state.memory.download(state.view.paths + row.begin, &path, 1);
      state.memory.synchronize();
      requireCpuReference(path.present, "absent-path injection lacks a positive control");
      path.present = false;
      state.memory.upload(&path, state.view.paths + row.begin, 1);
      state.memory.synchronize();
      const std::array root{chains.front()};
      const auto keys = owner.prepareReport({{}, {}, {}, root});
      requireCpuReference(keys.size() == 1 && keys.front().slot == row.begin
          && state.report_values.nodes.size() == 1 && state.report_values.paths.size() == 1
          && !state.report_values.paths.at(row.begin).present,
          "valid absent path was followed or confused with a missing tag");
      const auto before = owner.telemetry().cuda;
      static_cast<void>(owner.prepareReport({{}, {}, {}, root}));
      requireCpuReference(owner.telemetry().cuda.d2h_bytes == before.d2h_bytes
          && owner.telemetry().cuda.h2d_bytes == before.h2d_bytes
          && owner.telemetry().cuda.kernel_launches == before.kernel_launches
          && owner.telemetry().cuda.stream_waits == before.stream_waits,
          "cached absent path repeated device work");
    }
    for (const bool device : {false, true}) {
      std::size_t allocations;
      {
        auto owner = computeRc(input, 64);
        if (device) ista_test_cuda_allocation::armCountOnly(); else ista_test_allocation::armCountOnly();
        static_cast<void>(owner.prepareReport(request));
        allocations = device ? ista_test_cuda_allocation::disable() : ista_test_allocation::disable();
      }
      requireCpuReference(allocations > 0, "chain allocation failures lack positive calibration");
      for (std::size_t ordinal = 0; ordinal < allocations; ++ordinal) {
        auto owner = computeRc(input, 64);
        if (device) ista_test_cuda_allocation::armFailure(ordinal); else ista_test_allocation::armFailure(ordinal);
        bool failed = false;
        try { static_cast<void>(owner.prepareReport(request)); }
        catch (const std::bad_alloc&) { failed = !device; }
        catch (const CudaAllocationError&) { failed = device; }
        catch (...) {
          static_cast<void>(ista_test_cuda_allocation::disable());
          static_cast<void>(ista_test_allocation::disable());
          throw;
        }
        const bool injected = device ? ista_test_cuda_allocation::failureInjected() : ista_test_allocation::failureInjected();
        static_cast<void>(ista_test_cuda_allocation::disable());
        static_cast<void>(ista_test_allocation::disable());
        requireCpuReference(failed && injected && !owner.poisoned() && owner._impl->report_values.nodes.empty()
            && owner._impl->report_values.paths.empty() && !owner._impl->values,
            "failed chain allocation poisoned or published partial values at " + std::to_string(ordinal));
        static_cast<void>(query(owner));
      }
    }
  }
}
}  // namespace ista::csr
