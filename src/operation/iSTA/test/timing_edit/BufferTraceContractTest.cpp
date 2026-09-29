// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "BufferTraceCanonicalEncoding.hpp"
#include "BufferTraceJson.hpp"
#include "TimingEditValidation.hpp"

namespace ista::buffer_trace {
void testArtifactLists(ReplayProgram program);
void testDirectValueCodecBoundaries(const BufferTraceArtifact& artifact);
}

namespace {

using namespace ista;
using namespace ista::buffer_trace;

template <typename Value>
concept HasGenerationOnly = requires(Value value) { value.generation_only; };

static_assert(std::variant_size_v<ReplayPayload> == 2);
static_assert(!std::is_same_v<TechLefRecipe, CellLefRecipe>);
static_assert(!std::is_same_v<TechLefSourceRef, CellLefSourceRef>);
static_assert(!HasGenerationOnly<ReplayProgram>);

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

csr::NodeKey pin(std::string instance, std::string name)
{
  return csr::NodeKey{csr::NodeKind::kInstancePin,
                      instance + ":" + name,
                      std::move(instance),
                      std::move(name)};
}

timing_edit::ComponentFingerprints fingerprints(char digit)
{
  const std::string digest(64, digit);
  return timing_edit::ComponentFingerprints{
      csr::kCsrSchemaVersion, digest, digest, digest, digest, digest, digest};
}

timing_edit::BaselineSemanticIdentity baseline()
{
  timing_edit::BaselineSemanticIdentity value;
  value.design_name = "stage4_contract";
  value.fingerprints = fingerprints('a');
  value.canonical_pre_state.instances = {"u0"};
  value.canonical_pre_state.nets = {"fanout"};
  value.canonical_pre_state.nodes = csr::sortedUniqueNodeKeys({pin("u0", "A")});
  return value;
}

timing_edit::LibraryCellRef bufferCell()
{
  csr::LibraryArcSignature arc;
  arc.library_name = "stage4_buffer_lib";
  arc.arc_index = 0;
  arc.sense = csr::TimingSense::kPositiveUnate;
  return timing_edit::LibraryCellRef{
      "stage4_buffer_lib", "BUF_X1", "A", "Y", {std::move(arc)}};
}

timing_edit::CreationToken tokenFor(std::uint64_t batch,
                                    std::uint64_t seed = 42)
{
  return timing_edit::CreationToken{
      .case_id = "stage4_contract",
      .profile_id = "engineering_smoke",
      .seed = seed,
      .batch_ordinal = batch,
      .accepted_ordinal = 0};
}

timing_edit::EditBatch plan(
    std::uint64_t generation, timing_edit::StableNetRef original_net,
    timing_edit::CreationToken creation_token)
{
  return timing_edit::EditBatch{
      .expected_generation = timing_edit::TransactionGeneration{generation},
      .edits = {timing_edit::BufferInsertionMutation{
          .original_net = std::move(original_net),
          .moved_sinks = {
              timing_edit::ExistingNodeRef{pin("u0", "A")}},
          .library_cell = bufferCell(),
          .placement_inputs = timing_edit::UnplacedInputs{},
          .rc_inputs = timing_edit::NoParasiticRcInputs{},
          .creation_token = std::move(creation_token)}}};
}

SyntheticArtifactRecipe syntheticRecipe()
{
  return SyntheticArtifactRecipe{
      .tech_lef = {"tech.lef", "TECH LEF SOURCE"},
      .cell_lef = {"cell.lef", "CELL LEF SOURCE"},
      .liberty = {"stage4.lib", "LIBERTY SOURCE"},
      .verilog = {"design.v", "VERILOG SOURCE"},
      .sdc = {"design.sdc", "SDC SOURCE"},
      .top = "stage4_contract"};
}

ReplayProgram syntheticProgram()
{
  const auto first_token = tokenFor(1);
  const auto second_token = tokenFor(2);
  std::vector<timing_edit::EditBatch> batches;
  batches.push_back(
      plan(0, timing_edit::ExistingNetRef{"fanout"}, first_token));
  batches.push_back(plan(
      1,
      timing_edit::CreatedNetRef{
          first_token, timing_edit::CreatedNetRole::kOutputNet},
      second_token));
  return ReplayProgram{
      .header = ReplayHeader{
          .workload_kind = SemanticWorkloadKind::kSynthetic,
          .profile_id = ProfileId::kEngineeringSmoke,
          .comparability_status = ComparabilityStatus::kNonComparable,
          .comparability_reason =
              ComparabilityReason::kEngineeringRcNoPhysical},
      .workload = SyntheticReplayPayload{syntheticRecipe(), baseline(), std::move(batches)}};
}

ReplayProgram realProgram()
{
  const auto real_token = tokenFor(1, 0);
  return ReplayProgram{
      .header = ReplayHeader{
          .workload_kind = SemanticWorkloadKind::kRealDesign,
          .profile_id = ProfileId::kEngineeringSmoke,
          .comparability_status = ComparabilityStatus::kNonComparable,
          .comparability_reason =
              ComparabilityReason::kEngineeringRcNoPhysical},
      .workload = RealReplayPayload{
          .artifacts = SourceArtifactRefs{
              .tech_lef = {"/fixture/tech.lef"},
              .cell_lefs = {{"/fixture/cell.lef"}},
              .liberties = {{"/fixture/stage4.lib"}},
              .verilog = {"/fixture/design.v"},
              .sdc = {"/fixture/design.sdc"},
              .top = "stage4_buffer_fanout",
              .analysis_context = {"slow", "25", "__no_rc_corner__"}},
          .baseline = baseline(),
          .batches = {plan(
              0, timing_edit::ExistingNetRef{"fanout"}, real_token)}}};
}

GainComputationReceipt numericReceipt()
{
  return GainComputationReceipt{
      .arrival_ns = 1.0,
      .required_ns = 2.0,
      .slack_ns = 1.0,
      .upstream_resistance_kohm = 64.0,
      .candidate_load_pf = 2.0,
      .buffer_capacitance_pf = 1.0,
      .delta_capacitance_pf = 1.0,
      .benefit_ns = 64.0,
      .buffer_resistance_kohm = 1.0,
      .penalty_ns = 1.0,
      .benefit_minus_penalty_ns = 63.0,
      .buffer_delay_ns = 0.25,
      .gain_ns = 62.75};
}

CandidateEvaluationReceipt acceptedEvaluation(
    timing_edit::StableNetRef net,
    timing_edit::StableNodeRef sink,
    const timing_edit::CreationToken& creation_token)
{
  return CandidateEvaluationReceipt{
      .net = std::move(net),
      .driver = timing_edit::ExistingNodeRef{csr::NodeKey{
          csr::NodeKind::kPort, "data_in", {}, {}}},
      .candidate_pin = std::move(sink),
      .buffer = bufferCell(),
      .numeric = numericReceipt(),
      .decision = CandidateDecision::kAccepted,
      .rejection_reason = std::nullopt,
      .accepted_creation_token = creation_token};
}

BufferTraceArtifact syntheticArtifact()
{
  auto replay = syntheticProgram();
  const auto& batches = std::get<SyntheticReplayPayload>(replay.workload).batches;
  GenerationReceipt generation{
      .profile = frozenSelectionProfile(ProfileId::kEngineeringSmoke),
      .rounds = {
          GenerationRoundReceipt{
              .batch_ordinal = 1,
              .evaluations = {acceptedEvaluation(
                  batches[0].edits[0].original_net,
                  batches[0].edits[0].moved_sinks[0],
                  batches[0].edits[0].creation_token)},
              .accepted_tokens = {batches[0].edits[0].creation_token}},
          GenerationRoundReceipt{
              .batch_ordinal = 2,
              .evaluations = {acceptedEvaluation(
                  batches[1].edits[0].original_net,
                  batches[1].edits[0].moved_sinks[0],
                  batches[1].edits[0].creation_token)},
              .accepted_tokens = {batches[1].edits[0].creation_token}}},
      .stop_reason = GenerationStopReason::kRoundsReached};
  return BufferTraceArtifact{
      .replay = std::move(replay),
      .generation_only = std::move(generation)};
}

bool hasCode(const BufferTraceContractViolations& errors,
             BufferTraceContractCode code)
{
  return std::any_of(errors.begin(), errors.end(),
                     [&](const auto& error) { return error.code == code; });
}

void requireNoErrors(const BufferTraceContractViolations& errors,
                     const std::string& message)
{
  if (errors.empty())
    return;
  std::string detail = message;
  for (const auto& error : errors)
    detail += "\n" + std::string(token(error.code)) + ": " + error.field
              + ": " + error.message;
  throw std::runtime_error(detail);
}

void testValidTaggedUnionAndCreatedNetChain()
{
  const auto synthetic = syntheticProgram();
  require(validateReplayProgram(synthetic).empty(),
          "valid synthetic old-to-new/new-to-new replay was rejected");
  require(validateReplayProgram(realProgram()).empty(),
          "valid real non-comparable replay was rejected");
  require(validateBufferTraceArtifact(syntheticArtifact()).empty(),
          "valid two-round artifact was rejected");
}

void testTaggedUnionAndFiveSourceRolesFailClosed()
{
  auto program = syntheticProgram();
  program.header.workload_kind = SemanticWorkloadKind::kRealDesign;
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kInvalidTaggedUnion),
          "mismatched workload discriminator was accepted");

  program = syntheticProgram();
  auto& recipe =
      std::get<SyntheticReplayPayload>(program.workload).source_recipe;
  recipe.cell_lef.logical_name = recipe.tech_lef.logical_name;
  recipe.cell_lef.source_text = recipe.tech_lef.source_text;
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kInvalidSourceArtifacts),
          "aliased synthetic tech/cell LEF roles were accepted");

  program = realProgram();
  auto& artifacts = std::get<RealReplayPayload>(program.workload).artifacts;
  artifacts.cell_lefs.front().path = artifacts.tech_lef.path;
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kInvalidSourceArtifacts),
          "aliased real tech/cell LEF paths were accepted");
}

void testRealComparabilityAndEnvironmentAreExact()
{
  auto program = realProgram();
  program.header.comparability_status = ComparabilityStatus::kExact;
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kInvalidProfile),
          "real engineering-RC trace claimed exact comparability");

  program = realProgram();
  program.header.comparability_reason.reset();
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kInvalidProfile),
          "real engineering-RC trace omitted its non-comparability reason");

  program = realProgram();
  auto& edit = std::get<RealReplayPayload>(program.workload).batches[0].edits[0];
  edit.placement_inputs = timing_edit::PlacedInputs{1.0, 2.0};
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kInvalidValue),
          "real v1 accepted physical placement inputs");
}

void testForwardReferenceAndGenerationOnlyIsolation()
{
  auto program = syntheticProgram();
  auto& first = std::get<SyntheticReplayPayload>(program.workload).batches[0];
  first.edits[0].original_net = timing_edit::CreatedNetRef{
      tokenFor(2), timing_edit::CreatedNetRole::kOutputNet};
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kForwardReference),
          "forward created-net reference was accepted");

  const auto artifact = syntheticArtifact();
  auto tampered = artifact;
  tampered.generation_only.rounds[0]
      .evaluations[0]
      .numeric->gain_ns = 123.0;
  require(canonicalReplayProgramBytes(artifact.replay)
              == canonicalReplayProgramBytes(tampered.replay),
          "generation-only evidence leaked into replay-safe projection");
  require(canonicalGenerationReceiptBytes(artifact.generation_only)
              != canonicalGenerationReceiptBytes(tampered.generation_only),
          "generation evidence mutation was not observable in its own receipt");
}

void testCanonicalTokenAndRoundPoisoning()
{
  auto program = syntheticProgram();
  auto& batches = std::get<SyntheticReplayPayload>(program.workload).batches;
  batches[0].edits[0].creation_token.batch_ordinal = 2;
  require(!validateReplayProgram(program).empty(),
          "edit token with the wrong batch ordinal was accepted");

  program = syntheticProgram();
  auto& first = std::get<SyntheticReplayPayload>(program.workload).batches[0];
  first.edits[0].creation_token.accepted_ordinal = 1;
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kNotCanonical),
          "edit token with a noncanonical accepted ordinal was accepted");

  auto artifact = syntheticArtifact();
  artifact.generation_only.rounds[1].accepted_tokens[0]
      = artifact.generation_only.rounds[0].accepted_tokens[0];
  artifact.generation_only.rounds[1]
      .evaluations[0]
      .accepted_creation_token
      = artifact.generation_only.rounds[0].accepted_tokens[0];
  require(hasCode(validateBufferTraceArtifact(artifact),
                  BufferTraceContractCode::kDuplicate),
          "accepted token reused across generation rounds was accepted");

  artifact = syntheticArtifact();
  std::swap(artifact.generation_only.rounds[0].accepted_tokens[0],
            artifact.generation_only.rounds[1].accepted_tokens[0]);
  std::swap(artifact.generation_only.rounds[0]
                .evaluations[0]
                .accepted_creation_token,
            artifact.generation_only.rounds[1]
                .evaluations[0]
                .accepted_creation_token);
  require(!validateBufferTraceArtifact(artifact).empty(),
          "generation rounds swapped away from replay batches were accepted");
}

void testAcceptedEvaluationPermutationMapsByTokenOrdinal()
{
  auto artifact = syntheticArtifact();
  auto& batches
      = std::get<SyntheticReplayPayload>(artifact.replay.workload).batches;
  auto& expanded_baseline = std::get<SyntheticReplayPayload>(artifact.replay.workload).baseline;
  expanded_baseline.canonical_pre_state.nodes = csr::sortedUniqueNodeKeys(
      {pin("u0", "A"), pin("u0", "B")});
  expanded_baseline.canonical_pre_state.nets = {"fanout", "fanout2"};

  auto second_token = tokenFor(1);
  second_token.accepted_ordinal = 1;
  const auto second_sink = timing_edit::StableNodeRef{
      timing_edit::ExistingNodeRef{pin("u0", "B")}};
  batches[0].edits.push_back(timing_edit::BufferInsertionMutation{
      .original_net = timing_edit::ExistingNetRef{"fanout2"},
      .moved_sinks = {second_sink},
      .library_cell = bufferCell(),
      .placement_inputs = timing_edit::UnplacedInputs{},
      .rc_inputs = timing_edit::NoParasiticRcInputs{},
      .creation_token = second_token});

  auto& round = artifact.generation_only.rounds[0];
  const auto first_evaluation = round.evaluations.front();
  round.evaluations = {
      acceptedEvaluation(batches[0].edits[1].original_net, second_sink,
                         second_token),
      first_evaluation};
  round.accepted_tokens.push_back(second_token);
  requireNoErrors(validateBufferTraceArtifact(artifact),
                  "accepted evaluation permutation was not mapped by token ordinal");
}

void testStrictJsonAndCreatedReferenceRoundTrip()
{
  auto artifact = syntheticArtifact();
  const auto first_token
      = std::get<SyntheticReplayPayload>(artifact.replay.workload)
            .batches[0]
            .edits[0]
            .creation_token;
  artifact.generation_only.rounds[1].evaluations.push_back(
      CandidateEvaluationReceipt{
          .net = timing_edit::CreatedNetRef{
              first_token, timing_edit::CreatedNetRole::kOutputNet},
          .driver = timing_edit::CreatedNodeRef{
              first_token, timing_edit::CreatedNodeRole::kOutput},
          .candidate_pin = timing_edit::CreatedNodeRef{
              first_token, timing_edit::CreatedNodeRole::kInput},
          .buffer = bufferCell(),
          .numeric = numericReceipt(),
          .decision = CandidateDecision::kNonCriticalNet,
          .rejection_reason = std::nullopt,
          .accepted_creation_token = std::nullopt});
  require(validateBufferTraceArtifact(artifact).empty(),
          "round two evidence could not reference round one materializations");

  const std::string wire = bufferTraceArtifactToJson(artifact);
  auto parsed = parseBufferTraceArtifactJson(wire);
  const auto* round_trip = std::get_if<BufferTraceArtifact>(&parsed);
  require(round_trip != nullptr && *round_trip == artifact,
          "created-reference artifact JSON did not round-trip exactly");
  require(bufferTraceArtifactToJson(*round_trip) == wire,
          "artifact JSON writer was not deterministic after round-trip");

  require(std::holds_alternative<BufferTraceContractViolations>(
              parseBufferTraceArtifactJson("// forbidden\n" + wire)),
          "JSON line comment was accepted");
  require(std::holds_alternative<BufferTraceContractViolations>(
              parseBufferTraceArtifactJson("/* forbidden */" + wire)),
          "JSON block comment was accepted");
}

void testSyntheticLogicalNamesAreLeafFiles()
{
  for (const std::string invalid : {"/", ".", ".."}) {
    auto program = syntheticProgram();
    std::get<SyntheticReplayPayload>(program.workload)
        .source_recipe.tech_lef.logical_name = invalid;
    require(hasCode(validateReplayProgram(program),
                    BufferTraceContractCode::kInvalidSourceArtifacts),
            "synthetic non-leaf logical name was accepted: " + invalid);
  }
}

void requireArtifactCode(const BufferTraceArtifact& artifact,
                         BufferTraceContractCode code,
                         const std::string& message)
{
  require(hasCode(validateBufferTraceArtifact(artifact), code), message);
}

void requireArtifactCodeWithoutCanonicalization(
    const BufferTraceArtifact& artifact, BufferTraceContractCode code,
    const std::string& message)
{
  BufferTraceContractViolations errors;
  try {
    errors = validateBufferTraceArtifact(artifact);
  } catch (const std::exception&) {
    require(false, message + ": validator leaked an exception");
  }
  require(hasCode(errors, code), message);
}

void testWorkloadFirstComparability()
{
  auto program = realProgram();
  program.header.profile_id = ProfileId::kPaperProtocolTop100X10;
  auto& edit = std::get<RealReplayPayload>(program.workload).batches[0].edits[0];
  edit.creation_token.profile_id = "paper_protocol_top100_x10";
  requireNoErrors(validateReplayProgram(program),
                  "real paper-profile trace rejected required non-comparability");

  program.header.comparability_status = ComparabilityStatus::kProtocolOnly;
  program.header.comparability_reason.reset();
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kInvalidProfile),
          "real paper-profile trace claimed protocol-only comparability");

  auto synthetic = syntheticProgram();
  synthetic.header.profile_id = ProfileId::kPaperProtocolTop100X10;
  synthetic.header.comparability_status = ComparabilityStatus::kProtocolOnly;
  synthetic.header.comparability_reason.reset();
  auto& batches = std::get<SyntheticReplayPayload>(synthetic.workload).batches;
  for (auto& batch : batches)
    for (auto& mutation : batch.edits)
      mutation.creation_token.profile_id = "paper_protocol_top100_x10";
  auto* created_net = std::get_if<timing_edit::CreatedNetRef>(&batches[1].edits[0].original_net);
  require(created_net != nullptr, "synthetic batch 2 lost its created-net reference");
  created_net->creation_token = batches[0].edits[0].creation_token;
  require(created_net->creation_token == batches[0].edits[0].creation_token,
          "created-net use-site did not retain the complete defining token");
  require(created_net->role == timing_edit::CreatedNetRole::kOutputNet, "created-net use-site changed its output-net role");
  require(batches[0].edits[0].creation_token.batch_ordinal == 1 &&
              batches[1].edits[0].creation_token.batch_ordinal == 2 &&
              batches[0].edits[0].creation_token.accepted_ordinal == 0 &&
              batches[1].edits[0].creation_token.accepted_ordinal == 0 &&
              batches[0].edits[0].creation_token.profile_id ==
                  "paper_protocol_top100_x10" &&
              batches[1].edits[0].creation_token.profile_id ==
                  "paper_protocol_top100_x10",
          "synthetic profile migration changed complete token fields");
  requireNoErrors(validateReplayProgram(synthetic),
                  "synthetic paper-profile protocol trace was rejected");

  auto stale_reference = synthetic;
  auto& stale_batches = std::get<SyntheticReplayPayload>(stale_reference.workload).batches;
  auto& stale_created =
      std::get<timing_edit::CreatedNetRef>(stale_batches[1].edits[0].original_net);
  stale_created.creation_token.profile_id = "engineering_smoke";
  require(hasCode(validateReplayProgram(stale_reference),
                  BufferTraceContractCode::kForwardReference),
          "created-net validation ignored the full defining token identity");
}

void appendDuplicateEdits(timing_edit::EditBatch& edit_plan,
                          std::size_t target_count)
{
  const auto prototype = edit_plan.edits.front();
  while (edit_plan.edits.size() < target_count) {
    auto mutation = prototype;
    mutation.creation_token.accepted_ordinal = edit_plan.edits.size();
    mutation.original_net = timing_edit::ExistingNetRef{
        "fanout" + std::to_string(edit_plan.edits.size())};
    mutation.moved_sinks = {timing_edit::ExistingNodeRef{
        pin("u" + std::to_string(edit_plan.edits.size()), "A")}};
    edit_plan.edits.push_back(std::move(mutation));
  }
}

void testReplayProfileLimitsAndTemplateBoundary()
{
  auto program = syntheticProgram();
  auto& batches = std::get<SyntheticReplayPayload>(program.workload).batches;
  const auto second_token = batches.back().edits.front().creation_token;
  auto third_token = tokenFor(3);
  batches.push_back(plan(
      2,
      timing_edit::CreatedNetRef{
          second_token, timing_edit::CreatedNetRole::kOutputNet},
      third_token));
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kGenerationMismatch),
          "replay exceeded the frozen round limit");

  program = syntheticProgram();
  auto& oversized_batch
      = std::get<SyntheticReplayPayload>(program.workload).batches.front();
  appendDuplicateEdits(oversized_batch, 5);
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kGenerationMismatch),
          "replay batch exceeded the frozen top-k limit");

  program = syntheticProgram();
  auto& total_batches
      = std::get<SyntheticReplayPayload>(program.workload).batches;
  appendDuplicateEdits(total_batches[0], 3);
  appendDuplicateEdits(total_batches[1], 3);
  require(hasCode(validateReplayProgram(program),
                  BufferTraceContractCode::kGenerationMismatch),
          "replay exceeded the frozen total insertion limit");

  program = syntheticProgram();
  require(hasCode(validateReplayTemplate(program),
                  BufferTraceContractCode::kInvalidValue),
          "generation template accepted pre-populated replay edits");
  auto& payload = std::get<SyntheticReplayPayload>(program.workload);
  payload.batches.clear();
  require(hasCode(validateReplayTemplate(program), BufferTraceContractCode::kIdentityMismatch),
          "synthetic template accepted a baseline that its wire format cannot represent");
  payload.baseline = {};
  requireNoErrors(validateReplayTemplate(program),
                  "empty generation template was rejected");
  const auto parsed = parseReplayTemplateJson(replayTemplateToJson(program));
  require(std::holds_alternative<ReplayProgram>(parsed) && std::get<ReplayProgram>(parsed) == program,
          "empty synthetic template changed during a wire round trip");
}

void testGenerationNumericDecisionMatrix()
{
  auto artifact = syntheticArtifact();
  auto& evaluation = artifact.generation_only.rounds[0].evaluations[0];
  evaluation.numeric.reset();
  requireArtifactCode(std::move(artifact), BufferTraceContractCode::kInvalidValue,
                      "accepted evaluation without numeric evidence was accepted");

  artifact = syntheticArtifact();
  auto& lower = artifact.generation_only.rounds[0].evaluations[0];
  lower.decision = CandidateDecision::kLowerRankedCandidate;
  lower.accepted_creation_token.reset();
  lower.numeric->gain_ns = 0.0;
  requireArtifactCode(std::move(artifact), BufferTraceContractCode::kInvalidValue,
                      "lower-ranked nonpositive evaluation was accepted");

  artifact = syntheticArtifact();
  auto& noncritical = artifact.generation_only.rounds[0].evaluations[0];
  noncritical.decision = CandidateDecision::kNonCriticalNet;
  noncritical.accepted_creation_token.reset();
  noncritical.numeric.reset();
  requireArtifactCode(std::move(artifact), BufferTraceContractCode::kInvalidValue,
                      "noncritical evaluation without numeric evidence was accepted");

  artifact = syntheticArtifact();
  auto& rejected = artifact.generation_only.rounds[0].evaluations[0];
  rejected.decision = CandidateDecision::kRejected;
  rejected.accepted_creation_token.reset();
  rejected.rejection_reason = CandidateRejectionReason::kNonPositiveGain;
  requireArtifactCode(std::move(artifact), BufferTraceContractCode::kInvalidValue,
                      "nonpositive rejection retained positive numeric evidence");

  artifact = syntheticArtifact();
  artifact.generation_only.rounds[0].evaluations[0]
      .numeric->upstream_resistance_kohm = 63.0;
  requireArtifactCode(std::move(artifact), BufferTraceContractCode::kInvalidProfile,
                      "generation numeric resistance drifted from the profile");

  for (int operand = 0; operand != 3; ++operand) {
    artifact = syntheticArtifact();
    auto& numeric = *artifact.generation_only.rounds[0].evaluations[0].numeric;
    if (operand == 0)
      numeric.upstream_resistance_kohm = -1.0;
    else if (operand == 1)
      numeric.candidate_load_pf = -1.0;
    else
      numeric.buffer_delay_ns = -1.0;
    requireArtifactCode(std::move(artifact), BufferTraceContractCode::kInvalidValue,
                        "negative R/C/delay operand was accepted");
  }

  artifact = syntheticArtifact();
  artifact.generation_only.rounds[0].evaluations[0].decision
      = static_cast<CandidateDecision>(255);
  requireArtifactCodeWithoutCanonicalization(
      artifact, BufferTraceContractCode::kInvalidValue,
      "unknown candidate decision enum was accepted");

  artifact = syntheticArtifact();
  artifact.generation_only.rounds[0].evaluations[0].rejection_reason
      = static_cast<CandidateRejectionReason>(255);
  requireArtifactCodeWithoutCanonicalization(
      artifact, BufferTraceContractCode::kInvalidValue,
      "unknown candidate rejection enum was accepted");

  artifact = syntheticArtifact();
  artifact.generation_only.stop_reason
      = static_cast<GenerationStopReason>(255);
  requireArtifactCodeWithoutCanonicalization(
      artifact, BufferTraceContractCode::kInvalidValue,
      "unknown generation stop enum was accepted");

  auto program = syntheticProgram();
  program.header.profile_id = static_cast<ProfileId>(255);
  BufferTraceContractViolations errors;
  try {
    errors = validateReplayProgram(program);
  } catch (const std::exception&) {
    require(false, "unknown profile enum escaped the validator as an exception");
  }
  require(hasCode(errors, BufferTraceContractCode::kInvalidProfile),
          "unknown profile enum was not returned as a typed violation");
}

}  // namespace

int main()
{
  testArtifactLists(realProgram());
  testValidTaggedUnionAndCreatedNetChain();
  testTaggedUnionAndFiveSourceRolesFailClosed();
  testRealComparabilityAndEnvironmentAreExact();
  testForwardReferenceAndGenerationOnlyIsolation();
  testCanonicalTokenAndRoundPoisoning();
  testAcceptedEvaluationPermutationMapsByTokenOrdinal();
  testStrictJsonAndCreatedReferenceRoundTrip();
  testDirectValueCodecBoundaries(syntheticArtifact());
  testSyntheticLogicalNamesAreLeafFiles();
  testWorkloadFirstComparability();
  testReplayProfileLimitsAndTemplateBoundary();
  testGenerationNumericDecisionMatrix();
  return 0;
}
