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
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "CsrFingerprint.hpp"
#include "TimingSemanticKey.hpp"

namespace {

using namespace ista::csr;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Callable>
void requireInvalid(Callable callable, const std::string& message)
{
  try {
    callable();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

std::string bytesToHex(const std::vector<std::uint8_t>& bytes)
{
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

bool bytesLess(const std::vector<std::uint8_t>& lhs, const std::vector<std::uint8_t>& rhs)
{
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

NodeKey port(std::string name)
{
  return NodeKey{NodeKind::kPort, std::move(name), {}, {}};
}

NodeKey instancePin(std::string full_name, std::string instance_name, std::string pin_name)
{
  return NodeKey{NodeKind::kInstancePin, std::move(full_name), std::move(instance_name), std::move(pin_name)};
}

LibraryArcSignature librarySignature()
{
  LibraryArcSignature signature;
  signature.library_name = "slow.lib";
  signature.arc_index = -1;
  signature.sense = TimingSense::kNoneUsesPositiveFallback;
  signature.trigger_transition = Transition::kRise;
  signature.check_transition = Transition::kNone;
  signature.delay_tables[0] = {true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance};
  signature.slew_tables[1] = {true, TableVariable::kConstrainedTransition, TableVariable::kNone};
  return signature;
}

ArcKeyBase cellArcBase(std::int32_t arc_index)
{
  auto signature = librarySignature();
  signature.arc_index = arc_index;
  return ArcKeyBase{.source = instancePin("u1:A", "u1", "A"),
                    .sink = instancePin("u1:Y", "u1", "Y"),
                    .kind = ArcKind::kCell,
                    .owner_name = "u1",
                    .library_source_port = "A",
                    .library_sink_port = "Y",
                    .library_arcs = {signature}};
}

CsrSnapshotIdentity identity()
{
  FingerprintPayloads payloads;
  payloads.topology = {1};
  payloads.clock_constraint = {2};
  payloads.library = {3};
  payloads.corner = {4};
  payloads.parasitic = {5};
  payloads.timing_input = {6};
  return makeSnapshotIdentity(7, 0, payloads);
}

template <typename T>
OptionalSoa<T> makeSoa(std::size_t size, std::uint8_t presence = 0, T value = {})
{
  return {std::vector<std::uint8_t>(size, presence), std::vector<T>(size, value)};
}

CsrReferenceInput validInput()
{
  CsrReferenceInput input;
  input.source_identity = identity();
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.nodes = {NodeRecord{.id = NodeId{0}, .key = port("in"), .direction = PinDirection::kInput, .is_port = true, .is_startpoint = true},
                 NodeRecord{.id = NodeId{1}, .key = port("out"), .direction = PinDirection::kOutput, .is_port = true, .is_endpoint = true}};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}, {input.nodes[1].key, NodeId{1}}};

  ArcKeyBase base{.source = input.nodes[0].key, .sink = input.nodes[1].key, .kind = ArcKind::kNet, .owner_name = "n1"};
  input.arcs = {ArcRecord{
      .id = ArcId{0}, .key = ArcKey{base, 0}, .source = NodeId{0}, .sink = NodeId{1}, .kind = ArcKind::kNet, .eligibility = {true, true, true, false}}};
  input.arc_ids_by_key = {{input.arcs[0].key, ArcId{0}}};
  input.out_row_offsets = {0, 1, 1};
  input.out_arc_ids = {ArcId{0}};
  input.in_row_offsets = {0, 0, 1};
  input.in_arc_ids = {ArcId{0}};
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids = {NodeId{1}};
  input.clock_tags = {"seed"};
  StartSeedModel seed;
  seed.seed_node = NodeId{0};
  seed.path_start_node = NodeId{0};
  seed.path_source = PathSource::kInput;
  seed.clock_tag = "seed";
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      seed.arrival[analysis][transition] = 0.0;
      seed.slew[analysis][transition] = 0.1;
    }
  input.start_seeds = {seed};
  return input;
}

CsrTimingResult validResult(const CsrReferenceInput& input)
{
  const std::size_t nodes = input.nodes.size();
  const std::size_t arcs = input.arcs.size();
  const std::size_t node_lanes = nodes * kAnalysisCount * kTransitionCount;
  const std::size_t path_lanes = node_lanes * kPathSourceCount;
  const std::size_t arc_transition_lanes = arcs * kAnalysisCount * kTransitionCount;
  const std::size_t arc_io_lanes = arc_transition_lanes * kTransitionCount;

  CsrTimingResult result;
  result.source_identity = input.source_identity;
  result.level_offsets = {0, 1, 2};
  result.level_nodes = {NodeId{0}, NodeId{1}};
  result.node_to_level = {1, 2};
  result.timing_order = result.level_nodes;
  result.nodes.is_clock_point = makeSoa<std::uint8_t>(nodes, 1, 0);
  result.nodes.arrivals = makeSoa<double>(nodes, 1, -std::numeric_limits<double>::infinity());
  result.nodes.requireds = makeSoa<double>(nodes, 1, std::numeric_limits<double>::infinity());
  result.nodes.slacks = makeSoa<double>(nodes, 1, 0.0);
  result.nodes.launches = makeSoa<double>(nodes, 1, 0.0);
  result.nodes.clock_names = makeSoa<std::string>(nodes);
  result.nodes.predecessor_nodes = makeSoa<NodeId>(nodes);
  result.nodes.predecessor_arcs = makeSoa<ArcId>(nodes);

  result.effective_clock.arrivals = makeSoa<double>(node_lanes);
  result.effective_clock.slews = makeSoa<double>(node_lanes);
  result.effective_clock.predecessor_nodes = makeSoa<NodeId>(node_lanes);
  result.effective_clock.predecessor_arc_delays = makeSoa<double>(node_lanes);
  result.effective_clock.predecessor_transitions = makeSoa<Transition>(node_lanes, 0, Transition::kNone);
  result.physical_clock = result.effective_clock;
  result.data_slews = makeSoa<double>(node_lanes);
  result.paths.row_offsets.assign(path_lanes + 1, 0);

  result.arcs.delays = makeSoa<double>(arcs, 1, 0.0);
  result.arcs.min_delays = makeSoa<double>(arcs, 1, 0.0);
  result.arcs.max_delays = makeSoa<double>(arcs, 1, 0.0);
  result.arcs.transition_delays = makeSoa<double>(arc_transition_lanes);
  result.arcs.input_output_delays = makeSoa<double>(arc_io_lanes);
  result.arcs.graph_delays = makeSoa<double>(arc_io_lanes);
  result.arcs.transition_types = makeSoa<Transition>(arcs * kTransitionCount, 0, Transition::kNone);
  result.arcs.timing_arc_row_offsets.assign(arcs + 1, 0);
  return result;
}

template <typename Enum>
void requireTokens(std::initializer_list<std::pair<Enum, std::string_view>> expected, std::string_view family)
{
  for (const auto& [value, expected_token] : expected)
    require(token(value) == expected_token, std::string(family) + " token mismatch");
  requireInvalid([&] { static_cast<void>(token(static_cast<Enum>(0xff))); }, std::string(family) + " unknown enum");
}

void testAllEnumTokens()
{
  requireTokens<NodeKind>({{NodeKind::kPort, "port"}, {NodeKind::kInstancePin, "instance_pin"}}, "NodeKind");
  requireTokens<ArcKind>({{ArcKind::kCell, "cell"}, {ArcKind::kNet, "net"}}, "ArcKind");
  requireTokens<PinDirection>(
      {{PinDirection::kNone, "none"}, {PinDirection::kInput, "input"}, {PinDirection::kOutput, "output"}, {PinDirection::kInout, "inout"}}, "PinDirection");
  requireTokens<AnalysisMode>({{AnalysisMode::kMax, "max"}, {AnalysisMode::kMin, "min"}}, "AnalysisMode");
  requireTokens<Transition>({{Transition::kRise, "rise"}, {Transition::kFall, "fall"}, {Transition::kNone, "none"}}, "Transition");
  requireTokens<PathSource>({{PathSource::kInput, "input"}, {PathSource::kRegister, "register"}}, "PathSource");
  requireTokens<StartSeedAvailability>(
      {{StartSeedAvailability::kMaterialized, "materialized"}, {StartSeedAvailability::kDeferredSequential, "deferred_sequential"}},
      "StartSeedAvailability");
  requireTokens<TimingCheckKind>({{TimingCheckKind::kNone, "none"},
                                  {TimingCheckKind::kSetup, "setup"},
                                  {TimingCheckKind::kHold, "hold"},
                                  {TimingCheckKind::kRecovery, "recovery"},
                                  {TimingCheckKind::kRemoval, "removal"},
                                  {TimingCheckKind::kWidth, "width"},
                                  {TimingCheckKind::kPeriod, "period"}},
                                 "TimingCheckKind");
  requireTokens<TimingSense>({{TimingSense::kNoneUsesPositiveFallback, "none_positive_fallback"},
                              {TimingSense::kPositiveUnate, "positive_unate"},
                              {TimingSense::kNegativeUnate, "negative_unate"},
                              {TimingSense::kNonUnate, "non_unate"}},
                             "TimingSense");
  requireTokens<TableVariable>({{TableVariable::kNone, "none"},
                                {TableVariable::kInputTransition, "input_transition"},
                                {TableVariable::kOutputCapacitance, "output_capacitance"},
                                {TableVariable::kConstrainedTransition, "constrained_transition"}},
                               "TableVariable");
  requireTokens<CsrCaptureCode>({{CsrCaptureCode::kMalformedTopology, "malformed_topology"},
                                 {CsrCaptureCode::kSemanticKeyCollision, "semantic_key_collision"},
                                 {CsrCaptureCode::kIllegalNumeric, "illegal_numeric"},
                                 {CsrCaptureCode::kInvalidTableShape, "invalid_table_shape"},
                                 {CsrCaptureCode::kInvalidAnalysisContext, "invalid_analysis_context"},
                                 {CsrCaptureCode::kActiveCycle, "active_cycle"}},
                                "CsrCaptureCode");
  requireTokens<CsrReferenceUnsupportedReason>({{CsrReferenceUnsupportedReason::kParasiticNet, "parasitic_net"},
                                                {CsrReferenceUnsupportedReason::kSequentialInstance, "sequential_instance"},
                                                {CsrReferenceUnsupportedReason::kSequentialLaunch, "sequential_launch"},
                                                {CsrReferenceUnsupportedReason::kTimingCheck, "timing_check"},
                                                {CsrReferenceUnsupportedReason::kCrpr, "crpr"},
                                                {CsrReferenceUnsupportedReason::kPropagatedClock, "propagated_clock"},
                                                {CsrReferenceUnsupportedReason::kClockTreeFanout, "clock_tree_fanout"},
                                                {CsrReferenceUnsupportedReason::kClockGatingOrDivider, "clock_gating_or_divider"},
                                                {CsrReferenceUnsupportedReason::kConditionalTimingArc, "conditional_timing_arc"},
                                                {CsrReferenceUnsupportedReason::kCaseAnalysis, "case_analysis"}},
                                               "CsrReferenceUnsupportedReason");
  requireTokens<CsrComputationCode>({{CsrComputationCode::kCycle, "cycle"},
                                     {CsrComputationCode::kInvariantViolation, "invariant_violation"},
                                     {CsrComputationCode::kNumericFailure, "numeric_failure"}},
                                    "CsrComputationCode");
  requireTokens<CsrScatterCode>({{CsrScatterCode::kSuccess, "success"},
                                 {CsrScatterCode::kInvalidState, "invalid_state"},
                                 {CsrScatterCode::kSnapshotInvalidated, "snapshot_invalidated"},
                                 {CsrScatterCode::kSchemaMismatch, "schema_mismatch"},
                                 {CsrScatterCode::kSessionMismatch, "session_mismatch"},
                                 {CsrScatterCode::kGenerationMismatch, "generation_mismatch"},
                                 {CsrScatterCode::kSourceIdentityMismatch, "source_identity_mismatch"},
                                 {CsrScatterCode::kShapeMismatch, "shape_mismatch"},
                                 {CsrScatterCode::kResultNumericInvalid, "result_numeric_invalid"},
                                 {CsrScatterCode::kIdMappingMismatch, "id_mapping_mismatch"},
                                 {CsrScatterCode::kPredecessorMismatch, "predecessor_mismatch"}},
                                "CsrScatterCode");
  requireTokens<CsrInvalidationReason>({{CsrInvalidationReason::kTopologyChanged, "topology_changed"},
                                        {CsrInvalidationReason::kClockConstraintChanged, "clock_constraint_changed"},
                                        {CsrInvalidationReason::kLibraryChanged, "library_changed"},
                                        {CsrInvalidationReason::kCornerChanged, "corner_changed"},
                                        {CsrInvalidationReason::kParasiticChanged, "parasitic_changed"},
                                        {CsrInvalidationReason::kTimingInputChanged, "timing_input_changed"}},
                                       "CsrInvalidationReason");
  requireTokens<CsrSnapshotState>(
      {{CsrSnapshotState::kReady, "ready"}, {CsrSnapshotState::kInvalidated, "invalidated"}, {CsrSnapshotState::kReleased, "released"}}, "CsrSnapshotState");
  requireTokens<CapacityAction>({{CapacityAction::kInitialAllocate, "initial_allocate"}}, "CapacityAction");
}

void testNodeKeyValidation()
{
  const NodeKey valid = instancePin("u0:A", "u0", "A");
  require(!validateNodeKey(valid), "valid colon-delimited instance-pin NodeKey was rejected");

  NodeKey inconsistent = valid;
  inconsistent.full_name = "u0/A";
  const auto error = validateNodeKey(inconsistent);
  require(error && *error == "instance-pin full_name must equal instance_name + ':' + pin_name",
          "slash-delimited instance-pin full_name used the wrong validation error");
}

void testSemanticKeyKnownAnswers()
{
  const auto node_bytes = canonicalNodeKeyBytes(instancePin("u1:A", "u1", "A"));
  const std::string node_hex
      = "0000000000000008697374612d63737200000000000000086e6f64652d6b65790000000000000005312e302e30000000000000000c696e73"
        "74616e63655f70696e000000000000000475313a4100000000000000027531000000000000000141";
  require(bytesToHex(node_bytes) == node_hex, "NodeKey canonical known answer");

  const auto signature_bytes = canonicalLibraryArcSignatureBytes(librarySignature());
  const std::string signature_hex
      = "0000000000000008697374612d63737200000000000000156c6962726172792d6172632d7369676e61747572650000000000000005312e30"
        "2e300000000000000008736c6f772e6c6962ffffffff00000000000000166e6f6e655f706f7369746976655f66616c6c6261636b00000000"
        "000000047269736500000000000000046e6f6e650000000000000000010000000000000010696e7075745f7472616e736974696f6e000000"
        "00000000126f75747075745f63617061636974616e63650000000000000000046e6f6e6500000000000000046e6f6e650000000000000000"
        "046e6f6e6500000000000000046e6f6e65010000000000000016636f6e73747261696e65645f7472616e736974696f6e0000000000000004"
        "6e6f6e65";
  require(bytesToHex(signature_bytes) == signature_hex, "library signature canonical known answer");

  const auto arc_bytes = canonicalArcKeyBytes(ArcKey{cellArcBase(-1), 3});
  const std::string arc_hex
      = "0000000000000008697374612d63737200000000000000076172632d6b65790000000000000005312e302e30000000000000000c696e7374"
        "616e63655f70696e000000000000000475313a4100000000000000027531000000000000000141000000000000000c696e7374616e63655f"
        "70696e000000000000000475313a5900000000000000027531000000000000000159000000000000000463656c6c00000000000000027531"
        "00000000000000014100000000000000015900000000000000010000000000000008736c6f772e6c6962ffffffff00000000000000166e6f"
        "6e655f706f7369746976655f66616c6c6261636b00000000000000047269736500000000000000046e6f6e65000000000000000001000000"
        "0000000010696e7075745f7472616e736974696f6e00000000000000126f75747075745f63617061636974616e6365000000000000000004"
        "6e6f6e6500000000000000046e6f6e650000000000000000046e6f6e6500000000000000046e6f6e65010000000000000016636f6e737472"
        "61696e65645f7472616e736974696f6e00000000000000046e6f6e650000000000000003";
  require(bytesToHex(arc_bytes) == arc_hex, "ArcKey canonical known answer");
}

void testCanonicalOrderingCounterexamples()
{
  const NodeKey port_key = port("p");
  const NodeKey instance_key = instancePin("u:A", "u", "A");
  require(bytesLess(canonicalNodeKeyBytes(port_key), canonicalNodeKeyBytes(instance_key)), "port token byte order");
  require(nodeKeyLess(port_key, instance_key), "port comparator order");

  const NodeKey z = port("z");
  const NodeKey aa = port("aa");
  require(bytesLess(canonicalNodeKeyBytes(z), canonicalNodeKeyBytes(aa)), "length-prefixed z/aa byte order");
  require(nodeKeyLess(z, aa), "z/aa comparator must follow bytes");

  ArcKeyBase net_base{.source = port("a"), .sink = port("b"), .kind = ArcKind::kNet, .owner_name = "owner"};
  ArcKeyBase cell_base = net_base;
  cell_base.kind = ArcKind::kCell;
  cell_base.library_source_port = "A";
  cell_base.library_sink_port = "Y";
  const ArcKey net{net_base, 0};
  const ArcKey cell{cell_base, 0};
  require(bytesLess(canonicalArcKeyBytes(net), canonicalArcKeyBytes(cell)), "net/cell token byte order");
  require(arcKeyLess(net, cell), "net/cell comparator must follow bytes");

  const ArcKey zero_index{cellArcBase(0), 0};
  const ArcKey minus_one_index{cellArcBase(-1), 0};
  require(bytesLess(canonicalArcKeyBytes(zero_index), canonicalArcKeyBytes(minus_one_index)), "signed arc-index bit order");
  require(arcKeyLess(zero_index, minus_one_index), "arc-index comparator must follow bytes");
}

void testLargeInterleavedParallelOrdinals()
{
  constexpr std::size_t kGroups = 257;
  constexpr std::size_t kRepeats = 128;
  std::vector<ArcKeyBase> bases;
  std::vector<std::size_t> group_ids;
  bases.reserve(kGroups * kRepeats);
  group_ids.reserve(kGroups * kRepeats);
  for (std::size_t repeat = 0; repeat < kRepeats; ++repeat) {
    for (std::size_t offset = 0; offset < kGroups; ++offset) {
      const std::size_t group = (offset * 73 + repeat * 17) % kGroups;
      bases.push_back(ArcKeyBase{.source = port("a"), .sink = port("b"), .kind = ArcKind::kNet, .owner_name = "net_" + std::to_string(group)});
      group_ids.push_back(group);
    }
  }

  const auto assigned = assignParallelOrdinals(bases);
  std::array<std::uint64_t, kGroups> next_ordinal{};
  require(assigned.size() == bases.size(), "large ordinal output size");
  for (std::size_t index = 0; index < assigned.size(); ++index) {
    require(assigned[index].base == bases[index], "parallel assignment changed native order");
    require(assigned[index].parallel_ordinal == next_ordinal[group_ids[index]]++, "interleaved parallel ordinal");
  }
  for (std::uint64_t count : next_ordinal)
    require(count == kRepeats, "parallel group cardinality");
}

void testIndependentInstanceRoles()
{
  InstanceRecord instance;
  require(!instance.is_clock_gating && !instance.is_macro, "instance role defaults");
  instance.is_clock_gating = true;
  require(instance.is_clock_gating && !instance.is_macro, "clock-gating role is independent");
  instance.is_clock_gating = false;
  instance.is_macro = true;
  require(!instance.is_clock_gating && instance.is_macro, "macro role is independent");
  StartSeedModel floating_seed;
  require(!floating_seed.path_source, "native kNone path source is explicit");
}

void testTypedOutcomesAndFlatShapes()
{
  const CsrReferenceInput input = validInput();
  CsrTimingResult timing = validResult(input);
  CsrCaptureResult capture_success;
  CsrCaptureResult capture_error{CsrCaptureCode::kActiveCycle};
  require(!capture_success.error && capture_error.error == CsrCaptureCode::kActiveCycle, "capture outcome");

  CsrReferenceSupportResult supported;
  CsrReferenceSupportResult unsupported{{CsrReferenceUnsupportedReason::kParasiticNet}};
  require(supported.unsupported_reasons.empty() && unsupported.unsupported_reasons.size() == 1, "support outcome");

  CsrComputationResult computed{timing};
  CsrComputationResult computation_error{CsrComputationCode::kCycle};
  require(std::holds_alternative<CsrTimingResult>(computed.outcome), "computed outcome");
  require(std::get<CsrComputationCode>(computation_error.outcome) == CsrComputationCode::kCycle, "computation error");

  CsrScatterResult scattered{CsrScatterCode::kSuccess, 1, std::nullopt};
  CsrScatterResult invalidated{CsrScatterCode::kSnapshotInvalidated, 0, CsrInvalidationReason::kTopologyChanged};
  require(scattered.writes_applied == 1 && invalidated.writes_applied == 0, "scatter outcome");

  const std::size_t node_lanes = input.nodes.size() * kAnalysisCount * kTransitionCount;
  const std::size_t path_lanes = node_lanes * kPathSourceCount;
  const std::size_t arc_transition_lanes = input.arcs.size() * kAnalysisCount * kTransitionCount;
  require(timing.effective_clock.arrivals.values.size() == node_lanes, "effective clock shape");
  require(timing.physical_clock.arrivals.values.size() == node_lanes, "physical clock shape");
  require(timing.data_slews.values.size() == node_lanes, "data slew shape");
  require(timing.paths.row_offsets.size() == path_lanes + 1, "path row shape");
  require(timing.arcs.transition_delays.values.size() == arc_transition_lanes, "trans-delay shape");
  require(timing.arcs.input_output_delays.values.size() == arc_transition_lanes * kTransitionCount, "input-output shape");
  require(timing.arcs.graph_delays.values.size() == arc_transition_lanes * kTransitionCount, "graph-delay shape");
  require(timing.arcs.transition_types.values.size() == input.arcs.size() * kTransitionCount, "transition-map shape");
  require(timing.arcs.timing_arc_row_offsets.size() == input.arcs.size() + 1, "timing-arc row shape");

  timing.paths.row_offsets.back() = 1;
  timing.paths.clock_tags = makeSoa<ClockTagId>(1, 1, ClockTagId{0});
  timing.paths.arrivals = makeSoa<double>(1, 1, 0.0);
  timing.paths.slews = makeSoa<double>(1, 1, 0.0);
  timing.paths.launches = makeSoa<double>(1, 1, 0.0);
  timing.paths.start_nodes = makeSoa<NodeId>(1, 1, NodeId{0});
  timing.paths.predecessor_nodes = makeSoa<NodeId>(1);
  timing.paths.predecessor_arcs = makeSoa<ArcId>(1);
  timing.paths.predecessor_arc_delays = makeSoa<double>(1);
  timing.paths.predecessor_transitions = makeSoa<Transition>(1, 0, Transition::kNone);
  timing.paths.crpr_nodes = makeSoa<NodeId>(1);
  timing.paths.crpr_transitions = makeSoa<Transition>(1, 0, Transition::kNone);
  require(timing.paths.start_nodes.presence[0] == 1 && timing.paths.row_offsets.back() == 1, "flat path entry");

  timing.arcs.timing_arc_row_offsets = {0, 1};
  timing.arcs.timing_arc_indices = {-1};
  timing.arcs.timing_arc_delays = makeSoa<double>(kAnalysisCount * kTransitionCount * kTransitionCount);
  timing.arcs.timing_arc_fallbacks = makeSoa<std::uint8_t>(kAnalysisCount * kTransitionCount * kTransitionCount);
  require(timing.arcs.timing_arc_indices[0] == -1, "signed timing-arc key");
}

}  // namespace

int main()
{
  testAllEnumTokens();
  testNodeKeyValidation();
  testSemanticKeyKnownAnswers();
  testCanonicalOrderingCounterexamples();
  testLargeInterleavedParallelOrdinals();
  testIndependentInstanceRoles();
  testTypedOutcomesAndFlatShapes();
  return 0;
}
