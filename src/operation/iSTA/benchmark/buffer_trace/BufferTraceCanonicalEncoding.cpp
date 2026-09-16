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
#include "BufferTraceCanonicalEncoding.hpp"
#include "BufferTraceValidationRules.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include "CsrReferenceCanonicalEncoder.hpp"
#include "TimingEditByteOrder.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditValidation.hpp"
namespace ista::buffer_trace {
namespace {
using csr::CanonicalEncoder;
using Bytes = std::vector<std::uint8_t>;
template <typename Build>
Bytes encode(std::string_view domain, Build build)
{
  CanonicalEncoder encoder;
  encoder.appendDomain(domain);
  build(encoder);
  return encoder.bytes();
}
bool validText(const std::string& value) { return !value.empty() && value.find('\0') == std::string::npos; }
bool validLeafName(const std::string& value) { return validText(value) && value != "." && value != ".." && value.find('/') == std::string::npos; }
void appendProfile(CanonicalEncoder& encoder, const SelectionProfile& value)
{
  encoder.appendString(token(value.id));
  encoder.appendU64(value.top_k);
  encoder.appendU64(value.rounds);
  encoder.appendU64(value.max_total_insertions);
  encoder.appendString(token(value.analysis));
  encoder.appendDouble(value.operating_point.input_slew_ns);
  encoder.appendDouble(value.operating_point.output_load_pf);
  encoder.appendString(value.rc_profile.id);
  encoder.appendDouble(value.rc_profile.upstream_resistance_kohm);
  encoder.appendDouble(value.rc_profile.buffer_resistance_kohm);
}
void appendHeader(CanonicalEncoder& encoder, const ReplayHeader& value)
{
  encoder.appendU32(value.schema_version);
  encoder.appendString(token(value.selection_mode));
  encoder.appendString(token(value.workload_kind));
  encoder.appendString(token(value.profile_id));
  encoder.appendString(token(value.comparability_status));
  encoder.appendBool(value.comparability_reason.has_value());
  if (value.comparability_reason)
    encoder.appendString(token(*value.comparability_reason));
}
template <typename Recipe>
void appendRecipe(CanonicalEncoder& encoder, const Recipe& value)
{
  encoder.appendString(value.logical_name);
  encoder.appendString(value.source_text);
}
void appendSynthetic(CanonicalEncoder& encoder, const SyntheticArtifactRecipe& value)
{
  appendRecipe(encoder, value.tech_lef);
  appendRecipe(encoder, value.cell_lef);
  appendRecipe(encoder, value.liberty);
  appendRecipe(encoder, value.verilog);
  appendRecipe(encoder, value.sdc);
  encoder.appendString(value.top);
}
void appendSources(CanonicalEncoder& encoder, const SourceArtifactRefs& value)
{
  encoder.appendString(value.tech_lef.path);
  encoder.appendU64(value.cell_lefs.size());
  for (const auto& source : value.cell_lefs)
    encoder.appendString(source.path);
  encoder.appendU64(value.liberties.size());
  for (const auto& source : value.liberties)
    encoder.appendString(source.path);
  encoder.appendString(value.verilog.path);
  encoder.appendString(value.sdc.path);
  encoder.appendString(value.top);
  encoder.appendString(value.analysis_context.liberty_corner);
  encoder.appendString(value.analysis_context.temperature);
  encoder.appendString(value.analysis_context.rc_corner);
}
void appendPlans(CanonicalEncoder& encoder, const std::vector<timing_edit::DesignEditPlan>& plans)
{
  encoder.appendU64(plans.size());
  for (const auto& plan : plans)
    encoder.appendBytes(timing_edit::canonicalDesignEditPlanBytes(plan));
}
void appendPlacement(CanonicalEncoder& encoder, const timing_edit::PlacementInputs& value)
{
  std::visit(
      [&](const auto& placement) {
        using Placement = std::decay_t<decltype(placement)>;
        if constexpr (std::is_same_v<Placement, timing_edit::UnplacedInputs>) {
          encoder.appendString("unplaced");
        } else {
          encoder.appendString("placed");
          encoder.appendDouble(placement.x_microns);
          encoder.appendDouble(placement.y_microns);
        }
      },
      value);
}
void appendRc(CanonicalEncoder& encoder, const timing_edit::RcInputs& value)
{
  std::visit(
      [&](const auto& rc) {
        using Rc = std::decay_t<decltype(rc)>;
        if constexpr (std::is_same_v<Rc, timing_edit::NoParasiticRcInputs>) {
          encoder.appendString("no_parasitic");
        } else {
          encoder.appendString("parasitic");
          encoder.appendString(timing_edit::token(rc.parasitic_policy));
        }
      },
      value);
}
void appendCandidateIdentity(CanonicalEncoder& encoder, const BufferCandidateObservation& value)
{
  encoder.appendBytes(timing_edit::canonicalStableNetRefBytes(value.net));
  encoder.appendBytes(timing_edit::canonicalStableNodeRefBytes(value.driver));
  encoder.appendBytes(timing_edit::canonicalStableNodeRefBytes(value.candidate_pin));
  std::vector<Bytes> sinks;
  sinks.reserve(value.moved_sinks.size());
  for (const auto& sink : value.moved_sinks)
    sinks.push_back(timing_edit::canonicalStableNodeRefBytes(sink));
  std::sort(sinks.begin(), sinks.end(), timing_edit::detail::CanonicalBytesLess{});
  encoder.appendU64(sinks.size());
  for (const auto& sink : sinks)
    encoder.appendBytes(sink);
  encoder.appendBytes(timing_edit::canonicalLibraryCellRefBytes(value.buffer));
  appendPlacement(encoder, value.placement);
  appendRc(encoder, value.rc);
}
void appendNumeric(CanonicalEncoder& encoder, const GainComputationReceipt& value)
{
  encoder.appendDouble(value.arrival_ns);
  encoder.appendDouble(value.required_ns);
  encoder.appendDouble(value.slack_ns);
  encoder.appendDouble(value.upstream_resistance_kohm);
  encoder.appendDouble(value.candidate_load_pf);
  encoder.appendDouble(value.buffer_capacitance_pf);
  encoder.appendDouble(value.delta_capacitance_pf);
  encoder.appendDouble(value.benefit_ns);
  encoder.appendDouble(value.buffer_resistance_kohm);
  encoder.appendDouble(value.penalty_ns);
  encoder.appendDouble(value.benefit_minus_penalty_ns);
  encoder.appendDouble(value.buffer_delay_ns);
  encoder.appendDouble(value.gain_ns);
}
void reject(BufferTraceContractViolations& errors, BufferTraceContractCode code, std::string field, std::string message)
{
  errors.push_back(BufferTraceContractViolation{code, std::move(field), std::move(message)});
}
template <typename Enum>
bool knownEnum(Enum value, auto token_function)
{
  try {
    static_cast<void>(token_function(value));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}
void validatePlan(const timing_edit::DesignEditPlan& plan, std::string field, BufferTraceContractViolations& errors)
{
  for (const auto& violation : timing_edit::validateDesignEditPlan(plan))
    reject(errors, BufferTraceContractCode::kInvalidValue, field + "." + violation.field, violation.message);
  auto normalized = timing_edit::normalizeDesignEditPlan(plan);
  if (const auto* value = std::get_if<timing_edit::DesignEditPlan>(&normalized); value && *value != plan)
    reject(errors, BufferTraceContractCode::kNotCanonical, std::move(field), "design edit plan must be Stage 3 canonical");
}
template <typename Ref>
std::optional<timing_edit::CreationToken> referencedToken(const Ref& ref)
{
  return std::visit(
      [](const auto& value) -> std::optional<timing_edit::CreationToken> {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, timing_edit::CreatedNodeRef> || std::is_same_v<Value, timing_edit::CreatedNetRef>)
          return value.creation_token;
        return std::nullopt;
      },
      ref);
}
void validateReplayBatches(const ReplayProgram& program, const std::vector<timing_edit::DesignEditPlan>& batches,
                           const timing_edit::BaselineSemanticIdentity* explicit_baseline, BufferTraceContractViolations& errors)
{
  if (batches.empty()) {
    reject(errors, BufferTraceContractCode::kInvalidValue, "replay.workload.batches", "at least one batch is required");
    return;
  }
  const auto& baseline = explicit_baseline ? *explicit_baseline : batches.front().baseline;
  std::set<timing_edit::CreationToken> known_tokens;
  std::optional<std::string> case_id;
  std::optional<std::uint64_t> seed;
  for (std::size_t batch_index = 0; batch_index < batches.size(); ++batch_index) {
    const auto& plan = batches[batch_index];
    const std::string field = "replay.workload.batches[" + std::to_string(batch_index) + "]";
    validatePlan(plan, field, errors);
    if (plan.expected_generation.value() != batch_index)
      reject(errors, BufferTraceContractCode::kGenerationMismatch, field + ".expected_generation",
             "cumulative replay batches must start at generation zero and be contiguous");
    if (plan.baseline != baseline)
      reject(errors, BufferTraceContractCode::kIdentityMismatch, field + ".baseline", "all replay batches must use one exact baseline identity");
    std::vector<timing_edit::CreationToken> current_tokens;
    for (std::size_t edit_index = 0; edit_index < plan.edits.size(); ++edit_index) {
      const auto& edit = plan.edits[edit_index];
      const std::string edit_field = field + ".edits[" + std::to_string(edit_index) + "]";
      if (edit.creation_token.batch_ordinal != batch_index + 1)
        reject(errors, BufferTraceContractCode::kGenerationMismatch, edit_field + ".creation_token.batch_ordinal", "token batch ordinal must equal its one-based replay batch");
      if (edit.creation_token.accepted_ordinal != edit_index)
        reject(errors, BufferTraceContractCode::kNotCanonical, edit_field + ".creation_token.accepted_ordinal", "token accepted ordinal must equal canonical edit order");
      const auto validate_reference = [&](const auto& ref, std::string ref_field) {
        if (const auto token_value = referencedToken(ref); token_value && !known_tokens.contains(*token_value))
          reject(errors, BufferTraceContractCode::kForwardReference, std::move(ref_field),
                 "created reference must resolve to a token from an earlier batch");
      };
      validate_reference(edit.original_net, edit_field + ".original_net");
      for (std::size_t sink_index = 0; sink_index < edit.moved_sinks.size(); ++sink_index)
        validate_reference(edit.moved_sinks[sink_index], edit_field + ".moved_sinks[" + std::to_string(sink_index) + "]");
      if (known_tokens.contains(edit.creation_token)
          || std::find(current_tokens.begin(), current_tokens.end(), edit.creation_token) != current_tokens.end())
        reject(errors, BufferTraceContractCode::kDuplicate, edit_field + ".creation_token", "creation token must be globally unique");
      if (edit.creation_token.profile_id != token(program.header.profile_id))
        reject(errors, BufferTraceContractCode::kIdentityMismatch, edit_field + ".creation_token.profile_id",
               "creation token profile must match replay header");
      if (!case_id)
        case_id = edit.creation_token.case_id;
      if (!seed)
        seed = edit.creation_token.seed;
      if (*case_id != edit.creation_token.case_id || *seed != edit.creation_token.seed)
        reject(errors, BufferTraceContractCode::kIdentityMismatch, edit_field + ".creation_token",
               "case id and seed must be stable across the replay program");
      if (program.header.workload_kind == SemanticWorkloadKind::kRealDesign && edit.creation_token.seed != 0)
        reject(errors, BufferTraceContractCode::kInvalidValue, edit_field + ".creation_token.seed", "real-design v1 seed must be zero");
      if (program.header.workload_kind == SemanticWorkloadKind::kRealDesign
          && (!std::holds_alternative<timing_edit::UnplacedInputs>(edit.placement_inputs)
              || !std::holds_alternative<timing_edit::NoParasiticRcInputs>(edit.rc_inputs)))
        reject(errors, BufferTraceContractCode::kInvalidValue, edit_field,
               "real-design v1 requires unplaced and no-parasitic edit inputs");
      current_tokens.push_back(edit.creation_token);
    }
    known_tokens.insert(current_tokens.begin(), current_tokens.end());
  }
}
void validateEvaluationEvidence(const CandidateEvaluationReceipt& evaluation, const timing_edit::BaselineSemanticIdentity& baseline, ProfileId profile,
                                const std::string& field, BufferTraceContractViolations& errors)
{
  const auto append = [&](const timing_edit::TimingEditContractViolations& violations) {
    for (const auto& violation : violations) reject(errors, BufferTraceContractCode::kInvalidValue, field + "." + violation.field, violation.message);
  };
  append(timing_edit::validateStableNetRef(evaluation.net, "net"));
  append(timing_edit::validateStableNodeRef(evaluation.driver, "driver"));
  append(timing_edit::validateStableNodeRef(evaluation.candidate_pin, "candidate_pin"));
  timing_edit::CreationToken probe_token{.case_id = "__buffer_receipt_validation__", .profile_id = std::string(token(profile)), .batch_ordinal = 1};
  timing_edit::DesignEditPlan probe{
      .baseline = baseline, .expected_generation = timing_edit::TransactionGeneration{0},
      .edits = {{evaluation.net, {evaluation.candidate_pin}, evaluation.buffer, timing_edit::UnplacedInputs{}, timing_edit::NoParasiticRcInputs{}, std::move(probe_token)}}};
  constexpr std::string_view kLibraryPrefix = "plan.edits[0].library_cell";
  for (const auto& violation : timing_edit::validateDesignEditPlan(probe)) {
    if (!violation.field.starts_with(kLibraryPrefix)) continue;
    reject(errors, BufferTraceContractCode::kInvalidValue, field + ".buffer" + violation.field.substr(kLibraryPrefix.size()), violation.message);
  }
}
bool finiteNumeric(const GainComputationReceipt& value)
{
  const double fields[] = {value.arrival_ns, value.required_ns, value.slack_ns, value.upstream_resistance_kohm,
                           value.candidate_load_pf, value.buffer_capacitance_pf, value.delta_capacitance_pf,
                           value.benefit_ns, value.buffer_resistance_kohm, value.penalty_ns,
                           value.benefit_minus_penalty_ns, value.buffer_delay_ns, value.gain_ns};
  return std::all_of(std::begin(fields), std::end(fields), [](double field) { return std::isfinite(field); });
}
}  // namespace
std::string_view token(SelectionMode value)
{
  if (value == SelectionMode::kFixedTrace) return "fixed_trace";
  throw std::invalid_argument("unknown SelectionMode");
}
std::string_view token(SemanticWorkloadKind value)
{
  switch (value) {
    case SemanticWorkloadKind::kSynthetic: return "synthetic";
    case SemanticWorkloadKind::kRealDesign: return "real_design";
  }
  throw std::invalid_argument("unknown SemanticWorkloadKind");
}
std::string_view token(ProfileId value)
{
  switch (value) {
    case ProfileId::kPaperProtocolTop1000X1: return "paper_protocol_top1000_x1";
    case ProfileId::kPaperProtocolTop100X10: return "paper_protocol_top100_x10";
    case ProfileId::kEngineeringSmoke: return "engineering_smoke";
  }
  throw std::invalid_argument("unknown ProfileId");
}
std::string_view token(ComparabilityStatus value)
{
  switch (value) {
    case ComparabilityStatus::kExact: return "exact";
    case ComparabilityStatus::kProtocolOnly: return "protocol_only";
    case ComparabilityStatus::kNonComparable: return "non_comparable";
  }
  throw std::invalid_argument("unknown ComparabilityStatus");
}
std::string_view token(ComparabilityReason value)
{
  if (value == ComparabilityReason::kEngineeringRcNoPhysical) return "engineering_rc_no_physical";
  throw std::invalid_argument("unknown ComparabilityReason");
}
std::string_view token(AnalysisMode value)
{
  if (value == AnalysisMode::kCpuFull) return "cpu_full";
  throw std::invalid_argument("unknown AnalysisMode");
}
std::string_view token(CandidateDecision value)
{
  switch (value) {
    case CandidateDecision::kAccepted: return "accepted";
    case CandidateDecision::kNonCriticalNet: return "non_critical_net";
    case CandidateDecision::kLowerRankedCandidate: return "lower_ranked_candidate";
    case CandidateDecision::kRejected: return "rejected";
  }
  throw std::invalid_argument("unknown CandidateDecision");
}
std::string_view token(CandidateRejectionReason value)
{
  switch (value) {
    case CandidateRejectionReason::kInvalidSemanticReference: return "invalid_semantic_reference";
    case CandidateRejectionReason::kInvalidMovedSinkSet: return "invalid_moved_sink_set";
    case CandidateRejectionReason::kInvalidBufferCell: return "invalid_buffer_cell";
    case CandidateRejectionReason::kInvalidEnvironmentInputs: return "invalid_environment_inputs";
    case CandidateRejectionReason::kTimingNotMaterialized: return "timing_not_materialized";
    case CandidateRejectionReason::kNonFiniteTiming: return "non_finite_timing";
    case CandidateRejectionReason::kSlackMismatch: return "slack_mismatch";
    case CandidateRejectionReason::kNegativeOperand: return "negative_operand";
    case CandidateRejectionReason::kNonFiniteIntermediate: return "non_finite_intermediate";
    case CandidateRejectionReason::kNonPositiveGain: return "non_positive_gain";
  }
  throw std::invalid_argument("unknown CandidateRejectionReason");
}
std::string_view token(GenerationStopReason value)
{
  switch (value) {
    case GenerationStopReason::kEmptyAcceptedBatch: return "empty_accepted_batch";
    case GenerationStopReason::kRoundsReached: return "rounds_reached";
    case GenerationStopReason::kMaxTotalInsertionsReached: return "max_total_insertions_reached";
  }
  throw std::invalid_argument("unknown GenerationStopReason");
}
std::string_view token(BufferTraceContractCode value)
{
  switch (value) {
    case BufferTraceContractCode::kSchemaMismatch: return "schema_mismatch";
    case BufferTraceContractCode::kInvalidValue: return "invalid_value";
    case BufferTraceContractCode::kInvalidTaggedUnion: return "invalid_tagged_union";
    case BufferTraceContractCode::kInvalidProfile: return "invalid_profile";
    case BufferTraceContractCode::kInvalidSourceArtifacts: return "invalid_source_artifacts";
    case BufferTraceContractCode::kGenerationMismatch: return "generation_mismatch";
    case BufferTraceContractCode::kIdentityMismatch: return "identity_mismatch";
    case BufferTraceContractCode::kForwardReference: return "forward_reference";
    case BufferTraceContractCode::kDuplicate: return "duplicate";
    case BufferTraceContractCode::kNotCanonical: return "not_canonical";
  }
  throw std::invalid_argument("unknown BufferTraceContractCode");
}
SelectionProfile frozenSelectionProfile(ProfileId id)
{
  constexpr OperatingPoint operating_point{0.05, 0.01};
  const EngineeringRcProfile rc_profile{"engineering_rc_v1", 64.0, 1.0};
  switch (id) {
    case ProfileId::kPaperProtocolTop1000X1: return {id, 1000, 1, 1000, AnalysisMode::kCpuFull, operating_point, rc_profile};
    case ProfileId::kPaperProtocolTop100X10: return {id, 100, 10, 1000, AnalysisMode::kCpuFull, operating_point, rc_profile};
    case ProfileId::kEngineeringSmoke: return {id, 4, 2, 4, AnalysisMode::kCpuFull, operating_point, rc_profile};
  }
  throw std::invalid_argument("unknown ProfileId");
}
Bytes canonicalSelectionProfileBytes(const SelectionProfile& profile)
{
  return encode("buffer-selection-profile", [&](CanonicalEncoder& encoder) { appendProfile(encoder, profile); });
}
Bytes canonicalBufferCandidateIdentityBytes(const BufferCandidateObservation& candidate)
{
  return encode("buffer-candidate-identity", [&](CanonicalEncoder& encoder) { appendCandidateIdentity(encoder, candidate); });
}
Bytes canonicalReplayHeaderBytes(const ReplayHeader& header)
{
  return encode("buffer-replay-header", [&](CanonicalEncoder& encoder) { appendHeader(encoder, header); });
}
Bytes canonicalSyntheticArtifactRecipeBytes(const SyntheticArtifactRecipe& recipe)
{
  return encode("buffer-synthetic-artifact-recipe", [&](CanonicalEncoder& encoder) { appendSynthetic(encoder, recipe); });
}
Bytes canonicalSourceArtifactRefsBytes(const SourceArtifactRefs& artifacts)
{
  return encode("buffer-source-artifact-refs", [&](CanonicalEncoder& encoder) { appendSources(encoder, artifacts); });
}
Bytes canonicalReplayProgramBytes(const ReplayProgram& program)
{
  return encode("buffer-fixed-replay-program", [&](CanonicalEncoder& encoder) {
    encoder.appendBytes(canonicalReplayHeaderBytes(program.header));
    std::visit(
        [&](const auto& workload) {
          using Workload = std::decay_t<decltype(workload)>;
          if constexpr (std::is_same_v<Workload, SyntheticReplayPayload>) {
            encoder.appendString("synthetic");
            encoder.appendBytes(canonicalSyntheticArtifactRecipeBytes(workload.source_recipe));
            appendPlans(encoder, workload.batches);
          } else {
            encoder.appendString("real_design");
            encoder.appendBytes(canonicalSourceArtifactRefsBytes(workload.artifacts));
            encoder.appendBytes(timing_edit::canonicalBaselineSemanticIdentityBytes(workload.baseline));
            appendPlans(encoder, workload.batches);
          }
        },
        program.workload);
  });
}
Bytes canonicalGenerationReceiptBytes(const GenerationReceipt& receipt)
{
  return encode("buffer-generation-receipt", [&](CanonicalEncoder& encoder) {
    encoder.appendBytes(canonicalSelectionProfileBytes(receipt.profile));
    encoder.appendU64(receipt.rounds.size());
    for (const auto& round : receipt.rounds) {
      encoder.appendU64(round.batch_ordinal);
      encoder.appendU64(round.evaluations.size());
      for (const auto& evaluation : round.evaluations) {
        encoder.appendBytes(timing_edit::canonicalStableNetRefBytes(evaluation.net));
        encoder.appendBytes(timing_edit::canonicalStableNodeRefBytes(evaluation.driver));
        encoder.appendBytes(timing_edit::canonicalStableNodeRefBytes(evaluation.candidate_pin));
        encoder.appendBytes(timing_edit::canonicalLibraryCellRefBytes(evaluation.buffer));
        encoder.appendBool(evaluation.numeric.has_value());
        if (evaluation.numeric)
          appendNumeric(encoder, *evaluation.numeric);
        encoder.appendString(token(evaluation.decision));
        encoder.appendBool(evaluation.rejection_reason.has_value());
        if (evaluation.rejection_reason)
          encoder.appendString(token(*evaluation.rejection_reason));
        encoder.appendBool(evaluation.accepted_creation_token.has_value());
        if (evaluation.accepted_creation_token)
          encoder.appendBytes(timing_edit::canonicalCreationTokenBytes(*evaluation.accepted_creation_token));
      }
      encoder.appendU64(round.accepted_tokens.size());
      for (const auto& creation_token : round.accepted_tokens)
        encoder.appendBytes(timing_edit::canonicalCreationTokenBytes(creation_token));
    }
    encoder.appendString(token(receipt.stop_reason));
  });
}
BufferTraceContractViolations validateSelectionProfile(const SelectionProfile& profile)
{
  BufferTraceContractViolations errors;
  if (!knownEnum(profile.id, static_cast<std::string_view (*)(ProfileId)>(token))
      || !knownEnum(profile.analysis, static_cast<std::string_view (*)(AnalysisMode)>(token))) {
    reject(errors, BufferTraceContractCode::kInvalidProfile, "profile", "unknown profile id or analysis mode");
    return errors;
  }
  const double numeric[] = {profile.operating_point.input_slew_ns, profile.operating_point.output_load_pf,
                            profile.rc_profile.upstream_resistance_kohm, profile.rc_profile.buffer_resistance_kohm};
  if (profile.top_k == 0 || profile.rounds == 0 || profile.max_total_insertions == 0 || !validText(profile.rc_profile.id)
      || !std::all_of(std::begin(numeric), std::end(numeric), [](double value) { return std::isfinite(value) && value >= 0.0; }))
    reject(errors, BufferTraceContractCode::kInvalidProfile, "profile", "profile counts, text and numeric operands must be valid");
  if (profile != frozenSelectionProfile(profile.id))
    reject(errors, BufferTraceContractCode::kInvalidProfile, "profile", "profile must exactly match its frozen version");
  return errors;
}
BufferTraceContractViolations validateReplayProgram(const ReplayProgram& program)
{
  BufferTraceContractViolations errors;
  if (program.header.schema_version != kBufferFixedTraceSchemaVersion)
    reject(errors, BufferTraceContractCode::kSchemaMismatch, "replay.header.schema_version", "expected buffer fixed trace schema 1");
  if (!knownEnum(program.header.profile_id, static_cast<std::string_view (*)(ProfileId)>(token))) {
    reject(errors, BufferTraceContractCode::kInvalidProfile, "replay.header.profile_id", "header contains an unknown profile id");
    return errors;
  }
  if (!knownEnum(program.header.selection_mode, static_cast<std::string_view (*)(SelectionMode)>(token))
      || !knownEnum(program.header.workload_kind, static_cast<std::string_view (*)(SemanticWorkloadKind)>(token))
      || !knownEnum(program.header.comparability_status, static_cast<std::string_view (*)(ComparabilityStatus)>(token))
      || (program.header.comparability_reason
          && !knownEnum(*program.header.comparability_reason, static_cast<std::string_view (*)(ComparabilityReason)>(token)))) {
    reject(errors, BufferTraceContractCode::kInvalidValue, "replay.header", "header contains an unknown enum value");
    return errors;
  }
  detail::appendReplayProfileViolations(program, errors);
  const bool synthetic_tag = program.header.workload_kind == SemanticWorkloadKind::kSynthetic;
  const bool synthetic_value = std::holds_alternative<SyntheticReplayPayload>(program.workload);
  if (synthetic_tag != synthetic_value) {
    reject(errors, BufferTraceContractCode::kInvalidTaggedUnion, "replay.workload", "header workload kind must match the payload alternative");
    return errors;
  }
  if (synthetic_value) {
    const auto& payload = std::get<SyntheticReplayPayload>(program.workload);
    const auto& recipe = payload.source_recipe;
    const std::array<std::string, 5> names{recipe.tech_lef.logical_name, recipe.cell_lef.logical_name, recipe.liberty.logical_name,
                                           recipe.verilog.logical_name, recipe.sdc.logical_name};
    const std::array<std::string, 5> texts{recipe.tech_lef.source_text, recipe.cell_lef.source_text, recipe.liberty.source_text,
                                           recipe.verilog.source_text, recipe.sdc.source_text};
    std::set<std::string> unique_names;
    if (!validText(recipe.top) || !std::all_of(names.begin(), names.end(), validLeafName) || !std::all_of(texts.begin(), texts.end(), validText)
        || !std::all_of(names.begin(), names.end(), [&](const std::string& name) { return unique_names.insert(name).second; })
        || recipe.tech_lef.source_text == recipe.cell_lef.source_text)
      reject(errors, BufferTraceContractCode::kInvalidSourceArtifacts, "replay.workload.source_recipe",
             "five named source recipes must be nonempty and distinct, including separate tech/cell LEF content");
    validateReplayBatches(program, payload.batches, nullptr, errors);
  } else {
    const auto& payload = std::get<RealReplayPayload>(program.workload);
    const auto& refs = payload.artifacts;
    std::vector<std::string> paths{refs.tech_lef.path, refs.verilog.path, refs.sdc.path};
    for (const auto& source : refs.cell_lefs)
      paths.push_back(source.path);
    for (const auto& source : refs.liberties)
      paths.push_back(source.path);
    std::set<std::string> unique_paths;
    if (!validText(refs.top) || refs.cell_lefs.empty() || refs.liberties.empty()
        || !validText(refs.analysis_context.liberty_corner) || !validText(refs.analysis_context.temperature)
        || !validText(refs.analysis_context.rc_corner) || !std::all_of(paths.begin(), paths.end(), validText)
        || !std::all_of(paths.begin(), paths.end(), [&](const std::string& path) { return unique_paths.insert(path).second; }))
      reject(errors, BufferTraceContractCode::kInvalidSourceArtifacts, "replay.workload.artifacts",
             "real source lists/context must be nonempty and paths pairwise distinct");
    for (const auto& violation : timing_edit::validateBaselineSemanticIdentity(payload.baseline))
      reject(errors, BufferTraceContractCode::kIdentityMismatch, "replay.workload.baseline." + violation.field, violation.message);
    validateReplayBatches(program, payload.batches, &payload.baseline, errors);
  }
  return errors;
}
BufferTraceContractViolations validateReplayTemplate(const ReplayProgram& program)
{
  const auto* real = std::get_if<RealReplayPayload>(&program.workload);
  const bool default_real_baseline
      = real != nullptr && real->batches.empty() && real->baseline == timing_edit::BaselineSemanticIdentity{};
  BufferTraceContractViolations errors = validateReplayProgram(program);
  errors.erase(std::remove_if(errors.begin(), errors.end(), [default_real_baseline](const BufferTraceContractViolation& violation) {
                 const bool empty_batches = violation.code == BufferTraceContractCode::kInvalidValue
                                            && violation.field == "replay.workload.batches"
                                            && violation.message == "at least one batch is required";
                 return empty_batches || (default_real_baseline && violation.field.starts_with("replay.workload.baseline"));
               }),
               errors.end());
  const bool empty = std::visit([](const auto& payload) { return payload.batches.empty(); }, program.workload);
  if (!empty)
    reject(errors, BufferTraceContractCode::kInvalidValue, "replay.workload.batches",
           "generation template must not contain replay edits");
  return errors;
}
BufferTraceContractViolations validateBufferTraceArtifact(const BufferTraceArtifact& artifact)
{
  BufferTraceContractViolations errors = validateReplayProgram(artifact.replay);
  const bool replay_profile_known
      = knownEnum(artifact.replay.header.profile_id,
                  static_cast<std::string_view (*)(ProfileId)>(token));
  if (const auto profile_errors = validateSelectionProfile(artifact.generation_only.profile); !profile_errors.empty())
    errors.insert(errors.end(), profile_errors.begin(), profile_errors.end());
  if (artifact.generation_only.profile.id != artifact.replay.header.profile_id)
    reject(errors, BufferTraceContractCode::kIdentityMismatch, "generation_only.profile.id", "must match replay header profile id");
  if (!knownEnum(artifact.generation_only.stop_reason, static_cast<std::string_view (*)(GenerationStopReason)>(token))) reject(errors, BufferTraceContractCode::kInvalidValue, "generation_only.stop_reason", "unknown generation stop reason");
  if (artifact.generation_only.rounds.empty() || artifact.generation_only.rounds.size() > artifact.generation_only.profile.rounds)
    reject(errors, BufferTraceContractCode::kGenerationMismatch, "generation_only.rounds", "round count must be within the frozen profile");
  const auto& replay_batches = std::visit([](const auto& payload) -> const std::vector<timing_edit::DesignEditPlan>& {
    return payload.batches;
  }, artifact.replay.workload);
  const timing_edit::BaselineSemanticIdentity* receipt_baseline = std::visit([](const auto& payload) {
    using Payload = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<Payload, RealReplayPayload>)
      return &payload.baseline;
    else
      return payload.batches.empty() ? static_cast<const timing_edit::BaselineSemanticIdentity*>(nullptr)
                                     : &payload.batches.front().baseline;
  }, artifact.replay.workload);
  std::set<timing_edit::CreationToken> generation_tokens;
  std::vector<std::vector<timing_edit::CreationToken>> generated_batch_tokens;
  std::uint64_t accepted_count = 0;
  for (std::size_t round_index = 0; round_index < artifact.generation_only.rounds.size(); ++round_index) {
    const auto& round = artifact.generation_only.rounds[round_index];
    const std::string field = "generation_only.rounds[" + std::to_string(round_index) + "]";
    if (round.batch_ordinal != round_index + 1)
      reject(errors, BufferTraceContractCode::kGenerationMismatch, field + ".batch_ordinal", "round ordinals must be contiguous and one-based");
    if (round.accepted_tokens.size() > artifact.generation_only.profile.top_k)
      reject(errors, BufferTraceContractCode::kGenerationMismatch, field + ".accepted_tokens", "round exceeds the frozen profile top-k");
    std::set<timing_edit::CreationToken> round_tokens(round.accepted_tokens.begin(), round.accepted_tokens.end());
    if (round_tokens.size() != round.accepted_tokens.size())
      reject(errors, BufferTraceContractCode::kDuplicate, field + ".accepted_tokens", "accepted tokens must be unique");
    for (const auto& accepted_token : round.accepted_tokens)
      if (!generation_tokens.insert(accepted_token).second)
        reject(errors, BufferTraceContractCode::kDuplicate, field + ".accepted_tokens",
               "accepted token was already used by an earlier round");
    if (!round.accepted_tokens.empty())
      generated_batch_tokens.push_back(round.accepted_tokens);
    else if (round_index + 1 != artifact.generation_only.rounds.size())
      reject(errors, BufferTraceContractCode::kGenerationMismatch, field + ".accepted_tokens", "only the terminal empty-stop round may omit a replay batch");
    std::set<timing_edit::CreationToken> unique_evaluation_tokens;
    for (std::size_t evaluation_index = 0; evaluation_index < round.evaluations.size(); ++evaluation_index) {
      const auto& evaluation = round.evaluations[evaluation_index];
      const std::string evaluation_field = field + ".evaluations[" + std::to_string(evaluation_index) + "]";
      if (!knownEnum(evaluation.decision, static_cast<std::string_view (*)(CandidateDecision)>(token)) || (evaluation.rejection_reason && !knownEnum(*evaluation.rejection_reason, static_cast<std::string_view (*)(CandidateRejectionReason)>(token)))) reject(errors, BufferTraceContractCode::kInvalidValue, evaluation_field, "unknown candidate decision or rejection reason");
      if (receipt_baseline && replay_profile_known)
        validateEvaluationEvidence(evaluation, *receipt_baseline, artifact.replay.header.profile_id, evaluation_field, errors);
      detail::appendEvaluationContractViolations(
          evaluation, artifact.generation_only.profile, evaluation_field, errors);
      if (evaluation.numeric) {
        const auto& value = *evaluation.numeric;
        const double delta = value.candidate_load_pf - value.buffer_capacitance_pf;
        const double benefit = value.upstream_resistance_kohm * delta;
        const double penalty = value.buffer_resistance_kohm * value.buffer_capacitance_pf;
        const double net_benefit = benefit - penalty;
        const double gain = net_benefit - value.buffer_delay_ns;
        if (!finiteNumeric(value) || value.required_ns - value.arrival_ns != value.slack_ns || delta != value.delta_capacitance_pf
            || benefit != value.benefit_ns || penalty != value.penalty_ns || net_benefit != value.benefit_minus_penalty_ns
            || gain != value.gain_ns)
          reject(errors, BufferTraceContractCode::kInvalidValue, field + ".evaluations.numeric",
                 "numeric receipt must exactly reproduce the staged binary64 computation");
      }
      if (evaluation.accepted_creation_token) {
        const auto& accepted_token = *evaluation.accepted_creation_token;
        if (!unique_evaluation_tokens.insert(accepted_token).second)
          reject(errors, BufferTraceContractCode::kDuplicate, evaluation_field + ".accepted_creation_token",
                 "accepted evaluation token must be unique within its round");
        const std::uint64_t accepted_index = accepted_token.accepted_ordinal;
        if (round_index < replay_batches.size() && accepted_index < replay_batches[round_index].edits.size()) {
          const auto& edit = replay_batches[round_index].edits[accepted_index];
          if (accepted_token != edit.creation_token || evaluation.net != edit.original_net
              || edit.moved_sinks != std::vector<timing_edit::StableNodeRef>{evaluation.candidate_pin}
              || evaluation.buffer != edit.library_cell)
            reject(errors, BufferTraceContractCode::kIdentityMismatch, evaluation_field,
                   "accepted evaluation must equal the same replay edit token/net/single sink/library");
        } else
          reject(errors, BufferTraceContractCode::kGenerationMismatch, evaluation_field,
                 "accepted evaluation token has no same-round replay edit");
      }
    }
    if (round_tokens != unique_evaluation_tokens)
      reject(errors, BufferTraceContractCode::kIdentityMismatch, field + ".accepted_tokens",
             "must equal accepted evaluation tokens as a duplicate-free set");
    accepted_count += round.accepted_tokens.size();
  }
  if (accepted_count > artifact.generation_only.profile.max_total_insertions)
    reject(errors, BufferTraceContractCode::kGenerationMismatch, "generation_only.rounds", "accepted count exceeds max_total_insertions");
  std::set<timing_edit::CreationToken> replay_tokens;
  std::vector<std::vector<timing_edit::CreationToken>> replay_batch_tokens;
  std::visit([&](const auto& payload) {
    for (const auto& plan : payload.batches) {
      std::vector<timing_edit::CreationToken> batch_tokens;
      for (const auto& edit : plan.edits) {
        replay_tokens.insert(edit.creation_token);
        batch_tokens.push_back(edit.creation_token);
      }
      replay_batch_tokens.push_back(std::move(batch_tokens));
    }
  }, artifact.replay.workload);
  if (generation_tokens != replay_tokens)
    reject(errors, BufferTraceContractCode::kIdentityMismatch, "generation_only", "accepted generation tokens must equal replay edit tokens");
  if (generated_batch_tokens != replay_batch_tokens)
    reject(errors, BufferTraceContractCode::kGenerationMismatch, "generation_only.rounds", "each nonempty round must exactly match the same replay batch token order");
  if (!artifact.generation_only.rounds.empty()) {
    const bool last_empty = artifact.generation_only.rounds.back().accepted_tokens.empty();
    if ((artifact.generation_only.stop_reason == GenerationStopReason::kEmptyAcceptedBatch) != last_empty)
      reject(errors, BufferTraceContractCode::kInvalidValue, "generation_only.stop_reason", "only an empty terminal round uses the empty-batch stop reason");
  }
  if (artifact.generation_only.stop_reason == GenerationStopReason::kRoundsReached
      && artifact.generation_only.rounds.size() != artifact.generation_only.profile.rounds)
    reject(errors, BufferTraceContractCode::kInvalidValue, "generation_only.stop_reason", "rounds stop requires the configured round count");
  const bool reached_limit = accepted_count == artifact.generation_only.profile.max_total_insertions;
  if ((artifact.generation_only.stop_reason == GenerationStopReason::kMaxTotalInsertionsReached) != reached_limit)
    reject(errors, BufferTraceContractCode::kInvalidValue, "generation_only.stop_reason",
           "max-total stop reason must be used exactly when accepted count reaches the limit");
  return errors;
}
}  // namespace ista::buffer_trace
