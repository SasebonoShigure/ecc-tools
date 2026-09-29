// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <vector>

#include "Arc.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrTimingScatterTestFixture.hpp"
#include "Database.hpp"
#include "TimingPoint.hpp"

namespace ista::csr::test {
void runTimingScatterStaleContracts();
void runResolvedTimingScatterContracts();

namespace {

void verifySuccess(CsrTimingScatterTestFixture& fixture, const std::string& name)
{
  const std::vector<CsrContractViolation> result_violations = validateTimingResult(fixture.committed_input, fixture.result);
  if (!result_violations.empty()) {
    throw std::runtime_error(name + " materialized result violation at " + result_violations.front().field + ": " + result_violations.front().message);
  }
  const std::vector<std::uint8_t> input_before = canonicalReferenceInputBytes(fixture.committed_input);
  const std::vector<std::uint8_t> forbidden_before = timingScatterForbiddenBytes(fixture.database());
  const CsrTimingResultScatterAttempt attempt
      = scatterCsrTimingResult(fixture.database(), fixture.committed_input, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation);
  requireScatterCode(attempt, CsrScatterCode::kSuccess, expectedTimingScatterWrites(fixture.committed_input), std::nullopt, name);
  requireTimingScatter(!attempt.current_capture_error.has_value(), name + " success retained a current-capture error");
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), fixture.expected_oracle, name);
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database()) == forbidden_before, name + " changed forbidden source bytes");
  requireTimingScatter(canonicalReferenceInputBytes(fixture.committed_input) == input_before, name + " mutated the committed input");
}

void verifyNativePathBranches(const Database& database, const std::string& name)
{
  bool has_paths = false;
  bool has_empty_paths = false;
  for (const auto& [point_name, point] : database.get_timing_point_map()) {
    const auto& analyses = point.get_path_state_map();
    has_paths |= !analyses.empty();
    has_empty_paths |= analyses.empty();
    for (const auto& [analysis, sources] : analyses) {
      static_cast<void>(analysis);
      requireTimingScatter(!sources.empty(), name + " retained an empty path-source branch at " + point_name);
      for (const auto& [source, transitions] : sources) {
        static_cast<void>(source);
        requireTimingScatter(!transitions.empty(), name + " retained an empty transition branch at " + point_name);
        for (const auto& [transition, states] : transitions) {
          static_cast<void>(transition);
          requireTimingScatter(!states.empty(), name + " retained an empty clock-tag branch at " + point_name);
        }
      }
    }
  }
  requireTimingScatter(has_paths, name + " materialized no native paths");
  requireTimingScatter(has_empty_paths, name + " materialized path branches for every native node");
}

void testEmptyAndIdempotence()
{
  CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kEmpty, 0x6101U);
  fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
  requireTimingScatter(expectedTimingScatterWrites(fixture.committed_input) == 1, "empty graph write count is not one");
  verifySuccess(fixture, "empty success");
  verifySuccess(fixture, "empty idempotence");
}

void testExplicitSuccessShapes()
{
  {
    CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kChain, 0x6108U);
    requireTimingScatter(fixture.committed_input.ordered_root_ids.size() > 1, "multi-root fixture collapsed to one root");
    fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
    verifySuccess(fixture, "multi-root success");
  }
  {
    CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kSingleton, 0x6105U);
    requireTimingScatter(fixture.committed_input.nodes.size() == 1 && fixture.committed_input.arcs.empty(), "singleton fixture shape differs");
    fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
    verifySuccess(fixture, "singleton success");
  }
  {
    CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kParallel, 0x6106U);
    const auto secondary = std::ranges::find(fixture.committed_input.arcs, std::uint64_t{1}, [](const ArcRecord& arc) { return arc.key.parallel_ordinal; });
    requireTimingScatter(
        fixture.committed_input.nodes.size() == 4 && fixture.committed_input.arcs.size() == 5 && secondary != fixture.committed_input.arcs.end(),
        "parallel fixture shape differs");
    const auto primary = std::ranges::find_if(fixture.committed_input.arcs,
                                              [&](const ArcRecord& arc) { return arc.key.base == secondary->key.base && arc.key.parallel_ordinal == 0; });
    requireTimingScatter(primary != fixture.committed_input.arcs.end(), "parallel fixture did not retain semantic ordinal zero");
    fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
    verifySuccess(fixture, "parallel success");
  }
  {
    CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kLoopDisabled, 0x6107U);
    const auto loop_disabled = std::ranges::count_if(fixture.committed_input.arcs, &ArcRecord::is_loop_disabled);
    requireTimingScatter(fixture.committed_input.nodes.size() == 4 && fixture.committed_input.arcs.size() == 4 && loop_disabled == 1,
                         "loop-disabled fixture shape differs");
    fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
    verifySuccess(fixture, "loop-disabled success");
  }
  {
    CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kDisabled, 0x6109U);
    const auto disabled = std::ranges::count_if(fixture.committed_input.arcs, &ArcRecord::is_disabled);
    const auto loop_disabled = std::ranges::count_if(fixture.committed_input.arcs, &ArcRecord::is_loop_disabled);
    requireTimingScatter(fixture.committed_input.nodes.size() == 4 && fixture.committed_input.arcs.size() == 4 && disabled == 2 && loop_disabled == 0,
                         "disabled fixture shape differs");
    fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
    verifySuccess(fixture, "disabled success");
  }
}

void testPoisonRecoveryAndIdempotence()
{
  CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kDiamond, 0x6102U);
  fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
  const std::vector<std::uint8_t> source_before = fixture.forbidden_bytes;
  poisonTimingScatterFixture(fixture, 17);
  const CsrNativeTimingOracle poisoned = readNativeTimingOracle(fixture.database());
  requireTimingScatter(poisoned != fixture.expected_oracle, "poison did not change the native timing oracle");
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database()) == source_before, "poison changed forbidden source bytes");

  verifySuccess(fixture, "bulk poison recovery");
  verifyNativePathBranches(fixture.database(), "bulk poison recovery");
  verifySuccess(fixture, "bulk recovery idempotence");
  verifyNativePathBranches(fixture.database(), "bulk recovery idempotence");
}
void testSignedZeroRoundTrip()
{
  CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kDiamond, 0x6104U);
  fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
  const auto zero_slack = std::ranges::find_if(fixture.result.nodes.slacks.values, [](double value) { return value == 0.0 && !std::signbit(value); });
  requireTimingScatter(zero_slack != fixture.result.nodes.slacks.values.end(), "signed-zero fixture has no present positive-zero slack");
  const std::size_t node = static_cast<std::size_t>(std::distance(fixture.result.nodes.slacks.values.begin(), zero_slack));
  requireTimingScatter(fixture.result.nodes.slacks.presence[node] == 1, "signed-zero fixture selected an absent slack lane");
  const NodeKey node_key = fixture.committed_input.nodes[node].key;
  const std::vector<std::uint8_t> plus_bytes = canonicalIndependentNativeTimingOracleBytes(fixture.expected_oracle);
  const auto plus_node = std::ranges::find(fixture.expected_oracle.nodes, node_key, &CsrNativeTimingNode::key);
  requireTimingScatter(plus_node != fixture.expected_oracle.nodes.end() && plus_node->slack == 0.0 && !std::signbit(plus_node->slack),
                       "signed-zero fixture native control is not positive zero");
  plus_node->slack = -0.0;

  fixture.result.nodes.slacks.values[node] = -0.0;
  poisonTimingScatterFixture(fixture, 41);
  verifySuccess(fixture, "signed-zero round trip");
  const CsrNativeTimingOracle oracle = readNativeTimingOracle(fixture.database());
  const auto native = std::ranges::find(oracle.nodes, node_key, &CsrNativeTimingNode::key);
  requireTimingScatter(native != oracle.nodes.end() && native->slack == 0.0 && std::signbit(native->slack), "scatter lost the negative-zero slack sign bit");
  requireTimingScatter(canonicalIndependentNativeTimingOracleBytes(oracle) != plus_bytes, "negative-zero native oracle bytes equal positive-zero control");
}

void testMaterializerRejectsMissingLookupKeys()
{
  auto fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kDiamond, 0x6110U);
  requireTimingScatter(std::ranges::any_of(fixture.expected_oracle.nodes, [](const auto& node) {
                         return node.predecessor_arc.has_value();
                       }) && std::ranges::any_of(fixture.expected_oracle.nodes, [](const auto& node) {
                         return !node.paths.empty();
                       }), "missing-key fixture has no predecessor/path consumers");
  const auto check_missing = [&](auto mutate, const std::string& expected) {
    auto input = fixture.committed_input;
    mutate(input);
    try {
      static_cast<void>(materializeTimingResultFromNativeOracle(input, fixture.expected_oracle));
    } catch (const std::runtime_error& error) {
      requireTimingScatter(std::string(error.what()).find(expected) != std::string::npos,
                           "materializer reported the wrong missing-key error");
      return;
    }
    throw std::runtime_error("materializer accepted a missing lookup key: " + expected);
  };
  // Erasing a key keeps the lookup ordered: lower_bound can still find its
  // successor, which must not be mistaken for the requested predecessor.
  check_missing([](auto& input) { input.node_ids_by_key.erase(input.node_ids_by_key.begin()); },
                "NodeKey does not resolve");
  check_missing([](auto& input) { input.arc_ids_by_key.clear(); }, "ArcKey does not resolve");
  check_missing([](auto& input) { input.clock_tags.clear(); }, "clock tag does not resolve");
}

void testStableStorageRelocation()
{
  CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kDiamond, 0x6103U);
  fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
  poisonTimingScatterFixture(fixture, 29);

  std::vector<const Arc*> old_addresses;
  old_addresses.reserve(fixture.database().get_arc_list().size());
  for (const Arc& arc : fixture.database().get_arc_list()) {
    old_addresses.push_back(&arc);
  }
  reverseTimingScatterNativeArcStorage(fixture);
  requireTimingScatter(old_addresses.size() == fixture.database().get_arc_list().size(), "Arc relocation changed cardinality");
  for (std::size_t index = 0; index < old_addresses.size(); ++index) {
    requireTimingScatter(old_addresses[index] != &fixture.database().get_arc_list()[index], "Arc relocation retained a source address");
  }

  verifySuccess(fixture, "stable-key storage relocation");
}

}  // namespace
}  // namespace ista::csr::test

int main()
{
  try {
    ista::csr::test::runResolvedTimingScatterContracts();
    ista::csr::test::testSignedZeroRoundTrip();
    ista::csr::test::testEmptyAndIdempotence();
    ista::csr::test::testExplicitSuccessShapes();
    ista::csr::test::testPoisonRecoveryAndIdempotence();
    ista::csr::test::testStableStorageRelocation();
    ista::csr::test::testMaterializerRejectsMissingLookupKeys();
    ista::csr::test::runTimingScatterStaleContracts();
  } catch (const std::exception& error) {
    std::cerr << "CsrTimingScatterContractTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
