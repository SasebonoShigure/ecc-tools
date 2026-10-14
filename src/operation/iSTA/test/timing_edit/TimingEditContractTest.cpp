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
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditTrace.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

std::uint64_t nextXorshift64(std::uint64_t& state) noexcept
{
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

template <typename T>
void xorshiftFisherYates(std::vector<T>& values, std::uint64_t seed)
{
  std::uint64_t state = seed;
  for (std::size_t remaining = values.size(); remaining > 1; --remaining) {
    const std::size_t selected = static_cast<std::size_t>(
        nextXorshift64(state) % static_cast<std::uint64_t>(remaining));
    std::swap(values[remaining - 1], values[selected]);
  }
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

csr::NodeKey port(std::string name)
{
  return csr::NodeKey{csr::NodeKind::kPort, std::move(name), {}, {}};
}

csr::NodeKey pin(std::string instance, std::string name)
{
  return csr::NodeKey{csr::NodeKind::kInstancePin, instance + ":" + name, std::move(instance), std::move(name)};
}

csr::LibraryArcSignature signature()
{
  csr::LibraryArcSignature value;
  value.library_name = "fixture.lib";
  value.arc_index = 0;
  value.sense = csr::TimingSense::kPositiveUnate;
  value.trigger_transition = csr::Transition::kNone;
  value.check_transition = csr::Transition::kNone;
  return value;
}

LibraryCellRef bufferCell()
{
  return LibraryCellRef{"fixture.lib", "BUF_X1", "A", "Y", {signature()}};
}

ComponentFingerprints fingerprints(char value)
{
  const std::string digest(64, value);
  return ComponentFingerprints{csr::kCsrFingerprintSchemaVersion, digest, digest, digest, digest, digest, digest};
}

struct Fixture
{
  csr::NodeKey driver{port("in")};
  csr::NodeKey sink{pin("u1", "A")};
  csr::ArcKey old_arc;
  BaselineSemanticIdentity baseline;
  CreationToken token_value;
  BufferInsertionMutation edit_value;

  Fixture()
  {
    old_arc.base.source = driver;
    old_arc.base.sink = sink;
    old_arc.base.kind = csr::ArcKind::kNet;
    old_arc.base.owner_name = "n0";
    baseline.design_name = "contract_fixture";
    baseline.fingerprints = fingerprints('a');
    baseline.canonical_pre_state.instances = {"u1"};
    baseline.canonical_pre_state.nets = {"n0"};
    baseline.canonical_pre_state.nodes = csr::sortedUniqueNodeKeys({driver, sink});
    baseline.canonical_pre_state.arcs = {old_arc};
    token_value = CreationToken{1, "case", "profile", 42, 1, 0};
    edit_value = BufferInsertionMutation{ExistingNetRef{"n0"},
                                         {ExistingNodeRef{sink}},
                                         bufferCell(),
                                         UnplacedInputs{},
                                         NoParasiticRcInputs{},
                                         token_value};
  }
};

TimingGraphDelta normalized(TimingGraphDelta value)
{
  auto result = normalizeTimingGraphDelta(std::move(value));
  require(std::holds_alternative<TimingGraphDelta>(result), "delta normalization failed");
  return std::get<TimingGraphDelta>(std::move(result));
}

std::pair<TimingGraphDelta, TimingGraphDelta> reconnectDeltas(const Fixture& fixture)
{
  StableNodeRef sink_ref{ExistingNodeRef{fixture.sink}};
  StableNetRef old_net{ExistingNetRef{"n0"}};
  StableNetRef new_net{CreatedNetRef{fixture.token_value, CreatedNetRole::kOutputNet}};
  StableArcRef old_arc{ExistingArcRef{fixture.old_arc}};
  StableArcRef new_arc{CreatedNewNetArcRef{fixture.token_value,
                                           StableNodeRef{CreatedNodeRef{fixture.token_value, CreatedNodeRole::kOutput}},
                                           sink_ref,
                                           0}};
  TimingGraphDelta forward;
  forward.operations = {ReconnectSink{sink_ref, old_net, new_net}, RemoveNetArc{old_arc}, AddNetArc{new_arc}};
  TimingGraphDelta reverse;
  reverse.operations = {ReconnectSink{sink_ref, new_net, old_net}, RemoveNetArc{new_arc}, AddNetArc{old_arc}};
  return {normalized(std::move(forward)), normalized(std::move(reverse))};
}

PreparedBatchData preparedData(const Fixture& fixture)
{
  auto [forward, reverse] = reconnectDeltas(fixture);
  PreparedBatchData data;
  data.session_token = TimingEditSessionToken{7};
  data.expected_generation = TransactionGeneration{0};
  data.target_generation = TransactionGeneration{1};
  data.baseline = std::make_shared<const BaselineSemanticIdentity>(fixture.baseline);
  data.expected_pre_committed_identity = CommittedEditIdentity{TransactionGeneration{0}, fixture.baseline.fingerprints,
                                                               fixture.baseline.canonical_pre_state};
  data.normalized_edits = {fixture.edit_value};
  data.normalized_forward = std::move(forward);
  data.normalized_reverse = std::move(reverse);
  data.token_materializations = {defaultTokenMaterialization(fixture.token_value, fixture.edit_value.library_cell)};
  data.policy_receipt = PolicyReceipt{EditDisposition::kSupported, {}};
  return data;
}

TimingEditTrace trace(const Fixture& fixture)
{
  TimingEditTrace value;
  value.workload_kind = TraceWorkloadKind::kSynthetic;
  value.seed = 42;
  value.profile = "profile";
  value.baseline = fixture.baseline;
  TimingEditTraceBatch batch;
  batch.ordinal = 1;
  batch.expected_pre_identity = CommittedEditIdentity{TransactionGeneration{0}, fixture.baseline.fingerprints,
                                                       fixture.baseline.canonical_pre_state};
  batch.edits = {fixture.edit_value};
  batch.policy_receipt = PolicyReceipt{EditDisposition::kSupported, {}};
  batch.expected_post_identity = CommittedEditIdentity{TransactionGeneration{1}, fingerprints('b'), fixture.baseline.canonical_pre_state};
  value.batches = {std::move(batch)};
  value.replay_command = ReplayCommand{"ista_timing_edit_replay", {"--trace", "{trace}", "--prefix", "{prefix}"}};
  return value;
}

void testStableReferenceFamiliesAreSplit()
{
  static_assert(std::variant_size_v<StableNodeRef> == 2);
  static_assert(std::variant_size_v<StableNetRef> == 2);
  static_assert(std::variant_size_v<StableArcRef> == 4);
  static_assert(!std::is_copy_constructible_v<PreparedBatch>);
  static_assert(std::is_nothrow_move_constructible_v<PreparedBatch>);
  static_assert(std::is_same_v<PlacementInputs, std::variant<UnplacedInputs, PlacedInputs>>);
  static_assert(std::is_same_v<RcInputs, std::variant<NoParasiticRcInputs, ParasiticRcInputs>>);

  Fixture fixture;
  std::map<CreationToken, int> tokens;
  std::map<StableNodeRef, int> nodes;
  std::map<StableNetRef, int> nets;
  std::map<StableArcRef, int> arcs;
  std::map<LibraryCellRef, int> cells;
  tokens.emplace(fixture.token_value, 1);
  nodes.emplace(StableNodeRef{ExistingNodeRef{fixture.sink}}, 1);
  nets.emplace(StableNetRef{ExistingNetRef{"n0"}}, 1);
  arcs.emplace(StableArcRef{ExistingArcRef{fixture.old_arc}}, 1);
  cells.emplace(bufferCell(), 1);
  require(tokens.size() == 1 && nodes.size() == 1 && nets.size() == 1 && arcs.size() == 1 && cells.size() == 1,
          "stable keys are not usable as ordered-map keys");
}

void testPlanNormalizationAndStrictTokenSchema()
{
  Fixture fixture;
  DesignEditPlan canonical{.baseline = fixture.baseline,
                           .expected_generation = TransactionGeneration{0},
                           .edits = {fixture.edit_value}};
  for (const char* instance : {"u2", "u3", "u4", "u5", "u6"}) {
    canonical.edits[0].moved_sinks.emplace_back(ExistingNodeRef{pin(instance, "A")});
  }
  const auto normalized_canonical = normalizeDesignEditPlan(canonical);
  require(std::holds_alternative<DesignEditPlan>(normalized_canonical),
          "canonical fixed-seed plan did not normalize");
  const DesignEditPlan& expected = std::get<DesignEditPlan>(normalized_canonical);
  constexpr std::array<std::uint64_t, 5> kSeeds{1, 7, 42, 20260831, 0xdeadbeefULL};
  for (const std::uint64_t seed : kSeeds) {
    DesignEditPlan permuted = canonical;
    xorshiftFisherYates(permuted.edits[0].moved_sinks, seed);
    const auto normalized_permuted = normalizeDesignEditPlan(permuted);
    require(std::holds_alternative<DesignEditPlan>(normalized_permuted),
            "fixed-seed input permutation did not normalize");
    require(std::get<DesignEditPlan>(normalized_permuted) == expected,
            "fixed-seed input permutation changed normalized fields");
    require(canonicalDesignEditPlanBytes(permuted)
                == canonicalDesignEditPlanBytes(canonical),
            "fixed-seed input permutation changed canonical bytes");
  }

  canonical.edits[0].creation_token.schema_version = 2;
  auto rejected = normalizeDesignEditPlan(canonical);
  require(std::holds_alternative<TimingEditContractViolations>(rejected), "unknown creation token schema was accepted");
}

void testGenerationOverflowIsRejected()
{
  Fixture fixture;
  DesignEditPlan plan{.baseline = fixture.baseline,
                      .expected_generation = TransactionGeneration{
                          std::numeric_limits<std::uint64_t>::max()},
                      .edits = {fixture.edit_value}};
  const auto normalized_plan = normalizeDesignEditPlan(std::move(plan));
  require(std::holds_alternative<TimingEditContractViolations>(normalized_plan),
          "generation overflow was accepted");
  const auto& errors = std::get<TimingEditContractViolations>(normalized_plan);
  require(std::ranges::any_of(errors, [](const TimingEditContractViolation& error) {
            return error.code == TimingEditContractCode::kCapacityOverflow
                   && error.field == "plan.expected_generation";
          }),
          "generation overflow did not report the typed capacity gate");
}

void testTaggedPlacementAndRcEncoding()
{
  Fixture fixture;
  auto unplaced = fixture.edit_value;
  auto placed = fixture.edit_value;
  placed.placement_inputs = PlacedInputs{1.0, 2.0};
  require(canonicalBufferInsertionMutationBytes(unplaced) != canonicalBufferInsertionMutationBytes(placed), "placement tags alias");
  auto parasitic = fixture.edit_value;
  parasitic.rc_inputs = ParasiticRcInputs{ParasiticEditPolicy::kFullWithRecord};
  require(canonicalBufferInsertionMutationBytes(unplaced) != canonicalBufferInsertionMutationBytes(parasitic), "RC tags alias");
  auto pre_route = parasitic;
  pre_route.rc_inputs = ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree};
  require(canonicalBufferInsertionMutationBytes(pre_route) != canonicalBufferInsertionMutationBytes(parasitic),
          "prepared pre-route trees alias full-rebuild fallback");
  require(!validateEditBatch({TransactionGeneration{0}, {pre_route}}).empty(),
          "pre-route edit accepted absent placement");
  pre_route.placement_inputs = PlacedInputs{1.0, 2.0};
  require(validateEditBatch({TransactionGeneration{0}, {pre_route}}).empty(),
          "placed pre-route intent was rejected");

}

void testReverseRestoresBaselineArcAndSealIsPrivate()
{
  Fixture fixture;
  auto mutable_baseline = std::make_shared<BaselineSemanticIdentity>(fixture.baseline);
  auto data = preparedData(fixture);
  data.baseline = mutable_baseline;
  PreparedBatch batch = PreparedBatch::create(std::move(data));
  mutable_baseline->canonical_pre_state.nodes.clear();
  mutable_baseline->fingerprints.topology += "-mutated";
  require(batch.baseline() == fixture.baseline, "public prepared batch retained a mutable baseline alias");
  auto missing_baseline = preparedData(fixture);
  missing_baseline.baseline.reset();
  requireInvalid([&] { static_cast<void>(PreparedBatch::create(std::move(missing_baseline))); },
                 "public prepared batch accepted absent baseline");
  auto missing_identity = preparedData(fixture);
  missing_identity.expected_pre_committed_identity.reset();
  requireInvalid([&] { static_cast<void>(PreparedBatch::create(std::move(missing_identity))); },
                 "public prepared batch accepted absent identity evidence");

  require(std::holds_alternative<ExistingArcRef>(std::get<AddNetArc>(
              *std::find_if(batch.reverseDelta().operations.begin(), batch.reverseDelta().operations.end(),
                            [](const TimingGraphEdit& op) { return std::holds_alternative<AddNetArc>(op); }))
                                                    .arc),
          "reverse delta did not retain baseline arc identity");

  auto broken = preparedData(fixture);
  auto& reverse = broken.normalized_reverse.operations;
  reverse.erase(std::find_if(reverse.begin(), reverse.end(), [](const TimingGraphEdit& op) {
    const auto* add = std::get_if<AddNetArc>(&op);
    return add && std::holds_alternative<ExistingArcRef>(add->arc);
  }));
  broken.normalized_reverse = normalized(std::move(broken.normalized_reverse));
  requireInvalid([&] { static_cast<void>(PreparedBatch::create(std::move(broken))); }, "incomplete reverse delta was accepted");
}

void testUnknownPolicyDispositionIsRejected()
{
  Fixture fixture;
  PreparedBatchData forged = preparedData(fixture);
  forged.policy_receipt.disposition = static_cast<EditDisposition>(255);
  requireInvalid(
      [&] {
        static_cast<void>(PreparedBatch::create(std::move(forged)));
      },
      "unknown policy disposition was accepted");
}

void testClosedWorldTraceRoundTrip()
{
  Fixture fixture;
  TimingEditTrace value = trace(fixture);
  require(validateTimingEditTrace(value).empty(), "valid trace failed validation");
  const std::string wire = timingEditTraceToJson(value);
  auto parsed = parseTimingEditTraceJson(wire);
  require(std::holds_alternative<TimingEditTrace>(parsed), "serialized trace did not parse");
  require(std::get<TimingEditTrace>(parsed) == value, "trace JSON round trip changed typed value");
  require(canonicalTimingEditTraceBytes(std::get<TimingEditTrace>(parsed)) == canonicalTimingEditTraceBytes(value),
          "trace round trip changed canonical bytes");

  std::string duplicate_key = wire;
  const std::string seed_member = "\"seed\":42";
  const auto seed_location = duplicate_key.find(seed_member);
  require(seed_location != std::string::npos, "trace seed member missing from JSON");
  duplicate_key.insert(seed_location, seed_member + ",");
  const auto duplicate_key_result = parseTimingEditTraceJson(duplicate_key);
  require(std::holds_alternative<TimingEditContractViolations>(duplicate_key_result),
          "duplicate top-level trace key was accepted");
  const auto& duplicate_errors
      = std::get<TimingEditContractViolations>(duplicate_key_result);
  require(std::ranges::any_of(duplicate_errors, [](const TimingEditContractViolation& error) {
            return error.message.find("duplicate object key") != std::string::npos;
          }),
          "duplicate trace key did not report the duplicate-key parser gate");

  std::string unknown_key = wire;
  unknown_key.replace(0, 1, "{\"future_field\":true,");
  auto unknown_key_result = parseTimingEditTraceJson(unknown_key);
  require(std::holds_alternative<TimingEditContractViolations>(unknown_key_result), "unknown trace field was accepted");

  std::string unknown = wire;
  unknown.replace(0, 1, "{\"dense_node_id\":0,");
  auto unknown_result = parseTimingEditTraceJson(unknown);
  require(std::holds_alternative<TimingEditContractViolations>(unknown_result), "unknown dense ID field was accepted");

  std::string version = wire;
  const auto location = version.find(kTimingEditTraceSchema);
  require(location != std::string::npos, "trace schema missing from JSON");
  version.replace(location, std::string(kTimingEditTraceSchema).size(), "timing-edit-trace/2.0.0");
  auto version_result = parseTimingEditTraceJson(version);
  require(std::holds_alternative<TimingEditContractViolations>(version_result), "unknown trace version was accepted");
}

}  // namespace

int main()
{
  testStableReferenceFamiliesAreSplit();
  testPlanNormalizationAndStrictTokenSchema();
  testGenerationOverflowIsRejected();
  testTaggedPlacementAndRcEncoding();
  testReverseRestoresBaselineArcAndSealIsPrivate();
  testUnknownPolicyDispositionIsRejected();
  testClosedWorldTraceRoundTrip();
  return 0;
}
