// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <array>
#include <cmath>
#include "AllocationFailureSupport.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CudaDualCsrInternal.cuh"
#include "CudaDualCsrTestReader.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr {
namespace {
auto queryWork(const CsrCudaTelemetry& value)
{
  return std::array{value.h2d_bytes, value.d2h_bytes, value.d2d_bytes,
                    value.allocations, value.kernel_launches, value.stream_waits};
}
void requireScalars(const std::vector<value_detail::NodeTimingScalars>& values, const CsrTimingValues& full)
{
  test::requireCpuReference(values.size() == full.nodes.arrivals.values.size(), "scalar query lost nodes");
  for (std::size_t node = 0; node < values.size(); ++node)
    test::requireCpuReference(values[node].arrival == full.nodes.arrivals.values[node]
        && values[node].required == full.nodes.requireds.values[node] && values[node].slack == full.nodes.slacks.values[node],
        "scalar query differs from complete values");
}
}  // namespace

void CudaDualCsrTestReader::scalarQueryContracts(CudaLevelizationMode mode)
{
  using namespace cuda_detail;
  using test::requireCpuReference;
  const auto input = test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  const std::array ids{NodeId{1}, NodeId{0}};
  const auto independent = [&](const CsrTimingValues& full) {
    if constexpr (incregpusta_debug_checks_enabled) {
      const auto oracle = CsrCpuReferenceBackend{}.compute(FlatCsrEdgeView(input), FlatTimingPayloadView(input));
      const auto* expected = std::get_if<CsrTimingResult>(&oracle.outcome);
      requireCpuReference(expected && full == static_cast<const CsrTimingValues&>(*expected),
                          "scalar/report/full sequence differs from independent CPU");
    }
  };
  for (std::size_t cached = 0; cached <= ids.size(); ++cached) {
    CudaDualCsrSession owner(input, water);
    static_cast<void>(owner.reportValues(std::span(ids).first(cached), {}));
    const auto before = owner.telemetry().cuda;
    const auto& scalars = owner.timingScalars();
    requireCpuReference(!owner._impl->values && owner._impl->report_values.nodes.size() == cached
        && owner.telemetry().cuda.result_d2h_bytes - before.result_d2h_bytes
            == (ids.size() - cached) * sizeof(value_detail::NodeTimingScalars),
        "scalar query downloaded non-scalar or already cached fields");
    if (cached == ids.size()) requireCpuReference(queryWork(owner.telemetry().cuda) == queryWork(before),
        "fully cached report nodes did not satisfy scalar query without device work");
    const auto after = owner.telemetry().cuda;
    requireCpuReference(&owner.timingScalars() == &scalars && queryWork(owner.telemetry().cuda) == queryWork(after),
                        "repeated scalar query performed device work");
    const auto& sparse = owner.reportValues(ids, {});
    requireCpuReference(!sparse.complete && !owner._impl->values && sparse.nodes.size() == ids.size()
        && owner.telemetry().cuda.result_d2h_bytes - after.result_d2h_bytes
            == (ids.size() - cached) * sizeof(value_detail::NodeDetails),
        "report query reread existing scalar fields or omitted node details");
    for (const auto& [id, node] : sparse.nodes)
      requireCpuReference(node.timing.arrival == scalars[id].arrival && node.timing.required == scalars[id].required
          && node.timing.slack == scalars[id].slack, "report query did not assemble its cached scalars");
    const auto full_before = owner.telemetry().cuda;
    const auto& state = *owner._impl;
    const auto missing = state.storage->domain.host.live_tags * sizeof(value_detail::PathState)
        + state.view.arc_count * sizeof(ArcState) + state.view.child_state_count * sizeof(ChildState)
        + (incregpusta_debug_checks_enabled ? 0 : state.view.node_count * sizeof(Index));
    const auto& full = owner.timingValues();
    requireCpuReference(owner.telemetry().cuda.result_d2h_bytes - full_before.result_d2h_bytes == missing,
                        "full query reread node fields after report/scalar delivery");
    requireScalars(scalars, full);
    independent(full);
  }
  {
    CudaDualCsrSession owner(input, water);
    const auto& scalars = owner.timingScalars();
    const auto before = owner.telemetry().cuda;
    const auto& state = *owner._impl;
    const auto missing = state.view.node_count * sizeof(value_detail::NodeDetails)
        + state.storage->domain.host.live_tags * sizeof(value_detail::PathState)
        + state.view.arc_count * sizeof(ArcState) + state.view.child_state_count * sizeof(ChildState)
        + (incregpusta_debug_checks_enabled ? 0 : state.view.node_count * sizeof(Index));
    const auto& full = owner.timingValues();
    requireCpuReference(owner.telemetry().cuda.result_d2h_bytes - before.result_d2h_bytes == missing,
                        "scalar-to-full upgrade reread scalar fields");
    requireScalars(scalars, full);
    independent(full);
  }
  {
    CudaDualCsrSession owner(input, water);
    const auto& full = owner.timingValues();
    const auto before = owner.telemetry().cuda;
    requireScalars(owner.timingScalars(), full);
    requireCpuReference(queryWork(owner.telemetry().cuda) == queryWork(before), "full-to-scalar query performed device work");
  }
  {
    CudaDualCsrSession empty(test::makeCpuReferenceDagFixture(0, {}), {0, 0, 0});
    const auto before = empty.telemetry().cuda;
    requireCpuReference(empty.timingScalars().empty() && empty._impl->timing_scalars.has_value()
        && empty.timingScalars().empty() && queryWork(empty.telemetry().cuda) == queryWork(before),
        "empty scalar query did not cache without device work");
  }
  {
    CudaDualCsrSession owner(input, water);
    const auto& saved = owner.timingScalars();
    const auto& arc = input.arcs.front();
    ArcSemanticPayload expected{arc.key, arc.source, arc.sink, arc.kind, arc.is_clock_arc, arc.is_disabled,
        arc.is_loop_disabled, arc.eligibility, arc.transfer, arc.output_load, arc.native_cell_arc_ordinal};
    auto replacement = expected;
    replacement.eligibility.data_arrival = false;
    auto patch = test::patchFor(input, 0, water, water);
    patch.payload_changes = {{arc.id, expected, replacement}};
    {
      auto abandoned = owner.stage(patch, mode);
      bool rejected = false;
      try { static_cast<void>(owner.timingScalars()); } catch (const std::logic_error&) { rejected = true; }
      requireCpuReference(rejected && !abandoned.computationCode(), "pending scalar query exposed old results");
    }
    requireCpuReference(&owner.timingScalars() == &saved && saved[1].arrival == 1.0,
                        "abandoned transaction lost the committed scalar cache");
    auto candidate = owner.stage(patch, mode);
    requireCpuReference(owner.publish(std::move(candidate)) && !owner._impl->timing_scalars,
                        "commit preserved old scalar cache");
    const auto& changed = owner.timingScalars();
    requireCpuReference(std::isinf(changed[1].arrival) && changed[1].arrival < 0.0, "new scalar query returned stale timing");
    requireScalars(changed, owner.timingValues());
  }
  if constexpr (incregpusta_debug_checks_enabled) {
    for (const bool device : {false, true})
      for (unsigned scenario = 0; scenario != 5; ++scenario) {
        const auto prepare = [&] {
          CudaDualCsrSession owner(input, water);
          if (scenario == 1) static_cast<void>(owner.reportValues(std::span(ids).first(1), {}));
          if (scenario == 2 || scenario == 3) static_cast<void>(owner.timingScalars());
          if (scenario == 4) static_cast<void>(owner.timingValues());
          return owner;
        };
        const auto query = [&](CudaDualCsrSession& owner) {
          if (scenario == 2) static_cast<void>(owner.reportValues(ids, {}));
          else if (scenario == 3) static_cast<void>(owner.timingValues());
          else static_cast<void>(owner.timingScalars());
        };
        std::size_t allocations;
        {
          auto owner = prepare();
          if (device) ista_test_cuda_allocation::armCountOnly(); else ista_test_allocation::armCountOnly();
          query(owner);
          allocations = device ? ista_test_cuda_allocation::disable() : ista_test_allocation::disable();
        }
        if (device && (scenario == 1 || scenario == 4))
          requireCpuReference(allocations == 0, "scalar query did not reuse existing projection capacity");
        else requireCpuReference(allocations > 0, "scalar allocation injection lacks positive calibration");
        for (std::size_t ordinal = 0; ordinal < allocations; ++ordinal) {
          auto owner = prepare();
          if (device) ista_test_cuda_allocation::armFailure(ordinal); else ista_test_allocation::armFailure(ordinal);
          bool failed = false;
          try { query(owner); }
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
          const auto& state = *owner._impl;
          requireCpuReference(failed && injected && !owner.poisoned()
              && state.timing_scalars.has_value() == (scenario == 2 || scenario == 3)
              && state.values.has_value() == (scenario == 4)
              && state.report_values.nodes.size() == (scenario == 1 ? 1U : 0U) && state.report_values.paths.empty(),
              "failed scalar query changed or poisoned committed cache at " + std::to_string(ordinal));
          query(owner);
          requireScalars(owner.timingScalars(), owner.timingValues());
          independent(owner.timingValues());
        }
      }
  }
}
}  // namespace ista::csr
