// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrTimingScatterTestFixture.hpp"
#include "Database.hpp"
#include "ParasiticLibrary.hpp"
#include "ParasiticNet.hpp"
#include "Pin.hpp"
#include "TimingCell.hpp"
#include "TimingCheckArc.hpp"
#include "TimingClock.hpp"
#include "TimingConstraint.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingPoint.hpp"
#include "TimingTable.hpp"

namespace ista::csr::test {
namespace {

using ResultMutation = std::function<void(CsrTimingResult&)>;

[[nodiscard]] bool sameRawDouble(double lhs, double rhs)
{
  return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

[[nodiscard]] bool sameRawDoubles(const std::vector<double>& lhs, const std::vector<double>& rhs)
{
  return std::ranges::equal(lhs, rhs, sameRawDouble);
}
[[nodiscard]] CsrTimingScatterTestFixture makeMaterialized(std::uint64_t token, ReferenceNativeFixtureCase fixture_case = ReferenceNativeFixtureCase::kDiamond,
                                                           bool explicit_rc = false)
{
  CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(fixture_case, token);
  if (explicit_rc) {
    fixture.context.rc_corner = "typical_rc";
    CsrReferenceInputExportResult capture = exportCsrReferenceInputCandidate(fixture.database(), fixture.context, fixture.session_token);
    requireTimingScatter(std::holds_alternative<CsrReferenceInput>(capture), "explicit-RC baseline export failed");
    fixture.committed_input = std::get<CsrReferenceInput>(std::move(capture));
    fixture.forbidden_bytes = timingScatterForbiddenBytes(fixture.database(), fixture.context);
  }
  fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
  return fixture;
}

[[nodiscard]] std::string differentFingerprint(std::string fingerprint)
{
  requireTimingScatter(fingerprint.size() == 64, "fixture fingerprint is not SHA-256");
  fingerprint[0] = fingerprint[0] == '0' ? '1' : '0';
  return fingerprint;
}

void expectZeroWrite(CsrTimingScatterTestFixture& fixture, const CsrTimingResult& result, const StaAnalysisContext& context, std::uint64_t active_token,
                     std::uint64_t active_generation, CsrScatterCode code, std::optional<CsrInvalidationReason> reason, const std::string& name)
{
  const CsrNativeTimingOracle oracle_before = readNativeTimingOracle(fixture.database());
  std::vector<std::uint8_t> forbidden_before;
  try {
    forbidden_before = timingScatterForbiddenBytes(fixture.database(), context);
  } catch (const std::exception& error) {
    throw std::runtime_error(name + " current canonical capture failed: " + error.what());
  }

  const CsrTimingResultScatterAttempt attempt
      = scatterCsrTimingResult(fixture.database(), fixture.committed_input, result, context, active_token, active_generation);
  requireScatterCode(attempt, code, 0, reason, name);
  requireTimingScatter(!attempt.current_capture_error.has_value(), name + " unexpectedly failed current capture");
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), oracle_before, name + " zero-write oracle");
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database(), context) == forbidden_before, name + " changed forbidden source bytes");
}

void resultFailure(std::uint64_t token, CsrScatterCode expected, const std::string& name, const ResultMutation& mutate)
{
  CsrTimingScatterTestFixture fixture = makeMaterialized(token);
  CsrTimingResult bad = fixture.result;
  mutate(bad);
  poisonTimingScatterFixture(fixture, token);
  expectZeroWrite(fixture, bad, fixture.context, fixture.session_token, fixture.graph_generation, expected, std::nullopt, name);
}

void testGuardPriority()
{
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6201U);
    poisonTimingScatterFixture(fixture, 1);
    expectZeroWrite(fixture, fixture.result, fixture.context, 0, 0, CsrScatterCode::kInvalidState, std::nullopt, "zero active token");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6202U);
    poisonTimingScatterFixture(fixture, 2);
    expectZeroWrite(fixture, fixture.result, fixture.context, fixture.session_token + 1, 0, CsrScatterCode::kInvalidState, std::nullopt,
                    "foreign active token");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6203U);
    poisonTimingScatterFixture(fixture, 3);
    CsrTimingResult malformed = fixture.result;
    malformed.schema_version = "invalid";
    expectZeroWrite(fixture, malformed, fixture.context, fixture.session_token, 1, CsrScatterCode::kInvalidState, std::nullopt,
                    "guard before malformed result");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6204U);
    poisonTimingScatterFixture(fixture, 4);
    fixture.committed_input.schema_version = "invalid";
    expectZeroWrite(fixture, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation, CsrScatterCode::kInvalidState, std::nullopt,
                    "malformed committed input");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6205U);
    // Propagated clocks are supported; case analysis remains out of scope.
    const auto& node = fixture.committed_input.nodes.front();
    fixture.committed_input.case_analysis_constraints = {{node.key.full_name, node.id, true}};
    fixture.committed_input.unsupported_reasons = {CsrReferenceUnsupportedReason::kCaseAnalysis};
    fixture.committed_input.source_identity = makeReferenceInputIdentity(fixture.committed_input, fixture.session_token, fixture.graph_generation);
    requireTimingScatter(validateReferenceInputForSession(fixture.committed_input, fixture.session_token).empty(),
                         "unsupported guard fixture is not strict-valid");
    poisonTimingScatterFixture(fixture, 5);
    expectZeroWrite(fixture, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation, CsrScatterCode::kInvalidState, std::nullopt,
                    "strict-valid unsupported committed input");
  }
}

void testResultPriority()
{
  resultFailure(0x6210U, CsrScatterCode::kSchemaMismatch, "four-class overlap", [](CsrTimingResult& result) {
    result.schema_version = "invalid";
    ++result.source_identity.session_token;
    result.source_identity.graph_generation = 1;
    result.source_identity.topology_fingerprint = differentFingerprint(result.source_identity.topology_fingerprint);
  });
  resultFailure(0x6211U, CsrScatterCode::kSessionMismatch, "session mismatch", [](CsrTimingResult& result) { ++result.source_identity.session_token; });
  resultFailure(0x6212U, CsrScatterCode::kGenerationMismatch, "generation mismatch",
                [](CsrTimingResult& result) { result.source_identity.graph_generation = 1; });

  using FingerprintMember = std::string CsrSnapshotIdentity::*;
  constexpr std::array<FingerprintMember, 6> kFingerprints{
      &CsrSnapshotIdentity::topology_fingerprint, &CsrSnapshotIdentity::clock_constraint_fingerprint, &CsrSnapshotIdentity::library_fingerprint,
      &CsrSnapshotIdentity::corner_fingerprint,   &CsrSnapshotIdentity::parasitic_fingerprint,        &CsrSnapshotIdentity::timing_input_fingerprint,
  };
  for (std::size_t index = 0; index < kFingerprints.size(); ++index) {
    resultFailure(
        0x6220U + index, CsrScatterCode::kSourceIdentityMismatch, "source fingerprint " + std::to_string(index),
        [member = kFingerprints[index]](CsrTimingResult& result) { result.source_identity.*member = differentFingerprint(result.source_identity.*member); });
  }

  resultFailure(0x6230U, CsrScatterCode::kShapeMismatch, "Arc shape before numeric", [](CsrTimingResult& result) {
    result.arcs.transition_delays.presence.pop_back();
    result.arcs.delays.values.front() = std::numeric_limits<double>::quiet_NaN();
  });
  resultFailure(0x6231U, CsrScatterCode::kShapeMismatch, "canonical absent payload", [](CsrTimingResult& result) {
    result.nodes.clock_names.presence.front() = 0;
    result.nodes.clock_names.values.front() = "forbidden";
  });
  resultFailure(0x6232U, CsrScatterCode::kResultNumericInvalid, "NaN node scalar",
                [](CsrTimingResult& result) { result.nodes.arrivals.values.front() = std::numeric_limits<double>::quiet_NaN(); });
  resultFailure(0x6233U, CsrScatterCode::kResultNumericInvalid, "negative data slew", [](CsrTimingResult& result) {
    const auto found = std::ranges::find(result.data_slews.presence, std::uint8_t{1});
    requireTimingScatter(found != result.data_slews.presence.end(), "negative-slew fixture has no present lane");
    result.data_slews.values[std::distance(result.data_slews.presence.begin(), found)] = -1.0;
  });
  resultFailure(0x6234U, CsrScatterCode::kShapeMismatch, "timing-order FIFO",
                [](CsrTimingResult& result) { result.level_nodes.front() = NodeId{result.node_to_level.size()}; });
  resultFailure(0x6235U, CsrScatterCode::kPredecessorMismatch, "scalar predecessor endpoint", [](CsrTimingResult& result) {
    result.nodes.predecessor_nodes.presence.front() = 1;
    result.nodes.predecessor_nodes.values.front() = NodeId{0};
    result.nodes.predecessor_arcs.presence.front() = 1;
    result.nodes.predecessor_arcs.values.front() = ArcId{0};
  });
  resultFailure(0x6236U, CsrScatterCode::kShapeMismatch, "noncanonical path presence before ArcId mapping", [](CsrTimingResult& result) {
    requireTimingScatter(!result.paths.row_offsets.empty() && result.paths.row_offsets.back() > 0, "presence overlap fixture has no path entry");
    const ArcId invalid_arc{result.arcs.delays.values.size()};
    result.paths.predecessor_nodes.presence.front() = 2;
    result.paths.predecessor_arcs.presence.front() = 2;
    result.paths.predecessor_arc_delays.presence.front() = 2;
    result.paths.predecessor_transitions.presence.front() = 2;
    result.paths.predecessor_arcs.values.front() = invalid_arc;
  });

  resultFailure(0x6239U, CsrScatterCode::kResultNumericInvalid, "infinite node slack",
                [](CsrTimingResult& result) { result.nodes.slacks.values.front() = std::numeric_limits<double>::infinity(); });
  resultFailure(0x623aU, CsrScatterCode::kIdMappingMismatch, "NodeId mapping", [](CsrTimingResult& result) {
    const auto found = std::ranges::find(result.nodes.predecessor_nodes.presence, std::uint8_t{1});
    requireTimingScatter(found != result.nodes.predecessor_nodes.presence.end(), "NodeId mapping fixture has no present predecessor");
    const std::size_t lane = static_cast<std::size_t>(std::distance(result.nodes.predecessor_nodes.presence.begin(), found));
    result.nodes.predecessor_nodes.values[lane] = NodeId{result.nodes.arrivals.values.size()};
  });
  resultFailure(0x623bU, CsrScatterCode::kIdMappingMismatch, "ArcId mapping", [](CsrTimingResult& result) {
    const auto found = std::ranges::find(result.nodes.predecessor_arcs.presence, std::uint8_t{1});
    requireTimingScatter(found != result.nodes.predecessor_arcs.presence.end(), "ArcId mapping fixture has no present predecessor");
    const std::size_t lane = static_cast<std::size_t>(std::distance(result.nodes.predecessor_arcs.presence.begin(), found));
    result.nodes.predecessor_arcs.values[lane] = ArcId{result.arcs.delays.values.size()};
  });
  resultFailure(0x623cU, CsrScatterCode::kIdMappingMismatch, "clock-tag mapping", [](CsrTimingResult& result) {
    requireTimingScatter(!result.paths.clock_tags.values.empty(), "clock-tag mapping fixture has no path entry");
    result.paths.clock_tags.values.front() = ClockTagId{std::numeric_limits<std::uint64_t>::max()};
  });
  resultFailure(0x623dU, CsrScatterCode::kPredecessorMismatch, "clock predecessor", [](CsrTimingResult& result) {
    ClockResultSoa& clock = result.effective_clock;
    const auto found = std::ranges::find(clock.arrivals.presence, std::uint8_t{1});
    requireTimingScatter(found != clock.arrivals.presence.end(), "clock predecessor fixture has no isolated clock lane");
    const std::size_t lane = static_cast<std::size_t>(std::distance(clock.arrivals.presence.begin(), found));
    requireTimingScatter(
        clock.predecessor_nodes.presence[lane] == 0 && clock.predecessor_arc_delays.presence[lane] == 0 && clock.predecessor_transitions.presence[lane] == 0,
        "clock predecessor fixture lane is not isolated");
    clock.predecessor_nodes.presence[lane] = 1;
    clock.predecessor_arc_delays.presence[lane] = 1;
    clock.predecessor_transitions.presence[lane] = 1;
    clock.predecessor_nodes.values[lane] = NodeId{0};
    clock.predecessor_arc_delays.values[lane] = 0.0;
    clock.predecessor_transitions.values[lane] = Transition::kRise;
  });
  resultFailure(0x623eU, CsrScatterCode::kPredecessorMismatch, "path predecessor", [](CsrTimingResult& result) {
    PathResultSoa& paths = result.paths;
    const auto found = std::ranges::find(paths.predecessor_nodes.presence, std::uint8_t{0});
    requireTimingScatter(found != paths.predecessor_nodes.presence.end(), "path predecessor fixture has no root entry");
    const std::size_t entry = static_cast<std::size_t>(std::distance(paths.predecessor_nodes.presence.begin(), found));
    requireTimingScatter(paths.arrivals.presence[entry] == 1, "path predecessor root arrival is absent");
    const double replacement = paths.arrivals.values[entry] + 1.0;
    requireTimingScatter(std::isfinite(replacement) && replacement != paths.arrivals.values[entry], "path predecessor replacement is not distinct finite");
    paths.arrivals.values[entry] = replacement;
  });
}
void setMaximumScalarDelay(CsrTimingResult& result, ArcId arc_id)
{
  const std::size_t arc = arc_id.value();
  const double maximum = std::numeric_limits<double>::max();
  result.arcs.delays.values.at(arc) = maximum;
  result.arcs.max_delays.values.at(arc) = maximum;
  const std::size_t maximum_lane = arc * kAnalysisCount * kTransitionCount;
  result.arcs.transition_delays.values.at(maximum_lane) = maximum;
  result.arcs.transition_delays.values.at(maximum_lane + 1) = maximum;
}

void testScalarDerivedOverflow()
{
  CsrTimingScatterTestFixture fixture = makeMaterialized(0x6237U, ReferenceNativeFixtureCase::kChain);
  CsrTimingResult bad = fixture.result;
  std::optional<std::array<NodeId, 4>> chain_nodes;
  std::optional<std::array<ArcId, 3>> chain_arcs;
  for (std::size_t downstream = 0; downstream < fixture.committed_input.nodes.size(); ++downstream) {
    if (bad.nodes.predecessor_nodes.presence[downstream] != 1)
      continue;
    const NodeId node2 = bad.nodes.predecessor_nodes.values[downstream];
    const ArcId arc2 = bad.nodes.predecessor_arcs.values[downstream];
    if (bad.nodes.predecessor_nodes.presence[node2.value()] != 1)
      continue;
    const NodeId node1 = bad.nodes.predecessor_nodes.values[node2.value()];
    const ArcId arc1 = bad.nodes.predecessor_arcs.values[node2.value()];
    if (bad.nodes.predecessor_nodes.presence[node1.value()] != 1)
      continue;
    const NodeId node0 = bad.nodes.predecessor_nodes.values[node1.value()];
    const ArcId arc0 = bad.nodes.predecessor_arcs.values[node1.value()];
    if (fixture.committed_input.arcs[arc0.value()].kind == ArcKind::kCell && fixture.committed_input.arcs[arc1.value()].kind == ArcKind::kNet
        && fixture.committed_input.arcs[arc2.value()].kind == ArcKind::kCell) {
      chain_nodes = std::array{node0, node1, node2, NodeId{downstream}};
      chain_arcs = std::array{arc0, arc1, arc2};
      break;
    }
  }
  requireTimingScatter(chain_nodes && chain_arcs, "scalar overflow fixture has no cell-net-cell predecessor chain");

  const auto& nodes = *chain_nodes;
  const auto& arcs = *chain_arcs;
  const double maximum = std::numeric_limits<double>::max();
  requireTimingScatter(std::isfinite(bad.nodes.arrivals.values[nodes[0].value()] + maximum) && bad.nodes.arrivals.values[nodes[0].value()] + maximum == maximum,
                       "scalar overflow first cell lane does not round to DBL_MAX");
  requireTimingScatter(bad.arcs.delays.values[arcs[1].value()] == 0.0, "scalar overflow bridge net is not zero delay");
  setMaximumScalarDelay(bad, arcs[0]);
  setMaximumScalarDelay(bad, arcs[2]);
  for (NodeId node : {nodes[1], nodes[2]}) {
    bad.nodes.arrivals.values[node.value()] = maximum;
    const double required = bad.nodes.requireds.values[node.value()];
    bad.nodes.slacks.values[node.value()] = std::isfinite(required) ? required - maximum : 0.0;
  }
  requireTimingScatter(std::isfinite(bad.nodes.arrivals.values[nodes[3].value()]) && !std::isfinite(maximum + maximum),
                       "scalar overflow lane did not retain a finite published arrival against an overflowing derived sum");

  poisonTimingScatterFixture(fixture, 57);
  expectZeroWrite(fixture, bad, fixture.context, fixture.session_token, fixture.graph_generation, CsrScatterCode::kResultNumericInvalid, std::nullopt,
                  "scalar predecessor derived-addition overflow");
}

struct PathOverflowLane
{
  std::size_t entry;
  std::size_t intermediate_row;
  std::size_t target_row;
  std::size_t first_graph_lane;
  std::size_t second_graph_lane;
  std::size_t first_data_lane;
  std::size_t second_data_lane;
  std::size_t first_input_transition;
  std::size_t second_input_transition;
  ArcId first_arc;
  ArcId second_arc;
};

[[nodiscard]] std::optional<PathOverflowLane> findPathOverflowLane(const CsrReferenceInput& input, const CsrTimingResult& result)
{
  for (std::size_t row = 0; row + 1 < result.paths.row_offsets.size(); ++row) {
    std::size_t decoded = row;
    const std::size_t first_input_transition = decoded % kTransitionCount;
    decoded /= kTransitionCount;
    const std::size_t path_source = decoded % kPathSourceCount;
    decoded /= kPathSourceCount;
    const std::size_t analysis = decoded % kAnalysisCount;
    const std::size_t node = decoded / kAnalysisCount;
    for (std::uint64_t entry = result.paths.row_offsets[row]; entry < result.paths.row_offsets[row + 1]; ++entry) {
      if (result.paths.predecessor_nodes.presence[entry] != 0)
        continue;
      for (std::uint64_t first_offset = input.out_row_offsets[node]; first_offset < input.out_row_offsets[node + 1]; ++first_offset) {
        const ArcId first_id = input.out_arc_ids[first_offset];
        const ArcRecord& first = input.arcs[first_id.value()];
        if (!first.eligibility.data_arrival)
          continue;
        for (std::size_t intermediate_transition = 0; intermediate_transition < kTransitionCount; ++intermediate_transition) {
          const std::size_t first_graph
              = (((first_id.value() * kAnalysisCount) + analysis) * kTransitionCount + first_input_transition) * kTransitionCount + intermediate_transition;
          const std::size_t first_data = ((first.sink.value() * kAnalysisCount) + analysis) * kTransitionCount + intermediate_transition;
          const std::size_t intermediate_row
              = (((first.sink.value() * kAnalysisCount) + analysis) * kPathSourceCount + path_source) * kTransitionCount + intermediate_transition;
          if (result.arcs.graph_delays.presence[first_graph] != 1 || result.data_slews.presence[first_data] != 1
              || result.paths.row_offsets[intermediate_row] != result.paths.row_offsets[intermediate_row + 1])
            continue;
          for (std::uint64_t second_offset = input.out_row_offsets[first.sink.value()]; second_offset < input.out_row_offsets[first.sink.value() + 1];
               ++second_offset) {
            const ArcId second_id = input.out_arc_ids[second_offset];
            const ArcRecord& second = input.arcs[second_id.value()];
            if (second.kind != ArcKind::kCell || !second.eligibility.data_arrival)
              continue;
            for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
              const std::size_t second_graph
                  = (((second_id.value() * kAnalysisCount) + analysis) * kTransitionCount + intermediate_transition) * kTransitionCount + output_transition;
              const std::size_t second_data = ((second.sink.value() * kAnalysisCount) + analysis) * kTransitionCount + output_transition;
              const std::size_t target_row
                  = (((second.sink.value() * kAnalysisCount) + analysis) * kPathSourceCount + path_source) * kTransitionCount + output_transition;
              if (result.arcs.graph_delays.presence[second_graph] == 1 && result.data_slews.presence[second_data] == 1
                  && result.paths.row_offsets[target_row] == result.paths.row_offsets[target_row + 1])
                return PathOverflowLane{
                    static_cast<std::size_t>(entry), intermediate_row,        target_row, first_graph, second_graph, first_data, second_data,
                    first_input_transition,          intermediate_transition, first_id,   second_id};
            }
          }
        }
      }
    }
  }
  return std::nullopt;
}

template <typename T>
void insertPathValue(OptionalSoa<T>& values, std::size_t entry, std::uint8_t presence, const T& value)
{
  values.presence.insert(values.presence.begin() + entry, presence);
  values.values.insert(values.values.begin() + entry, value);
}
void appendPathEntry(PathResultSoa& paths, std::size_t row, ClockTagId clock_tag, double arrival, double slew, double launch, NodeId start_node,
                     NodeId predecessor_node, ArcId predecessor_arc, double predecessor_delay, Transition predecessor_transition, std::uint8_t crpr_presence,
                     NodeId crpr_node, Transition crpr_transition)
{
  const std::size_t target = static_cast<std::size_t>(paths.row_offsets[row]);
  insertPathValue(paths.clock_tags, target, 1, clock_tag);
  insertPathValue(paths.arrivals, target, 1, arrival);
  insertPathValue(paths.slews, target, 1, slew);
  insertPathValue(paths.launches, target, 1, launch);
  insertPathValue(paths.start_nodes, target, 1, start_node);
  insertPathValue(paths.predecessor_nodes, target, 1, predecessor_node);
  insertPathValue(paths.predecessor_arcs, target, 1, predecessor_arc);
  insertPathValue(paths.predecessor_arc_delays, target, 1, predecessor_delay);
  insertPathValue(paths.predecessor_transitions, target, 1, predecessor_transition);
  insertPathValue(paths.crpr_nodes, target, crpr_presence, crpr_node);
  insertPathValue(paths.crpr_transitions, target, crpr_presence, crpr_transition);
  for (std::size_t offset = row + 1; offset < paths.row_offsets.size(); ++offset)
    ++paths.row_offsets[offset];
}

void testPathDerivedOverflow()
{
  CsrTimingScatterTestFixture fixture = makeMaterialized(0x6238U, ReferenceNativeFixtureCase::kChain);
  CsrTimingResult bad = fixture.result;
  const std::optional<PathOverflowLane> selected = findPathOverflowLane(fixture.committed_input, bad);
  requireTimingScatter(selected.has_value(), "path overflow fixture has no root-net-cell lane with empty downstream path rows");

  PathResultSoa& paths = bad.paths;
  const std::size_t source = selected->entry;
  const ClockTagId clock_tag = paths.clock_tags.values[source];
  const double source_arrival = paths.arrivals.values[source];
  const double launch = paths.launches.values[source];
  const NodeId start_node = paths.start_nodes.values[source];
  const std::uint8_t crpr_presence = paths.crpr_nodes.presence[source];
  const NodeId crpr_node = paths.crpr_nodes.values[source];
  const Transition crpr_transition = paths.crpr_transitions.values[source];
  const double first_delay = bad.arcs.graph_delays.values[selected->first_graph_lane];
  const double intermediate_arrival = std::round((source_arrival + first_delay) * 1E15) / 1E15;
  const double maximum = std::numeric_limits<double>::max();
  const double finite_sum = intermediate_arrival + maximum;
  requireTimingScatter(std::isfinite(source_arrival) && std::isfinite(first_delay) && std::isfinite(intermediate_arrival) && std::isfinite(finite_sum)
                           && !std::isfinite(finite_sum * 1E15),
                       "path overflow fixture did not isolate second-hop rounded-scale overflow");

  const ArcRecord& first = fixture.committed_input.arcs[selected->first_arc.value()];
  appendPathEntry(paths, selected->intermediate_row, clock_tag, intermediate_arrival, bad.data_slews.values[selected->first_data_lane], launch, start_node,
                  first.source, selected->first_arc, first_delay, fixture.committed_input.transition_order[selected->first_input_transition], crpr_presence,
                  crpr_node, crpr_transition);
  const ArcRecord& second = fixture.committed_input.arcs[selected->second_arc.value()];
  appendPathEntry(paths, selected->target_row, clock_tag, intermediate_arrival, bad.data_slews.values[selected->second_data_lane], launch, start_node,
                  second.source, selected->second_arc, maximum, fixture.committed_input.transition_order[selected->second_input_transition], crpr_presence,
                  crpr_node, crpr_transition);
  bad.arcs.graph_delays.values[selected->second_graph_lane] = maximum;

  poisonTimingScatterFixture(fixture, 58);
  expectZeroWrite(fixture, bad, fixture.context, fixture.session_token, fixture.graph_generation, CsrScatterCode::kResultNumericInvalid, std::nullopt,
                  "path predecessor rounded-scale overflow");
}

void addTopologyDrift(Database& database)
{
  constexpr const char* kName = "topology_drift";
  Pin pin;
  pin.set_full_name(kName);
  pin.set_pin_name(kName);
  pin.set_direction(ista::PinDirection::kOutput);
  pin.set_is_port(true);
  database.get_pin_map().emplace(kName, std::move(pin));
  database.get_timing_point_map().emplace(kName, TimingPoint{});
}

TimingTable& libraryDelayTable(Database& database)
{
  TimingTable& table = database.get_timing_library().get_cell_map().at("DFF_X1").get_cell_arc_list().at(1).get_timing_arc_list().at(0).get_delay_table_map().at(
      TransType::kRise);
  requireTimingScatter(!table.get_value_list().empty(), "library drift table has no values");
  return table;
}

void addLibraryDrift(Database& database)
{
  libraryDelayTable(database).get_value_list().front() += 0.03125;
}

void addParasiticDrift(Database& database)
{
  const std::string net_name = database.get_net_map().begin()->first;
  ParasiticNet net;
  net.set_net_name(net_name);
  net.set_lumped_capacitance(0.25);
  auto& library = database.get_parasitic_library();
  library.set_spef_file_path("scatter-drift.spef");
  library.set_capacitive_unit("PF");
  library.set_resistance_unit("OHM");
  library.get_net_map().emplace(net_name, std::move(net));
}

void requireParasiticCanary(const ParasiticLibrary& library, const std::string& capacitive_unit, const std::string& name)
{
  requireTimingScatter(library.get_spef_file_path() == "scatter-drift.spef" && library.get_capacitive_unit() == capacitive_unit
                           && library.get_resistance_unit() == "OHM" && library.get_net_map().size() == 1,
                       name + " changed parasitic library metadata");
  const auto& [map_name, net] = *library.get_net_map().begin();
  requireTimingScatter(map_name == net.get_net_name() && net.get_lumped_capacitance() == 0.25 && net.get_node_map().empty() && net.get_resistor_list().empty(),
                       name + " changed parasitic net payload");
}

void currentDrift(std::uint64_t token, CsrInvalidationReason reason, const std::string& name, const std::function<void(CsrTimingScatterTestFixture&)>& mutate,
                  ReferenceNativeFixtureCase fixture_case = ReferenceNativeFixtureCase::kDiamond, bool explicit_rc = false)
{
  CsrTimingScatterTestFixture fixture = makeMaterialized(token, fixture_case, explicit_rc);
  mutate(fixture);
  poisonTimingScatterFixture(fixture, token);
  expectZeroWrite(fixture, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation, CsrScatterCode::kSnapshotInvalidated, reason,
                  name);
}

void testCurrentDriftPriority()
{
  currentDrift(0x6240U, CsrInvalidationReason::kTopologyChanged, "topology drift",
               [](CsrTimingScatterTestFixture& fixture) { addTopologyDrift(fixture.database()); });
  currentDrift(0x6241U, CsrInvalidationReason::kClockConstraintChanged, "clock drift", [](CsrTimingScatterTestFixture& fixture) {
    TimingClock& clock = fixture.database().get_timing_constraint().get_clock_map().begin()->second;
    clock.set_period(clock.get_period() + 1.0);
  });
  currentDrift(0x6242U, CsrInvalidationReason::kCornerChanged, "corner drift",
               [](CsrTimingScatterTestFixture& fixture) { fixture.context.temperature = "125"; });
  currentDrift(
      0x6243U, CsrInvalidationReason::kParasiticChanged, "parasitic drift", [](CsrTimingScatterTestFixture& fixture) { addParasiticDrift(fixture.database()); },
      ReferenceNativeFixtureCase::kDiamond, true);
  // Successful timing-input captures are closed over the five preceding fingerprints and exact-derived fields;
  // phase-6 malformed capture evidence below is the reachable timing-input-only boundary.
  currentDrift(0x6245U, CsrInvalidationReason::kTopologyChanged, "topology before clock overlap", [](CsrTimingScatterTestFixture& fixture) {
    addTopologyDrift(fixture.database());
    fixture.database().get_timing_constraint().get_clock_map().begin()->second.set_period(17.0);
  });
  currentDrift(
      0x6246U, CsrInvalidationReason::kClockConstraintChanged, "clock before parasitic overlap",
      [](CsrTimingScatterTestFixture& fixture) {
        fixture.database().get_timing_constraint().get_clock_map().begin()->second.set_period(19.0);
        addParasiticDrift(fixture.database());
      },
      ReferenceNativeFixtureCase::kDiamond, true);
  currentDrift(0x6247U, CsrInvalidationReason::kTopologyChanged, "drift before bad result", [](CsrTimingScatterTestFixture& fixture) {
    addTopologyDrift(fixture.database());
    fixture.result.schema_version = "invalid";
  });
  currentDrift(
      0x6248U, CsrInvalidationReason::kLibraryChanged, "library drift", [](CsrTimingScatterTestFixture& fixture) { addLibraryDrift(fixture.database()); },
      ReferenceNativeFixtureCase::kChain);
  currentDrift(
      0x6249U, CsrInvalidationReason::kTopologyChanged, "topology before library overlap",
      [](CsrTimingScatterTestFixture& fixture) {
        addTopologyDrift(fixture.database());
        addLibraryDrift(fixture.database());
      },
      ReferenceNativeFixtureCase::kChain);
  currentDrift(
      0x624aU, CsrInvalidationReason::kClockConstraintChanged, "constraint before library overlap",
      [](CsrTimingScatterTestFixture& fixture) {
        fixture.database().get_timing_constraint().get_clock_map().begin()->second.set_period(23.0);
        addLibraryDrift(fixture.database());
      },
      ReferenceNativeFixtureCase::kChain);
  currentDrift(
      0x624bU, CsrInvalidationReason::kLibraryChanged, "library before parasitic overlap",
      [](CsrTimingScatterTestFixture& fixture) {
        addLibraryDrift(fixture.database());
        addParasiticDrift(fixture.database());
      },
      ReferenceNativeFixtureCase::kChain, true);
  currentDrift(
      0x624cU, CsrInvalidationReason::kCornerChanged, "corner before parasitic overlap",
      [](CsrTimingScatterTestFixture& fixture) {
        fixture.context.temperature = "125";
        addParasiticDrift(fixture.database());
      },
      ReferenceNativeFixtureCase::kDiamond, true);
}

void requireCapturePrefix(const CsrCurrentFingerprints& current, CsrReferenceInputExportPhase phase, const std::string& name)
{
  using FingerprintMember = std::optional<std::string> CsrCurrentFingerprints::*;
  constexpr std::array<FingerprintMember, 6> kMembers{
      &CsrCurrentFingerprints::topology_fingerprint, &CsrCurrentFingerprints::clock_constraint_fingerprint, &CsrCurrentFingerprints::library_fingerprint,
      &CsrCurrentFingerprints::corner_fingerprint,   &CsrCurrentFingerprints::parasitic_fingerprint,        &CsrCurrentFingerprints::timing_input_fingerprint,
  };
  const std::size_t phase_index = static_cast<std::size_t>(phase);
  const std::size_t completed = phase_index == 0 ? 0 : phase_index - 1;
  for (std::size_t index = 0; index < kMembers.size(); ++index) {
    requireTimingScatter((current.*kMembers[index]).has_value() == (index < completed),
                         name + " returned the wrong completed-fingerprint prefix at " + std::to_string(index));
  }
}

void captureFailure(CsrTimingScatterTestFixture& fixture, const StaAnalysisContext& context, std::optional<CsrInvalidationReason> reason,
                    CsrReferenceInputExportPhase phase, const std::string& name)
{
  const CsrNativeTimingOracle oracle_before = readNativeTimingOracle(fixture.database());
  const CsrTimingResultScatterAttempt attempt
      = scatterCsrTimingResult(fixture.database(), fixture.committed_input, fixture.result, context, fixture.session_token, fixture.graph_generation);
  const CsrScatterCode expected_code
      = phase == CsrReferenceInputExportPhase::kAnalysisContextPreflight ? CsrScatterCode::kInvalidState : CsrScatterCode::kSnapshotInvalidated;
  requireScatterCode(attempt, expected_code, 0, reason, name);
  requireTimingScatter(attempt.current_capture_error && attempt.current_capture_error->failed_phase == phase, name + " did not preserve failed-phase evidence");
  requireTimingScatter(attempt.current_capture_error->current_fingerprints == attempt.current_fingerprints, name + " split capture and attempt fingerprints");
  requireCapturePrefix(attempt.current_fingerprints, phase, name);
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), oracle_before, name + " zero-write oracle");
}

void requireRestoredSource(const CsrTimingScatterTestFixture& fixture, const std::string& name)
{
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database(), fixture.context) == fixture.forbidden_bytes,
                       name + " did not restore canonical source bytes");
}

void testCaptureEvidence()
{
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6250U);
    poisonTimingScatterFixture(fixture, 50);
    StaAnalysisContext invalid = fixture.context;
    invalid.liberty_corner.clear();
    captureFailure(fixture, invalid, std::nullopt, CsrReferenceInputExportPhase::kAnalysisContextPreflight, "analysis preflight failure");
    requireTimingScatter(invalid.liberty_corner.empty(), "analysis preflight mutated its invalid-context canary");
    requireRestoredSource(fixture, "analysis preflight failure");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6251U);
    poisonTimingScatterFixture(fixture, 51);
    auto& incoming = fixture.database().get_incoming_arc_list_map().at("left");
    const auto original_incoming = incoming;
    incoming.clear();
    captureFailure(fixture, fixture.context, CsrInvalidationReason::kTopologyChanged, CsrReferenceInputExportPhase::kTopology, "topology capture failure");
    requireTimingScatter(incoming.empty(), "topology capture mutated its malformed-row canary");
    incoming = original_incoming;
    requireRestoredSource(fixture, "topology capture failure");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6252U);
    poisonTimingScatterFixture(fixture, 52);
    TimingClock& clock = fixture.database().get_timing_constraint().get_clock_map().begin()->second;
    const double original_period = clock.get_period();
    clock.set_period(std::numeric_limits<double>::quiet_NaN());
    const double malformed_period = clock.get_period();
    captureFailure(fixture, fixture.context, CsrInvalidationReason::kClockConstraintChanged, CsrReferenceInputExportPhase::kClockConstraint,
                   "constraint capture failure");
    requireTimingScatter(sameRawDouble(clock.get_period(), malformed_period), "constraint capture mutated its NaN-period bits");
    clock.set_period(original_period);
    requireRestoredSource(fixture, "constraint capture failure");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6253U, ReferenceNativeFixtureCase::kChain);
    poisonTimingScatterFixture(fixture, 53);
    TimingTable& table = libraryDelayTable(fixture.database());
    const std::vector<double> original_values = table.get_value_list();
    table.get_value_list().assign(1, std::numeric_limits<double>::infinity());
    const std::vector<double> malformed_values = table.get_value_list();
    captureFailure(fixture, fixture.context, CsrInvalidationReason::kLibraryChanged, CsrReferenceInputExportPhase::kLibrary, "library capture failure");
    requireTimingScatter(sameRawDoubles(table.get_value_list(), malformed_values), "library capture mutated its malformed-table bits");
    table.get_value_list() = original_values;
    requireRestoredSource(fixture, "library capture failure");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6254U);
    poisonTimingScatterFixture(fixture, 54);
    const StaAnalysisContext original_context = fixture.context;
    const ParasiticLibrary original_library = fixture.database().get_parasitic_library();
    fixture.context.temperature = "125";
    addParasiticDrift(fixture.database());
    captureFailure(fixture, fixture.context, CsrInvalidationReason::kCornerChanged, CsrReferenceInputExportPhase::kParasitic,
                   "corner plus parasitic capture failure");
    requireTimingScatter(fixture.context.temperature == "125", "corner plus parasitic capture mutated its context canary");
    requireParasiticCanary(fixture.database().get_parasitic_library(), "PF", "corner plus parasitic capture");
    fixture.context = original_context;
    fixture.database().get_parasitic_library() = original_library;
    requireRestoredSource(fixture, "corner plus parasitic capture failure");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6255U, ReferenceNativeFixtureCase::kDiamond, true);
    poisonTimingScatterFixture(fixture, 55);
    const ParasiticLibrary original_library = fixture.database().get_parasitic_library();
    addParasiticDrift(fixture.database());
    ParasiticLibrary& malformed = fixture.database().get_parasitic_library();
    malformed.get_capacitive_unit().clear();
    captureFailure(fixture, fixture.context, CsrInvalidationReason::kParasiticChanged, CsrReferenceInputExportPhase::kParasitic, "parasitic capture failure");
    requireParasiticCanary(malformed, "", "parasitic capture");
    malformed = original_library;
    requireRestoredSource(fixture, "parasitic capture failure");
  }
  {
    CsrTimingScatterTestFixture fixture = makeMaterialized(0x6256U, ReferenceNativeFixtureCase::kChain);
    poisonTimingScatterFixture(fixture, 56);
    auto& checks = fixture.database().get_instance_map().at("ff0").get_check_arc_list();
    const std::vector<TimingCheckArc> original_checks = checks;
    checks.emplace_back();
    TimingCheckArc& canary = checks.back();
    canary.set_clock_port("__timing_input_capture_failure__");
    captureFailure(fixture, fixture.context, CsrInvalidationReason::kTimingInputChanged, CsrReferenceInputExportPhase::kTimingInput,
                   "timing-input capture failure");
    requireTimingScatter(checks.size() == 1 && canary.get_clock_port() == "__timing_input_capture_failure__" && canary.get_data_port().empty()
                             && canary.get_check_type() == TimingCheckType::kNone && sameRawDouble(canary.get_check_time(), 0.0)
                             && canary.get_timing_arc_list().empty() && canary.get_clock_trans_type() == TransType::kRise,
                         "timing-input capture mutated its malformed-check canary");
    checks = original_checks;
    requireRestoredSource(fixture, "timing-input capture failure");
  }
}

}  // namespace

void runTimingScatterStaleContracts()
{
  testGuardPriority();
  testScalarDerivedOverflow();
  testPathDerivedOverflow();
  testResultPriority();
  testCurrentDriftPriority();
  testCaptureEvidence();
}

}  // namespace ista::csr::test
