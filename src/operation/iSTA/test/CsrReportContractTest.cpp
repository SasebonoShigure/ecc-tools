// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrTimingScatterTestFixture.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "TimingResultScatterInternal.hpp"
#include "BenchmarkTimingConsumer.hpp"
#include "BufferTraceReplayInput.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "ProductionFreshSessionLoader.hpp"
#include "StaProductionLifecycle.hpp"
#include "DelayCalculator.hpp"
#include "TimingAnalyzer.hpp"
#include <fstream>
#include <iostream>

namespace ista::csr::test {
void compareReport(const timing_edit::oracle::ProductionFreshSessionView& view, const char* output)
{
  const auto& input = view.reference_input;
  auto& database = view.database;
  requireTimingScatter(database.get_timing_path_group_list().empty(),
                       "timing-only baseline prepared report groups without a consumer");
  const auto propagated = readNativeTimingOracle(database);
  const auto prepared = executeStaLifecyclePipeline(
      {{"Prepare native report", [] { TimingAnalyzer::initInst(); }, [] { STATA.analyzePropagatedTiming(); },
        [] { TimingAnalyzer::destroyInst(); }}},
      StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
  requireTimingScatter(prepared.succeeded, prepared.message);
  requireNativeTimingOracleEqual(readNativeTimingOracle(database), propagated, "report preparation changed propagated timing");
  const auto values = materializeTimingResultFromNativeOracle(input, propagated);
  const auto expected = TimingReporterProbeAccess::read();
  if (output) {
    std::ofstream file;
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.open(output);
    file << expected;
  }
  requireTimingScatter(!expected.at("paths").at("paths").empty(), "native report fixture contains no selected path");
  detail::StableScatterTargets targets;
  targets.timing_order = &database.get_timing_order_list();
  for (const auto& node : input.nodes)
    targets.nodes.push_back({&database.get_timing_point_map().at(node.key.full_name), &node.key.full_name});
  for (const auto& arc : input.arcs)
    targets.arcs.push_back({&database.get_arc_list().at(arc.native_traversal_rank), static_cast<std::size_t>(arc.native_traversal_rank)});
  const detail::TimingScatterLayout layout{input.analysis_order, input.path_source_order, input.transition_order, input.clock_tags};
  for (auto& [name, point] : database.get_timing_point_map()) {
    static_cast<void>(name);
    point = TimingPoint{};
    point.set_arrival(-1234.0);
    point.set_required(4321.0);
    point.set_slack(5555.0);
  }
  for (auto& arc : database.get_arc_list()) {
    arc.set_delay(-999.0); arc.set_delay_min(-999.0); arc.set_delay_max(-999.0);
    arc.get_graph_delay_map().clear(); arc.get_input_output_delay_map().clear();
  }
  const auto before = readNativeTimingOracle(database);
  const auto native_report_before = TimingReporterProbeAccess::read();
  const auto status = executeStaLifecyclePipeline(
      {{"CSR report contract", [] {}, [&] {
        for (int repeat = 0; repeat != 2; ++repeat) {
          detail::CsrReportNodeReader reader(targets, layout, values);
          const auto summary = TimingReporter::readCsrReport(reader, true);
          requireTimingScatter(nlohmann::json::parse(summary.first) == expected.at("qor") && summary.second.empty(),
                               "summary-only query changed QOR or generated paths");
          requireNativeTimingOracleEqual(readNativeTimingOracle(database), before, "summary-only report mutated native timing");
          const auto [qor, paths] = TimingReporter::readCsrReport(reader);
          const nlohmann::json actual{{"qor", nlohmann::json::parse(qor)}, {"paths", nlohmann::json::parse(paths)}};
          requireTimingScatter(actual == expected, "CSR report differs from native report");
          requireNativeTimingOracleEqual(readNativeTimingOracle(database), before, "CSR report mutated native timing");
          requireTimingScatter(TimingReporterProbeAccess::read() == native_report_before, "CSR report replaced native report groups");
        }
      }, [] {}}},
      StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
  requireTimingScatter(status.succeeded, status.message);
}
}  // namespace ista::csr::test

int main(int argc, char** argv)
{
  try {
    if (argc != 2 && !(argc == 4 && std::string_view(argv[2]) == "--report-output"))
      throw std::invalid_argument("expected replay trace and optional --report-output path");
    const auto program = ista::buffer_trace::test::readReplayInput(argv[1], false);
    ista::timing_dual_csr::test::DualCsrTargetedArtifactSupport artifacts(program);
    ista::timing_edit::oracle::ProductionFreshSessionLoader::run(artifacts.artifacts(), [&](const auto& view) {
      ista::csr::test::compareReport(view, argc == 4 ? argv[3] : nullptr);
    });
    artifacts.close();
    std::cout << "CSR full/summary/repeated report contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
