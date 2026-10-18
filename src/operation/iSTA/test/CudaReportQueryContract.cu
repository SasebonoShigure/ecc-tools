#include <array>
#include <cmath>
#include <string>
#include <variant>

#include "AllocationFailureSupport.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CudaDualCsrInternal.cuh"
#include "CudaDualCsrTestReader.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr {
void CudaDualCsrTestReader::reportQueryContracts(CudaLevelizationMode mode)
{
  using namespace cuda_detail;
  using csr::test::requireCpuReference;
  const auto input = csr::test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  const std::array nodes{NodeId{1}, NodeId{1}};
  const auto pathsFor = [](const CudaDualCsrSession& owner) {
    std::vector<CsrQueryPath> paths;
    const auto& rows = owner._impl->storage->domain.host.rows;
    for (Index row = 0; row < rows.size(); ++row)
      for (Index slot = rows[row].begin; slot < rows[row].end; ++slot)
        paths.push_back({row, slot});
    return paths;
  };
  CudaDualCsrSession owner(input, water);
  auto& state = *owner._impl;
  auto all_paths = pathsFor(owner);
  requireCpuReference(!all_paths.empty(), "report fixture has no path domain");
  const std::array paths{all_paths.back(), all_paths.back()};
  const auto before_empty = owner.telemetry().cuda;
  const auto& empty = owner.reportValues({}, {});
  requireCpuReference(empty.nodes.empty() && empty.paths.empty() && !empty.complete && !state.values
      && owner.telemetry().cuda.d2h_bytes == before_empty.d2h_bytes
      && owner.telemetry().cuda.stream_waits == before_empty.stream_waits,
      "empty report query performed delivery or materialization");
  const auto& sparse = owner.reportValues(nodes, paths);
  const auto& node = sparse.nodes.at(1);
  const auto& path = sparse.paths.at(paths.front().slot);
  requireCpuReference(!sparse.complete && !state.values && sparse.nodes.size() == 1 && sparse.paths.size() == 1
      && node.level == 2 && node.timing.arrival == 1.0 && node.timing.predecessor_node == 0
      && path.present && path.arrival == 1.0 && path.start == 0 && path.predecessor_node == 0,
      "sparse report did not preserve timing, identity or deduplicate requests");
  requireCpuReference(owner.telemetry().cuda.result_d2h_bytes == sizeof(value_detail::NodeValue) + sizeof(value_detail::PathState),
      "sparse report read data beyond requested nodes and paths");
  const auto cached = owner.telemetry().cuda;
  static_cast<void>(owner.reportValues(nodes, paths));
  requireCpuReference(owner.telemetry().cuda.d2h_bytes == cached.d2h_bytes
      && owner.telemetry().cuda.h2d_bytes == cached.h2d_bytes
      && owner.telemetry().cuda.allocations == cached.allocations
      && owner.telemetry().cuda.kernel_launches == cached.kernel_launches
      && owner.telemetry().cuda.stream_waits == cached.stream_waits,
      "repeated report query did not reuse its generation cache");

  const auto checkFullUpgrade = [&](CudaDualCsrSession& current) {
    const auto& impl = *current._impl;
    const auto missing_bytes = (impl.view.node_count - impl.report_values.nodes.size()) * sizeof(value_detail::NodeValue)
        + (impl.storage->domain.host.live_tags - impl.report_values.paths.size()) * sizeof(value_detail::PathState)
        + impl.view.arc_count * sizeof(ArcState) + impl.view.child_state_count * sizeof(ChildState)
        + (incregpusta_debug_checks_enabled ? 0 : impl.view.node_count * sizeof(Index));
    const auto before = current.telemetry().cuda;
    const auto& full = current.timingValues();
    requireCpuReference(current.telemetry().cuda.result_d2h_bytes - before.result_d2h_bytes == missing_bytes
        && current.telemetry().cuda.stream_waits == before.stream_waits + 1,
        "partial-to-full query reread cached values or split its delivery wait");
    const auto after = current.telemetry().cuda;
    const auto& reused = current.reportValues(nodes, pathsFor(current));
    requireCpuReference(reused.complete == &full && reused.nodes.empty() && reused.paths.empty()
        && current.telemetry().cuda.d2h_bytes == after.d2h_bytes
        && current.telemetry().cuda.stream_waits == after.stream_waits,
        "report query ignored existing complete host timing");
    if constexpr (incregpusta_debug_checks_enabled) {
      const auto oracle = CsrCpuReferenceBackend{}.compute(FlatCsrEdgeView(input), FlatTimingPayloadView(input));
      const auto* expected = std::get_if<CsrTimingResult>(&oracle.outcome);
      requireCpuReference(expected != nullptr, "report independent CPU oracle failed");
      CsrTimingResult actual;
      static_cast<CsrTimingValues&>(actual) = full;
      actual.schema_version = expected->schema_version;
      actual.source_identity = expected->source_identity;
      std::string mismatch;
      requireCpuReference(csr::test::exactTimingResultEqual(actual, *expected, &mismatch), "report upgrade differs from CPU: " + mismatch);
    }
  };
  checkFullUpgrade(owner);
  CudaDualCsrSession all_cached(input, water);
  const std::array all_nodes{NodeId{0}, NodeId{1}};
  static_cast<void>(all_cached.reportValues(all_nodes, pathsFor(all_cached)));
  checkFullUpgrade(all_cached);

  // A real eligibility edit changes the result and its live path domain.
  const auto& arc = input.arcs.front();
  ArcSemanticPayload expected{arc.key, arc.source, arc.sink, arc.kind, arc.is_clock_arc, arc.is_disabled,
      arc.is_loop_disabled, arc.eligibility, arc.transfer, arc.output_load, arc.native_cell_arc_ordinal};
  auto replacement = expected;
  replacement.eligibility.data_arrival = false;
  auto patch = csr::test::patchFor(input, 0, water, water);
  patch.payload_changes = {{arc.id, expected, replacement}};
  {
    auto abandoned = owner.stage(patch, mode);
    bool rejected = false;
    try { static_cast<void>(owner.reportValues(nodes, {})); }
    catch (const std::logic_error&) { rejected = true; }
    requireCpuReference(rejected && !abandoned.computationCode(), "pending report exposed the old generation");
  }
  requireCpuReference(owner.reportValues(nodes, {}).complete->nodes.arrivals.values[1] == 1.0,
      "abandonment lost committed report cache");
  auto candidate = owner.stage(patch, mode);
  requireCpuReference(owner.publish(std::move(candidate)), "report lifecycle fixture could not commit");
  const auto& changed = owner.reportValues(nodes, pathsFor(owner));
  requireCpuReference(!changed.complete && !owner._impl->values && std::isinf(changed.nodes.at(1).timing.arrival)
      && changed.nodes.at(1).timing.arrival < 0.0, "new generation reused old report timing");
  const auto changed_arrival = changed.nodes.at(1).timing.arrival;
  requireCpuReference(owner.timingValues().nodes.arrivals.values[1] == changed_arrival,
      "full upgrade disagrees after path-domain removal");

  if constexpr (incregpusta_debug_checks_enabled) {
    // Inject actual allocations, including unpack allocations after delivery.
    for (const bool device : {false, true})
      for (const bool full : {false, true}) {
        const auto prepare = [&]() {
          CudaDualCsrSession current(input, water);
          if (full) static_cast<void>(current.reportValues(nodes, paths));
          return current;
        };
        const auto query = [&](CudaDualCsrSession& current) {
          if (full) static_cast<void>(current.timingValues());
          else static_cast<void>(current.reportValues(nodes, paths));
        };
        std::size_t allocations = 0;
        {
          auto current = prepare();
          if (device) ista_test_cuda_allocation::armCountOnly();
          else ista_test_allocation::armCountOnly();
          query(current);
          allocations = device ? ista_test_cuda_allocation::disable() : ista_test_allocation::disable();
        }
        requireCpuReference(allocations > 0, "report allocation injection has no positive calibration");
        for (std::size_t ordinal = 0; ordinal < allocations; ++ordinal) {
          auto current = prepare();
          if (device) ista_test_cuda_allocation::armFailure(ordinal);
          else ista_test_allocation::armFailure(ordinal);
          bool failed = false;
          try { query(current); }
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
          const auto& cache = current._impl->report_values;
          requireCpuReference(failed && injected && !current.poisoned() && !current._impl->values && !cache.complete
              && cache.nodes.size() == (full ? 1U : 0U) && cache.paths.size() == (full ? 1U : 0U),
              "failed report allocation poisoned or partially published timing at " + std::to_string(ordinal));
          if (full) requireCpuReference(cache.nodes.at(1).timing.arrival == 1.0 && cache.paths.at(paths[0].slot).arrival == 1.0,
              "full query failure changed sparse cached values");
          query(current);
          requireCpuReference(!current.poisoned() && current.timingValues().nodes.arrivals.values[1] == 1.0,
              "report allocation failure could not retry");
        }
      }
  }
}
}  // namespace ista::csr
