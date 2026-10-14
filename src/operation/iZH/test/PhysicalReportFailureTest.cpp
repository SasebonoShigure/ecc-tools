// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingAnalyzer.hpp"
#include "PhysicalSessionTestFixture.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "BenchmarkTimingConsumer.hpp"
#include "AllocationFailureSupport.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "DataManager.hpp"
#include <array>

namespace ista {
namespace {
void groupedReportSelection()
{
  timing_edit::test::NativeTransactionRuntime runtime;
  timing_edit::test::installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  database.get_timing_path_group_list().clear();
  auto& group = database.get_timing_path_group_list().emplace_back();
  group.set_group_name("selection");
  STADM.getConfig().timing_path_limit = 20;
  const auto append = [&](const char* end, const char* start, AnalysisType analysis, double slack) {
    auto& row = group.get_timing_path_end_map()[end];
    row.set_end_point(end);
    auto& path = row.get_timing_path_list().emplace_back();
    path.set_end_point(end);
    path.set_start_point(start);
    path.set_analysis_type(analysis);
    path.set_slack(slack);
  };
  // The first equal-slack candidate must survive; setup and hold select independently.
  append("b", "b_setup", AnalysisType::kMax, -2.0);
  append("b", "b_hold", AnalysisType::kMin, 0.25);
  append("a", "a_worse", AnalysisType::kMax, -1.0);
  append("a", "a_first", AnalysisType::kMax, -3.0);
  append("a", "a_equal", AnalysisType::kMax, -3.0);
  append("a", "a_hold_worse", AnalysisType::kMin, -0.1);
  append("a", "a_hold_first", AnalysisType::kMin, -0.5);
  append("a", "a_hold_equal", AnalysisType::kMin, -0.5);
  group.get_timing_path_end_map()["empty"].set_end_point("empty");
  const auto result = TimingReporterProbeAccess::read();
  const auto& summary = result.at("qor").at("summary");
  physical_test::require(summary.at("setup").at("wns") == -3.0 && summary.at("setup").at("tns") == -5.0
      && summary.at("setup").at("nvp") == 2 && summary.at("hold").at("wns") == -0.5
      && summary.at("hold").at("tns") == -0.5 && summary.at("hold").at("nvp") == 1,
      "grouped endpoint selection changed full QOR");
  nlohmann::json selected = nlohmann::json::array();
  for (const auto& path : result.at("paths").at("paths"))
    selected.push_back({path.at("analysis_type"), path.at("end_point"), path.at("start_point"), path.at("slack_ns")});
  const nlohmann::json expected = {{"setup", "a", "a_first", -3.0}, {"setup", "b", "b_setup", -2.0},
                                 {"hold", "a", "a_hold_first", -0.5}, {"hold", "b", "b_hold", 0.25}};
  physical_test::require(selected == expected, "grouped endpoint selection changed ordering or first equal-slack winner");
  physical_test::require(TimingReporterProbeAccess::read() == result, "repeated report mutated its endpoint groups");
}
void deferredTraceRetry()
{
  using namespace timing_edit;
  using namespace timing_edit::test;
  using physical_test::require;
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
  const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session,
      [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
    auto& database = STADM.getDatabase();
    StaTimingEditSessionProbeAccess::physicalState(context).prepareReport(database, context, authority);
    TimingPath* selected = nullptr;
    for (auto& group : database.get_timing_path_group_list())
      for (auto& [name, end] : group.get_timing_path_end_map()) {
        (void) name;
        for (auto& path : end.get_timing_path_list()) {
          require(path.get_point_list().empty(), "report grouping eagerly expanded a path trace");
          if (!selected) selected = &path;
        }
      }
    require(selected != nullptr, "trace fixture has no constrained path");
    const auto slack = selected->get_slack();
    if constexpr (incregpusta_debug_checks_enabled) {
      ista_test_allocation::armFailure(0);
      bool rejected = false;
      try { TimingAnalyzer::materializePathTrace(*selected); }
      catch (const std::bad_alloc&) { rejected = true; }
      catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
      const bool injected = ista_test_allocation::failureInjected();
      static_cast<void>(ista_test_allocation::disable());
      require(injected && rejected && selected->get_point_list().empty() && selected->get_slack() == slack,
              "failed trace preparation published a partial path");
    }
    TimingAnalyzer::materializePathTrace(*selected);
    require(!selected->get_point_list().empty() && selected->get_slack() == slack,
            "trace retry lost its points or changed the endpoint result");
    const auto* points = selected->get_point_list().data();
    if constexpr (incregpusta_debug_checks_enabled) ista_test_allocation::armFailure(0);
    try { TimingAnalyzer::materializePathTrace(*selected); }
    catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
    const bool injected = ista_test_allocation::failureInjected();
    static_cast<void>(ista_test_allocation::disable());
    require(!injected && points == selected->get_point_list().data(), "cached trace was rebuilt");
    return StaStatus::success(session.state());
  });
  if (!status.ok()) throw std::runtime_error(status.message);
  require(guarded.closeAndRelease().ok(), "deferred trace fixture close failed");
}
}  // namespace

void physicalFirstReportFailure()
{
  groupedReportSelection();
  deferredTraceRetry();
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
    const auto old_timing = csr::test::readNativeTimingOracle(database);
    const auto old_report = TimingReporterProbeAccess::read();
    if (inject) {
      // A real first-query allocation failure must leave the last complete result intact.
      ista_test_allocation::armFailure(0);
      bool rejected = false;
      try { (void) session.readPhysicalResult(); }
      catch (const std::exception&) { rejected = true; }
      catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
      const bool injected = ista_test_allocation::failureInjected();
      static_cast<void>(ista_test_allocation::disable());
      require(injected, "first native report did not reach the allocation failure");
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
    // Returned report points own their values independently of mutable native timing.
    const auto published_report = TimingReporterProbeAccess::read();
    const auto independent = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session,
        [&](std::uint64_t, StaCsrSessionContext&, const NativeExecutionAuthority&) {
      struct RestoreScalars {
        std::vector<std::pair<TimingPoint*, std::array<double, 3>>> values;
        ~RestoreScalars() {
          for (const auto& [point, value] : values) {
            point->set_arrival(value[0]); point->set_required(value[1]); point->set_slack(value[2]);
          }
        }
      } restore;
      restore.values.reserve(database.get_timing_point_map().size());
      for (auto& [name, point] : database.get_timing_point_map()) {
        (void) name;
        restore.values.push_back({&point, {point.get_arrival(), point.get_required(), point.get_slack()}});
        point.set_arrival(12345.0); point.set_required(12346.0); point.set_slack(1.0);
      }
      require(TimingReporterProbeAccess::read() == published_report, "published path point borrowed mutable timing scalars");
      return StaStatus::success(session.state());
    });
    if (!independent.ok()) throw std::runtime_error(independent.message);
    const auto cached = session.readPhysicalResult();
    require(cached.timing == current.timing && cached.qor_json == current.qor_json && cached.paths_json == current.paths_json,
            "repeated native report changed the current generation");
    require(guarded.closeAndRelease().ok(), "native report retry close failed");
  }
}
}  // namespace ista
