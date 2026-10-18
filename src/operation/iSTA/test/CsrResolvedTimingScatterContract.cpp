#include <algorithm>
#include <variant>
#include <source_location>
#include <omp.h>

#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrTimingScatterTestFixture.hpp"
#include "Database.hpp"
#include "TimingResultScatterInternal.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "TimingEditResolver.hpp"

namespace ista::csr::test {

namespace {
void runResolvedTimingScatterContract(ReferenceNativeFixtureCase fixture_case)
{
  auto fixture = makeTimingScatterFixture(fixture_case, 0x6180U);
  const auto& input = fixture.committed_input;
  const auto values = materializeTimingResultFromNativeOracle(input, fixture.expected_oracle);
  detail::StableScatterTargets targets;
  targets.timing_order = &fixture.database().get_timing_order_list();
  for (const auto& node : input.nodes)
    targets.nodes.push_back({&fixture.database().get_timing_point_map().at(node.key.full_name), &node.key.full_name});
  for (const auto& arc : input.arcs)
    targets.arcs.push_back({&fixture.database().get_arc_list().at(arc.native_traversal_rank), static_cast<std::size_t>(arc.native_traversal_rank)});
  const detail::TimingScatterLayout layout{input.analysis_order, input.path_source_order, input.transition_order, input.clock_tags};
  poisonTimingScatterFixture(fixture, 73);
  const auto before = readNativeTimingOracle(fixture.database());
  const auto rejected = [&](const CsrTimingValues& bad, const detail::StableScatterTargets& mapping, std::source_location source = std::source_location::current()) {
    auto result = detail::prepareResolvedTimingResult(mapping, layout, bad);
    requireTimingScatter(std::holds_alternative<detail::TimingResultPreparationFailure>(result), "resolved scatter accepted malformed values/mapping at line " + std::to_string(source.line()));
    requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), before, "resolved preparation failure mutated native state");
    return std::get<detail::TimingResultPreparationFailure>(result).code;
  };
  auto bad = static_cast<const CsrTimingValues&>(values);
  bad.paths.arrivals.values.push_back(0.0);
  rejected(bad, targets);
  bad = values;
  bad.effective_clock.slews.presence.clear();
  rejected(bad, targets);
  bad = values;
  bad.arcs.timing_arc_row_offsets.pop_back();
  rejected(bad, targets);

  bad = values;
  bad.arcs.min_delays.presence.front() = 0;
  rejected(bad, targets);
  bad = values;
  bad.arcs.transition_delays.presence.front() = 0;
  rejected(bad, targets);
  bad = values;
  bad.nodes.arrivals.presence.front() = 0;
  rejected(bad, targets);
  bad = values;
  bad.effective_clock.slews.presence.front() = 1 - bad.effective_clock.arrivals.presence.front();
  rejected(bad, targets);
  bad = values;
  bad.physical_clock.predecessor_arc_delays.presence.front() = 1 - bad.physical_clock.predecessor_nodes.presence.front();
  rejected(bad, targets);
  bad = values;
  if (!bad.paths.start_nodes.presence.empty()) {
    bad.paths.start_nodes.presence.front() = 0;
    rejected(bad, targets);
  }

  const auto predecessor = std::ranges::find(values.nodes.predecessor_arcs.presence, std::uint8_t{1});
  if (predecessor != values.nodes.predecessor_arcs.presence.end()) {
    const auto node = static_cast<std::size_t>(predecessor - values.nodes.predecessor_arcs.presence.begin());
    auto inactive = targets;
    inactive.arcs.at(values.nodes.predecessor_arcs.values[node].value()).arc = nullptr;
    rejected(values, inactive);
  }

  bad = values;
  bad.nodes.arrivals.presence.back() = 0;
  auto missing = targets;
  missing.nodes.front().point = nullptr;
  requireTimingScatter(rejected(bad, missing) == CsrScatterCode::kIdMappingMismatch, "parallel preparation changed first-node error");
  bad = values;
  bad.nodes.arrivals.presence.front() = 0;
  missing = targets;
  missing.nodes.back().point = nullptr;
  requireTimingScatter(rejected(bad, missing) == CsrScatterCode::kShapeMismatch, "parallel preparation changed error precedence");

  const auto resolver = timing_edit::TimingEditResolverState::bindBaseline(fixture.database());
  const auto baseline = timing_dual_csr::DualCsrBaselineFactory::create(input,
      timing_edit::TimingEditSessionToken{input.source_identity.session_token}, resolver.highWater());
  requireTimingScatter(baseline.success(), "report fixture graph preparation failed");
  const CsrReportValues report_values{{}, {}, &values};

  // Report queries decode only requested points and never publish into the native owner.
  detail::CsrReportNodeReader reader(fixture.database(), *baseline.state, resolver, report_values, {}, {});
  reader.preparePathTrace();
  auto& first = reader.point(input.nodes.front().key.full_name);
  requireTimingScatter(&first == &reader.point(input.nodes.front().key.full_name), "report node cache did not reuse its value");
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), before, "report node query mutated native timing");
  for (const auto& node : input.nodes)
    fixture.database().get_timing_point_map().at(node.key.full_name) = reader.point(node.key.full_name);
  auto expected_nodes = before;
  expected_nodes.nodes = fixture.expected_oracle.nodes;
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), expected_nodes, "report node decoding differs from full native oracle");
  requireTimingScatter(&first == &reader.point(input.nodes.front().key.full_name), "report cache invalidated an earlier point");
  // getPathState may insert a default when the report re-resolves a missing clock tag.
  first.get_path_state_map()[AnalysisType::kMax][PathSourceType::kInput][TransType::kRise]["missing-report-tag"];
  reader.prepare({});
  for (const auto& node : input.nodes)
    fixture.database().get_timing_point_map().at(node.key.full_name) = reader.point(node.key.full_name);
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), expected_nodes,
                                "next report preparation reused an inserted missing-tag default");
  poisonTimingScatterFixture(fixture, 73);

  auto result = detail::prepareResolvedTimingResult(targets, layout, values);
  auto* prepared = std::get_if<detail::PreparedTimingResult>(&result);
  requireTimingScatter(prepared != nullptr, "resolved scatter rejected healthy computation");
  detail::publishPreparedTimingResult(*prepared);
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), fixture.expected_oracle, "resolved scatter round trip");
}

}  // namespace

void runResolvedTimingScatterContracts()
{
  const int threads_before = omp_get_max_threads();
  const int dynamic_before = omp_get_dynamic();
  omp_set_dynamic(0);
  for (int threads : {1, 2, 8}) {
    omp_set_num_threads(threads);
    for (auto fixture : {ReferenceNativeFixtureCase::kDiamond, ReferenceNativeFixtureCase::kParallel})
      runResolvedTimingScatterContract(fixture);
  }
  omp_set_num_threads(threads_before);
  omp_set_dynamic(dynamic_before);
}

}  // namespace ista::csr::test
