// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include <algorithm>
#include <limits>
#include <string>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrTimingResultValidatorInternal.hpp"
#include "CsrValidatorTestFixture.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

bool hasClass(const detail::TimingResultViolations& errors, CsrTimingResultViolationClass classification)
{
  return std::ranges::any_of(errors, [&](const auto& error) { return error.classification == classification; });
}

void requireClass(const detail::TimingResultViolations& errors, CsrTimingResultViolationClass classification, const std::string& name)
{
  require(hasClass(errors, classification), name + " classification mismatch");
}

template <typename Mutate>
void requireClassifiedMutation(const CsrReferenceInput& input, const CsrTimingResult& baseline, CsrTimingResultViolationClass classification,
                               const std::string& name, Mutate mutate)
{
  CsrTimingResult changed = baseline;
  mutate(changed);
  requireClass(detail::validateTimingResultClassified(input, changed), classification, name);
}

template <typename Mutate>
void rejectMutation(const CsrReferenceInput& input, const CsrTimingResult& baseline, const std::string& name, Mutate mutate)
{
  requireValid(validateTimingResult(input, baseline), name + " baseline");
  CsrTimingResult changed = baseline;
  mutate(changed);
  requireInvalid(validateTimingResult(input, changed), name);
}

void clearPaths(CsrTimingResult& result)
{
  result.paths.row_offsets.assign(2 * kAnalysisCount * kPathSourceCount * kTransitionCount + 1, 0);
  result.paths.clock_tags = soa<ClockTagId>(0);
  result.paths.arrivals = soa<double>(0);
  result.paths.slews = soa<double>(0);
  result.paths.launches = soa<double>(0);
  result.paths.start_nodes = soa<NodeId>(0);
  result.paths.predecessor_nodes = soa<NodeId>(0);
  result.paths.predecessor_arcs = soa<ArcId>(0);
  result.paths.predecessor_arc_delays = soa<double>(0);
  result.paths.predecessor_transitions = transitionSoa(0);
  result.paths.crpr_nodes = soa<NodeId>(0);
  result.paths.crpr_transitions = transitionSoa(0);
}

void addGhostTimingRow(CsrTimingResult& result)
{
  result.arcs.timing_arc_row_offsets = {0, 1};
  result.arcs.timing_arc_indices = {-1};
  result.arcs.timing_arc_delays = soa<double>(8);
  result.arcs.timing_arc_fallbacks = soa<std::uint8_t>(8);
}
CsrReferenceInput singletonClockInput(bool startpoint)
{
  CsrReferenceInput input;
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  const std::string name = startpoint ? "in" : "out";
  input.nodes = {NodeRecord{.id = NodeId{0},
                            .key = port(name),
                            .direction = startpoint ? PinDirection::kInput : PinDirection::kOutput,
                            .is_port = true,
                            .is_startpoint = startpoint}};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}};
  input.out_row_offsets = {0, 0};
  input.in_row_offsets = {0, 0};
  input.ordered_root_ids = {NodeId{0}};
  input.clocks = {ClockConstraintModel{"map_clock", "embedded_clock", {RawNodeReference{name, NodeId{0}}}, 10.0, {1.25, 2.5}, false}};
  input.clock_owner_by_node = {std::optional<std::string>{"map_clock"}};
  input.clock_tags = {"embedded_clock", "map_clock"};
  if (startpoint) {
    input.ordered_start_ids = {NodeId{0}};
    StartSeedModel seed;
    seed.seed_node = NodeId{0};
    seed.path_start_node = NodeId{0};
    seed.path_source = PathSource::kInput;
    seed.clock_tag = "embedded_clock";
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        seed.arrival[analysis][transition] = analysis == 0 && transition == 1 ? 2.5 : 1.25;
        seed.slew[analysis][transition] = 0.0;
      }
    }
    input.start_seeds = {seed};
  }
  refreshIdentity(input);
  return input;
}

CsrTimingResult singletonClockResult(const CsrReferenceInput& input)
{
  const bool startpoint = !input.start_seeds.empty();
  CsrTimingResult result;
  result.source_identity = input.source_identity;
  result.level_offsets = {0, 1};
  result.level_nodes = {NodeId{0}};
  result.node_to_level = {1};
  result.nodes.is_clock_point = soa<std::uint8_t>(1, 1, 1);
  result.nodes.arrivals = soa<double>(1, 1, startpoint ? 1.25 : -std::numeric_limits<double>::infinity());
  result.nodes.requireds = soa<double>(1, 1, std::numeric_limits<double>::infinity());
  result.nodes.slacks = soa<double>(1, 1, 0.0);
  result.nodes.launches = soa<double>(1, 1, startpoint ? 1.25 : 0.0);
  result.nodes.clock_names = soa<std::string>(1, 1, startpoint ? "embedded_clock" : "map_clock");
  result.nodes.predecessor_nodes = soa<NodeId>(1);
  result.nodes.predecessor_arcs = soa<ArcId>(1);

  constexpr std::size_t kClockLanes = kAnalysisCount * kTransitionCount;
  result.effective_clock = emptyClock(kClockLanes);
  result.physical_clock = emptyClock(kClockLanes);
  for (ClockResultSoa* clock : {&result.effective_clock, &result.physical_clock}) {
    clock->arrivals = soa<double>(kClockLanes, 1, 0.0);
    clock->slews = soa<double>(kClockLanes, 1, 0.0);
  }
  result.data_slews = soa<double>(kClockLanes, startpoint ? 1 : 0, 0.0);
  result.paths.row_offsets.assign(kAnalysisCount * kPathSourceCount * kTransitionCount + 1, 0);
  if (startpoint) {
    result.paths.row_offsets = {0, 1, 2, 2, 2, 3, 4, 4, 4};
    result.paths.clock_tags = soa<ClockTagId>(4, 1, ClockTagId{0});
    result.paths.arrivals = OptionalSoa<double>{{1, 1, 1, 1}, {1.25, 2.5, 1.25, 1.25}};
    result.paths.slews = soa<double>(4, 1, 0.0);
    result.paths.launches = OptionalSoa<double>{{1, 1, 1, 1}, {1.25, 2.5, 1.25, 1.25}};
    result.paths.start_nodes = soa<NodeId>(4, 1, NodeId{0});
    result.paths.predecessor_nodes = soa<NodeId>(4);
    result.paths.predecessor_arcs = soa<ArcId>(4);
    result.paths.predecessor_arc_delays = soa<double>(4);
    result.paths.predecessor_transitions = transitionSoa(4);
    result.paths.crpr_nodes = soa<NodeId>(4);
    result.paths.crpr_transitions = transitionSoa(4);
  }
  result.arcs.timing_arc_row_offsets = {0};
  return result;
}

CsrReferenceInput predecessorClockSourceInput()
{
  CsrReferenceInput input = referenceInput();
  input.nodes[1].is_endpoint = false;
  input.ordered_end_ids.clear();
  input.endpoint_required.clear();
  input.clocks = {ClockConstraintModel{"map_b", "embedded_b", {RawNodeReference{"out", NodeId{1}}}, 10.0, {0.0, 5.0}, false}};
  input.clock_owner_by_node = {std::nullopt, std::optional<std::string>{"map_b"}};
  input.port_constraints[0].clock_name = "tag_a";
  input.start_seeds[0].clock_tag = "tag_a";
  input.clock_tags = {"embedded_b", "map_b", "tag_a"};
  refreshIdentity(input);
  return input;
}

CsrTimingResult predecessorClockSourceResult(const CsrReferenceInput& input)
{
  CsrTimingResult result = timingResult(input);
  result.nodes.is_clock_point.values[1] = 1;
  result.nodes.clock_names.values = {"tag_a", "tag_a"};
  for (ClockResultSoa* clock : {&result.effective_clock, &result.physical_clock}) {
    for (std::size_t lane = kAnalysisCount * kTransitionCount; lane < 2 * kAnalysisCount * kTransitionCount; ++lane) {
      clock->arrivals.presence[lane] = 1;
      clock->slews.presence[lane] = 1;
    }
  }
  for (auto& tag : result.paths.clock_tags.values)
    tag = ClockTagId{2};
  return result;
}

template <typename T>
void eraseSoaEntry(OptionalSoa<T>& values, std::size_t entry)
{
  values.presence.erase(values.presence.begin() + entry);
  values.values.erase(values.values.begin() + entry);
}

void erasePathEntry(CsrTimingResult& result, std::size_t entry)
{
  eraseSoaEntry(result.paths.clock_tags, entry);
  eraseSoaEntry(result.paths.arrivals, entry);
  eraseSoaEntry(result.paths.slews, entry);
  eraseSoaEntry(result.paths.launches, entry);
  eraseSoaEntry(result.paths.start_nodes, entry);
  eraseSoaEntry(result.paths.predecessor_nodes, entry);
  eraseSoaEntry(result.paths.predecessor_arcs, entry);
  eraseSoaEntry(result.paths.predecessor_arc_delays, entry);
  eraseSoaEntry(result.paths.predecessor_transitions, entry);
  eraseSoaEntry(result.paths.crpr_nodes, entry);
  eraseSoaEntry(result.paths.crpr_transitions, entry);
  for (auto& offset : result.paths.row_offsets)
    if (offset > entry)
      --offset;
}

void testNativeEquivalentBaseline()
{
  const CsrReferenceInput input = referenceInput();
  const CsrTimingResult result = timingResult(input);
  require(input.nodes.size() == 2 && input.arcs.size() == 1, "timing fixture graph must remain nonvacuous");
  require(result.paths.clock_tags.values.size() == 8, "timing fixture must retain four roots and four propagated paths");
  requireValid(validateTimingResult(input, result), "native-equivalent timing result");

  CsrTimingResult infinite_required = result;
  infinite_required.nodes.requireds.values[1] = std::numeric_limits<double>::infinity();
  infinite_required.nodes.slacks.values[1] = 0.0;
  requireValid(validateTimingResult(input, infinite_required), "positive-infinity required sentinel");

  requireInvalid(validateTimingResult(clockInput(), timingResult(clockInput())), "unsupported input timing result");
}

void testIdentityLevelsAndNodeState()
{
  const CsrReferenceInput input = referenceInput();
  const CsrTimingResult result = timingResult(input);
  rejectMutation(input, result, "result schema", [](auto& value) { value.schema_version = "2.0.0"; });
  rejectMutation(input, result, "result identity", [](auto& value) { value.source_identity.session_token = 99; });
  rejectMutation(input, result, "level offsets", [](auto& value) { value.level_offsets.pop_back(); });
  rejectMutation(input, result, "level FIFO order", [](auto& value) { std::ranges::reverse(value.level_nodes); });
  rejectMutation(input, result, "node level", [](auto& value) { value.node_to_level[1] = 1; });

  rejectMutation(input, result, "clock-point required presence", [](auto& value) { value.nodes.is_clock_point.presence[0] = 0; });
  rejectMutation(input, result, "clock-point domain", [](auto& value) { value.nodes.is_clock_point.values[0] = 2; });
  rejectMutation(input, result, "arrival shape", [](auto& value) { value.nodes.arrivals.values.pop_back(); });
  rejectMutation(input, result, "arrival NaN", [](auto& value) { value.nodes.arrivals.values[0] = std::numeric_limits<double>::quiet_NaN(); });
  rejectMutation(input, result, "arrival positive infinity", [](auto& value) { value.nodes.arrivals.values[1] = std::numeric_limits<double>::infinity(); });
  rejectMutation(input, result, "required negative infinity", [](auto& value) { value.nodes.requireds.values[1] = -std::numeric_limits<double>::infinity(); });
  rejectMutation(input, result, "slack recurrence", [](auto& value) { value.nodes.slacks.values[1] = 1.0; });
  rejectMutation(input, result, "start clock name missing", [](auto& value) {
    value.nodes.clock_names.presence[0] = 0;
    value.nodes.clock_names.values[0].clear();
  });
  rejectMutation(input, result, "start clock name wrong", [](auto& value) { value.nodes.clock_names.values[0] = "missing"; });
  rejectMutation(input, result, "predecessor presence pair", [](auto& value) { value.nodes.predecessor_arcs.presence[1] = 0; });
  rejectMutation(input, result, "predecessor NodeId OOB", [](auto& value) { value.nodes.predecessor_nodes.values[1] = NodeId{99}; });
  rejectMutation(input, result, "predecessor recurrence", [](auto& value) { value.nodes.arrivals.values[1] = 2.0; });
  rejectMutation(input, result, "predecessor launch inheritance", [](auto& value) { value.nodes.launches.values[1] = 1.0; });
  rejectMutation(input, result, "absent NodeId payload", [](auto& value) { value.nodes.predecessor_nodes.values[0] = NodeId{1}; });
}

void testClockDataAndPathState()
{
  const CsrReferenceInput input = referenceInput();
  const CsrTimingResult result = timingResult(input);
  rejectMutation(input, result, "clock lane shape", [](auto& value) { value.effective_clock.arrivals.presence.pop_back(); });
  rejectMutation(input, result, "clock ghost lane", [](auto& value) {
    value.effective_clock.arrivals.presence[0] = 1;
    value.effective_clock.slews.presence[0] = 1;
  });
  rejectMutation(input, result, "clock absent negative zero", [](auto& value) { value.physical_clock.arrivals.values[0] = -0.0; });
  CsrTimingResult unknown_clock_predecessor = result;
  {
    auto& value = unknown_clock_predecessor;
    value.effective_clock.predecessor_nodes.presence[0] = 1;
    value.effective_clock.predecessor_arc_delays.presence[0] = 1;
    value.effective_clock.predecessor_transitions.presence[0] = 1;
    value.effective_clock.predecessor_transitions.values[0] = static_cast<Transition>(0xff);
  }
  requireViolation(validateTimingResult(input, unknown_clock_predecessor), "result.effective_clock.predecessor_transitions", "transition must be rise or fall",
                   "clock predecessor unknown enum");
  rejectMutation(input, result, "data slew shape", [](auto& value) { value.data_slews.values.pop_back(); });
  rejectMutation(input, result, "data slew negative", [](auto& value) { value.data_slews.values[0] = -1.0; });
  rejectMutation(input, result, "data slew path mismatch", [](auto& value) { value.data_slews.values[4] = 0.3; });

  rejectMutation(input, result, "all materialized paths missing", [](auto& value) { clearPaths(value); });
  rejectMutation(input, result, "path offset shape", [](auto& value) { value.paths.row_offsets.pop_back(); });
  rejectMutation(input, result, "path offset final", [](auto& value) { --value.paths.row_offsets.back(); });
  CsrTimingResult missing_seed_lane = result;
  erasePathEntry(missing_seed_lane, 0);
  requireViolation(validateTimingResult(input, missing_seed_lane), "result.paths", "every materialized seed lane", "missing seed lane");
  rejectMutation(input, result, "path required presence", [](auto& value) { value.paths.clock_tags.presence[0] = 0; });
  rejectMutation(input, result, "path tag OOB", [](auto& value) { value.paths.clock_tags.values[0] = ClockTagId{1}; });
  rejectMutation(input, result, "path arrival NaN", [](auto& value) { value.paths.arrivals.values[0] = std::numeric_limits<double>::quiet_NaN(); });
  rejectMutation(input, result, "path slew negative", [](auto& value) { value.paths.slews.values[0] = -1.0; });
  rejectMutation(input, result, "path launch infinity", [](auto& value) { value.paths.launches.values[0] = std::numeric_limits<double>::infinity(); });
  rejectMutation(input, result, "path start OOB", [](auto& value) { value.paths.start_nodes.values[0] = NodeId{99}; });
  rejectMutation(input, result, "path root mismatch", [](auto& value) { value.paths.start_nodes.values[0] = NodeId{1}; });
  rejectMutation(input, result, "path root launch", [](auto& value) { value.paths.launches.values[0] = 1.0; });
  rejectMutation(input, result, "path predecessor presence", [](auto& value) { value.paths.predecessor_arcs.presence[4] = 0; });
  rejectMutation(input, result, "path predecessor OOB", [](auto& value) { value.paths.predecessor_nodes.values[4] = NodeId{99}; });
  rejectMutation(input, result, "path ArcId OOB", [](auto& value) { value.paths.predecessor_arcs.values[4] = ArcId{99}; });
  rejectMutation(input, result, "path transition unknown", [](auto& value) { value.paths.predecessor_transitions.values[4] = static_cast<Transition>(0xff); });
  rejectMutation(input, result, "path delay mismatch", [](auto& value) {
    value.paths.predecessor_arc_delays.values[4] = 1.0;
    value.paths.arrivals.values[4] = 2.0;
  });
  rejectMutation(input, result, "path arrival recurrence", [](auto& value) { value.paths.arrivals.values[4] = 2.0; });
  rejectMutation(input, result, "path slew recurrence", [](auto& value) { value.paths.slews.values[4] = 0.3; });
  rejectMutation(input, result, "path absent sentinel", [](auto& value) { value.paths.predecessor_nodes.values[0] = NodeId{1}; });
  rejectMutation(input, result, "path CRPR pair", [](auto& value) { value.paths.crpr_nodes.presence[0] = 1; });
  rejectMutation(input, result, "path ghost CRPR", [](auto& value) {
    value.paths.crpr_nodes.presence[0] = 1;
    value.paths.crpr_transitions.presence[0] = 1;
    value.paths.crpr_transitions.values[0] = Transition::kRise;
  });
  CsrTimingResult path_cycle = result;
  path_cycle.paths.predecessor_nodes.values[4] = NodeId{1};
  requireViolation(validateTimingResult(input, path_cycle), "result.paths.predecessor", "contains a cycle", "path predecessor cycle");
}

void testArcMapsAndTimingRows()
{
  const CsrReferenceInput input = referenceInput();
  const CsrTimingResult result = timingResult(input);
  const CsrReferenceInput cell = cellInput();
  const CsrTimingResult cell_result = cellTimingResult(cell);
  requireValid(validateTimingResult(cell, cell_result), "cell timing-arc baseline");
  rejectMutation(input, result, "arc scalar presence", [](auto& value) { value.arcs.delays.presence[0] = 0; });
  rejectMutation(input, result, "arc scalar NaN", [](auto& value) { value.arcs.delays.values[0] = std::numeric_limits<double>::quiet_NaN(); });
  rejectMutation(input, result, "arc scalar extrema", [](auto& value) { value.arcs.delays.values[0] = 1.0; });
  rejectMutation(input, result, "transition-delay shape", [](auto& value) { value.arcs.transition_delays.values.pop_back(); });
  rejectMutation(input, result, "transition-delay no-SPEF value", [](auto& value) { value.arcs.transition_delays.values[0] = 1.0; });
  rejectMutation(input, result, "input-output identity missing", [](auto& value) { value.arcs.input_output_delays.presence[0] = 0; });
  rejectMutation(input, result, "input-output cross transition", [](auto& value) { value.arcs.input_output_delays.presence[1] = 1; });
  rejectMutation(input, result, "graph identity missing", [](auto& value) { value.arcs.graph_delays.presence[0] = 0; });
  rejectMutation(input, result, "graph absent payload", [](auto& value) { value.arcs.graph_delays.values[1] = 1.0; });
  rejectMutation(input, result, "transition type identity", [](auto& value) { value.arcs.transition_types.values[0] = Transition::kFall; });
  CsrTimingResult unknown_transition = result;
  unknown_transition.arcs.transition_types.values[0] = static_cast<Transition>(0xff);
  requireViolation(validateTimingResult(input, unknown_transition), "result.arcs.transition_types", "present transition must be rise or fall",
                   "transition type unknown");
  rejectMutation(input, result, "timing row offset shape", [](auto& value) { value.arcs.timing_arc_row_offsets.pop_back(); });
  rejectMutation(input, result, "timing row cardinality", [](auto& value) { addGhostTimingRow(value); });
  rejectMutation(cell, cell_result, "timing fallback presence", [](auto& value) { value.arcs.timing_arc_fallbacks.presence[0] = 0; });
  rejectMutation(cell, cell_result, "timing fallback domain", [](auto& value) { value.arcs.timing_arc_fallbacks.values[0] = 2; });
}

void testClockSourceNamePriority()
{
  const CsrReferenceInput isolated = singletonClockInput(false);
  const CsrTimingResult isolated_result = singletonClockResult(isolated);
  requireValid(validateReferenceInput(isolated), "isolated clock-source input");
  requireValid(validateTimingResult(isolated, isolated_result), "isolated clock-source map key");
  rejectMutation(isolated, isolated_result, "missing physical source state", [](auto& value) {
    value.physical_clock = emptyClock(kAnalysisCount * kTransitionCount);
  });
  rejectMutation(isolated, isolated_result, "isolated source embedded name", [](auto& value) { value.nodes.clock_names.values[0] = "embedded_clock"; });

  const CsrReferenceInput start = singletonClockInput(true);
  const CsrTimingResult start_result = singletonClockResult(start);
  requireValid(validateReferenceInput(start), "direct-clock start input");
  requireValid(validateTimingResult(start, start_result), "direct-clock start seed tag");
  rejectMutation(start, start_result, "direct-clock start map key", [](auto& value) { value.nodes.clock_names.values[0] = "map_clock"; });

  const CsrReferenceInput predecessor = predecessorClockSourceInput();
  const CsrTimingResult predecessor_result = predecessorClockSourceResult(predecessor);
  requireValid(validateReferenceInput(predecessor), "predecessor clock-source input");
  requireValid(validateTimingResult(predecessor, predecessor_result), "clock source predecessor-chain name");
  rejectMutation(predecessor, predecessor_result, "clock source owning map key with predecessor",
                 [](auto& value) { value.nodes.clock_names.values[1] = "map_b"; });
}

void testComputedPhysicalClockMutations()
{
  const CsrReferenceInput input = clockInput();
  const auto computation = CsrCpuReferenceBackend{}.compute(input);
  const auto* result = std::get_if<CsrTimingResult>(&computation.outcome);
  require(result != nullptr, "clock fanout must compute actual timing values");
  requireValid(validateTimingResult(input, *result), "computed clock fanout result");
  rejectMutation(input, *result, "computed physical source omitted", [](auto& value) {
    value.physical_clock = emptyClock(2 * kAnalysisCount * kTransitionCount);
  });
  rejectMutation(input, *result, "physical net predecessor transition changed", [](auto& value) {
    value.physical_clock.predecessor_transitions.values[4] = Transition::kFall;
  });
}

void testTypedClassificationAndLegacyProjection()
{
  const CsrReferenceInput input = referenceInput();
  const CsrTimingResult result = timingResult(input);
  requireValid(validateReferenceInput(input), "classified validator precondition");
  require(detail::validateTimingResultClassified(input, result).empty(), "classified baseline must be valid");

  requireClassifiedMutation(input, result, CsrTimingResultViolationClass::kSchema, "schema class", [](auto& value) { value.schema_version = "2.0.0"; });
  requireClassifiedMutation(input, result, CsrTimingResultViolationClass::kSession, "session class",
                            [](auto& value) { value.source_identity.session_token = 99; });
  requireClassifiedMutation(input, result, CsrTimingResultViolationClass::kGeneration, "generation class",
                            [](auto& value) { value.source_identity.graph_generation = 1; });
  requireClassifiedMutation(input, result, CsrTimingResultViolationClass::kSourceIdentity, "source identity class", [](auto& value) {
    char& first = value.source_identity.topology_fingerprint.front();
    first = first == 'a' ? 'b' : 'a';
  });
  requireClassifiedMutation(input, result, CsrTimingResultViolationClass::kShape, "shape class", [](auto& value) { value.level_offsets.pop_back(); });
  requireClassifiedMutation(input, result, CsrTimingResultViolationClass::kNumeric, "numeric class",
                            [](auto& value) { value.nodes.requireds.values[1] = -std::numeric_limits<double>::infinity(); });
  requireClassifiedMutation(input, result, CsrTimingResultViolationClass::kIdMapping, "ID mapping class",
                            [](auto& value) { value.paths.predecessor_nodes.values[4] = NodeId{99}; });
  requireClassifiedMutation(input, result, CsrTimingResultViolationClass::kPredecessor, "predecessor class",
                            [](auto& value) { value.nodes.launches.values[1] = 1.0; });

  CsrTimingResult identity_overlap = result;
  identity_overlap.schema_version = "2.0.0";
  identity_overlap.source_identity.fingerprint_schema_version = "2.0.0";
  identity_overlap.source_identity.session_token = 0;
  identity_overlap.source_identity.graph_generation = 1;
  identity_overlap.source_identity.topology_fingerprint.clear();
  const auto identity_errors = detail::validateTimingResultClassified(input, identity_overlap);
  requireClass(identity_errors, CsrTimingResultViolationClass::kSchema, "overlap schema");
  requireClass(identity_errors, CsrTimingResultViolationClass::kSession, "overlap session");
  requireClass(identity_errors, CsrTimingResultViolationClass::kGeneration, "overlap generation");
  requireClass(identity_errors, CsrTimingResultViolationClass::kSourceIdentity, "overlap source identity");

  CsrTimingResult arc_overlap = result;
  arc_overlap.arcs.transition_delays.values.pop_back();
  arc_overlap.arcs.delays.values[0] = std::numeric_limits<double>::quiet_NaN();
  const auto arc_errors = detail::validateTimingResultClassified(input, arc_overlap);
  requireClass(arc_errors, CsrTimingResultViolationClass::kShape, "arc overlap shape");
  requireClass(arc_errors, CsrTimingResultViolationClass::kNumeric, "arc overlap numeric");

  const auto legacy_identity_errors = validateTimingResult(input, identity_overlap);
  require(legacy_identity_errors.size() == identity_errors.size(), "legacy projection size mismatch");
  for (std::size_t index = 0; index < identity_errors.size(); ++index) {
    require(legacy_identity_errors[index].field == identity_errors[index].field, "legacy projection field mismatch");
    require(legacy_identity_errors[index].message == identity_errors[index].message, "legacy projection message mismatch");
  }

  CsrReferenceInput malformed = input;
  malformed.schema_version = "2.0.0";
  CsrTimingResult malformed_result = result;
  malformed_result.schema_version = "2.0.0";
  const auto malformed_errors = validateTimingResult(malformed, malformed_result);
  require(malformed_errors.size() == 1 && malformed_errors.front().field == "input"
              && malformed_errors.front().message == "cannot validate a result against malformed reference input",
          "malformed input legacy behavior must remain exact");

  CsrReferenceInput unsupported = clockInput();
  unsupported.clocks[0].propagated = true;
  unsupported.unsupported_reasons = {CsrReferenceUnsupportedReason::kPropagatedClock};
  refreshIdentity(unsupported);
  CsrTimingResult unsupported_result = result;
  unsupported_result.schema_version = "2.0.0";
  const auto unsupported_errors = validateTimingResult(unsupported, unsupported_result);
  require(unsupported_errors.size() == 1 && unsupported_errors.front().field == "input.unsupported_reasons"
              && unsupported_errors.front().message == "unsupported reference input must not produce a timing result",
          "unsupported input legacy behavior must remain exact");
}

}  // namespace

int main()
{
  testNativeEquivalentBaseline();
  testIdentityLevelsAndNodeState();
  testClockDataAndPathState();
  testArcMapsAndTimingRows();
  testClockSourceNamePriority();
  testComputedPhysicalClockMutations();
  testTypedClassificationAndLegacyProjection();
  return 0;
}
