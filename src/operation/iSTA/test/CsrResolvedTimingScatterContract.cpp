#include <algorithm>
#include <variant>

#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrTimingScatterTestFixture.hpp"
#include "Database.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista::csr::test {

void runResolvedTimingScatterContracts()
{
  auto fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kDiamond, 0x6180U);
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
  const auto rejected = [&](const CsrTimingValues& bad, const detail::StableScatterTargets& mapping) {
    auto result = detail::prepareResolvedTimingResult(mapping, layout, bad);
    requireTimingScatter(std::holds_alternative<detail::TimingResultPreparationFailure>(result), "resolved scatter accepted malformed values/mapping");
    requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), before, "resolved preparation failure mutated native state");
  };
  auto bad = static_cast<const CsrTimingValues&>(values);
  bad.paths.arrivals.values.clear();
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
  requireTimingScatter(!bad.paths.start_nodes.presence.empty(), "resolved fixture has no paths");
  bad.paths.start_nodes.presence.front() = 0;
  rejected(bad, targets);

  const auto predecessor = std::ranges::find(values.nodes.predecessor_arcs.presence, std::uint8_t{1});
  requireTimingScatter(predecessor != values.nodes.predecessor_arcs.presence.end(), "resolved fixture has no predecessor");
  const auto node = static_cast<std::size_t>(predecessor - values.nodes.predecessor_arcs.presence.begin());
  auto inactive = targets;
  inactive.arcs.at(values.nodes.predecessor_arcs.values[node].value()).arc = nullptr;
  rejected(values, inactive);

  auto result = detail::prepareResolvedTimingResult(targets, layout, values);
  auto* prepared = std::get_if<detail::PreparedTimingResult>(&result);
  requireTimingScatter(prepared != nullptr, "resolved scatter rejected healthy computation");
  detail::publishPreparedTimingResult(*prepared);
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), fixture.expected_oracle, "resolved scatter round trip");
}

}  // namespace ista::csr::test
