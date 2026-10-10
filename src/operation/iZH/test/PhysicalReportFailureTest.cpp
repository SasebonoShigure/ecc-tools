// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalSessionTestFixture.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "BenchmarkTimingConsumer.hpp"
#include "AllocationFailureSupport.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "DataManager.hpp"

namespace ista {
void physicalFirstReportFailure()
{
  using namespace timing_edit;
  using namespace timing_edit::test;
  using physical_test::require;
  std::optional<PhysicalStaCurrentResult> expected;
  for (const bool inject : {false, true}) {
    if (inject && !incregpusta_debug_checks_enabled) continue;
    NativeTransactionRuntime runtime;
    installNativeTransactionBaseline();
    physical_test::constrainPorts(STADM.getDatabase());
    physical_test::PhysicalDesign physical;
    physical_test::RecoveryDirectory recovery;
    auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
    GuardedTestSession guarded({StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError}, analysis);
    auto& session = guarded.session();
    const auto baseline = session.runPhysicalBaseline(recovery.config());
    if (!baseline.ok()) throw std::runtime_error(baseline.status.message);
    auto& database = STADM.getDatabase();
    require(database.get_timing_path_group_list().empty(), "native physical baseline eagerly generated report groups");
    // Controlled analyzer input: a parallel diversion can improve a report path,
    // while propagated timing remains authoritative. This reaches the report's
    // local path mutation, including allocations after the first arrival write.
    const auto prepared = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session,
        [&](std::uint64_t, StaCsrSessionContext&, const NativeExecutionAuthority&) {
      auto arc = database.get_arc_list().front();
      for (auto* delays : {&arc.get_graph_delay_map(), &arc.get_input_output_delay_map()})
        for (auto& [type, inputs] : *delays) {
          (void) type;
          for (auto& [input, outputs] : inputs) {
            (void) input;
            for (auto& [output, delay] : outputs) { (void) output; delay += 1.0; }
          }
        }
      arc.set_delay_max(arc.get_delay_max() + 1.0);
      const auto rank = database.get_arc_list().size();
      database.get_outgoing_arc_list_map().at(arc.get_source_pin()).push_back(rank);
      database.get_incoming_arc_list_map().at(arc.get_sink_pin()).push_back(rank);
      database.get_arc_list().push_back(std::move(arc));
      return StaStatus::success(session.state());
    });
    if (!prepared.ok()) throw std::runtime_error(prepared.message);
    const auto old_timing = csr::test::readNativeTimingOracle(database);
    const auto old_report = TimingReporterProbeAccess::read();
    if (inject) {
      auto& point = database.get_timing_point_map().at("out");
      struct Failure {
        std::vector<std::pair<const TimingPathState*, double>> paths;
        bool fired{false};
      } failure;
      for (const auto& [transition, row] : point.get_path_state_map().at(AnalysisType::kMax).at(PathSourceType::kInput)) {
        (void) transition;
        for (const auto& [tag, path] : row) {
          (void) tag;
          failure.paths.emplace_back(&path, path.get_arrival());
        }
      }
      ista_test_allocation::armFailureWhen([](void* opaque) noexcept {
        auto& value = *static_cast<Failure*>(opaque);
        for (const auto& [path, arrival] : value.paths)
          if (path->get_arrival() != arrival) return value.fired = true;
        return false;
      }, &failure);
      bool rejected = false;
      try { (void) session.readPhysicalResult(); }
      catch (const std::exception&) { rejected = true; }
      catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
      static_cast<void>(ista_test_allocation::disable());
      require(failure.fired, "first native report did not mutate a watched path before allocation");
      require(rejected, "first native report accepted its injected allocation failure");
      require(session.state() == StaSessionState::kBaselineExecuted, "first native report failure changed session state");
      require(csr::test::readNativeTimingOracle(database) == old_timing && TimingReporterProbeAccess::read() == old_report,
              "failed first native report changed authoritative timing/paths or published partial report");
    }
    const auto current = session.readPhysicalResult();
    require(csr::test::readNativeTimingOracle(database) == old_timing,
            "successful report changed authoritative timing paths");
    require(!database.get_timing_path_group_list().empty(), "explicit native report generated no path groups");
    if (expected)
      require(current.generation == expected->generation && current.timing == expected->timing
                  && current.qor_json == expected->qor_json && current.paths_json == expected->paths_json,
              "first native report retry differs from an independent successful preparation");
    else expected = current;
    const auto cached = session.readPhysicalResult();
    require(cached.timing == current.timing && cached.qor_json == current.qor_json && cached.paths_json == current.paths_json,
            "repeated native report changed the current generation");
    require(guarded.closeAndRelease().ok(), "native report retry close failed");
  }
}
}  // namespace ista
