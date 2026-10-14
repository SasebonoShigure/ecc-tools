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
#include "BufferTraceJson.hpp"
#include "BufferTraceJsonInternal.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include "TimingEditJsonInternal.hpp"
#include "StrictJson.hpp"
#include "TimingEditValidation.hpp"
#include "json.hpp"
namespace ista::buffer_trace {
namespace {
using Json = nlohmann::json;
using json_detail::WireError;
using json_detail::exactObject;
using json_detail::text;
using json_detail::vectorOf;
Json parseClosedWorldJson(std::string_view text)
{
  try {
    return ista::json_detail::parseUniqueKeyJson(text);
  } catch (const ista::json_detail::DuplicateJsonKey& error) {
    throw WireError(BufferTraceContractCode::kDuplicate, "json", error.what());
  }
}
std::uint64_t u64(const Json& value, std::string_view field)
{
  if (!value.is_number_unsigned())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "expected unsigned integer");
  return value.get<std::uint64_t>();
}
std::uint32_t u32(const Json& value, std::string_view field)
{
  const auto result = u64(value, field);
  if (result > std::numeric_limits<std::uint32_t>::max())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "unsigned integer exceeds uint32");
  return static_cast<std::uint32_t>(result);
}
double number(const Json& value, std::string_view field)
{
  if (!value.is_number())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "expected number");
  return value.get<double>();
}
template <typename Enum>
Enum enumValue(const Json& value, std::string_view field, std::initializer_list<std::pair<std::string_view, Enum>> choices)
{
  const std::string name = text(value, field);
  for (const auto& [candidate, result] : choices)
    if (name == candidate)
      return result;
  throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "unknown enum token");
}
timing_edit::CreationToken creationToken(const Json& value, std::string_view field)
{
  exactObject(value, field, {"schema_version", "case_id", "profile_id", "seed", "batch_ordinal", "accepted_ordinal"});
  timing_edit::CreationToken result{u32(value.at("schema_version"), std::string(field) + ".schema_version"),
                                    text(value.at("case_id"), std::string(field) + ".case_id"),
                                    text(value.at("profile_id"), std::string(field) + ".profile_id"),
                                    u64(value.at("seed"), std::string(field) + ".seed"),
                                    u64(value.at("batch_ordinal"), std::string(field) + ".batch_ordinal"),
                                    u64(value.at("accepted_ordinal"), std::string(field) + ".accepted_ordinal")};
  if (const auto errors = timing_edit::validateCreationToken(result); !errors.empty())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), errors.front().message);
  return result;
}
Json toJson(const timing_edit::CreationToken& value)
{
  return {{"schema_version", value.schema_version}, {"case_id", value.case_id}, {"profile_id", value.profile_id}, {"seed", value.seed},
          {"batch_ordinal", value.batch_ordinal}, {"accepted_ordinal", value.accepted_ordinal}};
}
std::vector<timing_edit::EditBatch> parsePlans(const Json& value, const Json& baseline_json, std::string_view field)
{
  if (!value.is_array())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "expected array");
  std::vector<timing_edit::EditBatch> result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const std::string plan_field = std::string(field) + "[" + std::to_string(index) + "]";
    exactObject(value[index], plan_field, {"schema_version", "baseline", "expected_generation", "edits"});
    if (text(value[index].at("schema_version"), plan_field + ".schema_version") != timing_edit::kDesignEditPlanSchema)
      throw WireError(BufferTraceContractCode::kSchemaMismatch, plan_field + ".schema_version", "unknown design plan schema");
    if (u64(value[index].at("expected_generation"), plan_field + ".expected_generation") != index)
      throw WireError(BufferTraceContractCode::kGenerationMismatch, plan_field + ".expected_generation", "batches must be contiguous from zero");
    if (value[index].at("baseline") != baseline_json)
      throw WireError(BufferTraceContractCode::kIdentityMismatch, plan_field + ".baseline", "baseline identity mismatch");
    result.push_back({.expected_generation = timing_edit::TransactionGeneration{index},
                      .edits = vectorOf<timing_edit::BufferInsertionMutation>(
                          value[index].at("edits"), plan_field + ".edits", timing_edit::json_detail::edit)});
  }
  return result;
}
Json plansJson(const timing_edit::BaselineSemanticIdentity& baseline, const std::vector<timing_edit::EditBatch>& plans)
{
  const Json baseline_json = timing_edit::json_detail::toJson(baseline);
  Json result = Json::array();
  for (std::size_t index = 0; index < plans.size(); ++index) {
    Json edits = Json::array();
    for (const auto& edit : plans[index].edits)
      edits.push_back(timing_edit::json_detail::toJson(edit));
    result.push_back({{"schema_version", timing_edit::kDesignEditPlanSchema}, {"baseline", baseline_json},
                      {"expected_generation", index}, {"edits", std::move(edits)}});
  }
  return result;
}
Json defaultBaselineJson()
{
  return {{"design_name", ""}, {"fingerprints", {{"schema_version", csr::kCsrFingerprintSchemaVersion}, {"topology", ""},
      {"clock_constraint", ""}, {"library", ""}, {"corner", ""}, {"parasitic", ""}, {"timing_input", ""}}},
      {"canonical_pre_state", {{"instances", Json::array()}, {"nets", Json::array()}, {"nodes", Json::array()}, {"arcs", Json::array()}}}};
}
ReplayHeader parseHeader(const Json& value)
{
  exactObject(value, "replay.header", {"schema_version", "selection_mode", "semantic_workload_kind", "profile_id", "comparability_status"},
              {"comparability_reason"});
  ReplayHeader result;
  result.schema_version = u32(value.at("schema_version"), "replay.header.schema_version");
  result.selection_mode = enumValue<SelectionMode>(value.at("selection_mode"), "replay.header.selection_mode", {{"fixed_trace", SelectionMode::kFixedTrace}});
  result.workload_kind = enumValue<SemanticWorkloadKind>(value.at("semantic_workload_kind"), "replay.header.semantic_workload_kind",
                                                         {{"synthetic", SemanticWorkloadKind::kSynthetic}, {"real_design", SemanticWorkloadKind::kRealDesign}});
  result.profile_id = enumValue<ProfileId>(value.at("profile_id"), "replay.header.profile_id",
                                           {{"paper_protocol_top1000_x1", ProfileId::kPaperProtocolTop1000X1},
                                            {"paper_protocol_top100_x10", ProfileId::kPaperProtocolTop100X10},
                                            {"engineering_smoke", ProfileId::kEngineeringSmoke}});
  result.comparability_status = enumValue<ComparabilityStatus>(value.at("comparability_status"), "replay.header.comparability_status",
                                                               {{"exact", ComparabilityStatus::kExact}, {"protocol_only", ComparabilityStatus::kProtocolOnly},
                                                                {"non_comparable", ComparabilityStatus::kNonComparable}});
  result.comparability_reason = value.contains("comparability_reason")
                                    ? std::optional{enumValue<ComparabilityReason>(value.at("comparability_reason"),
                                                                                   "replay.header.comparability_reason",
                                                                                   {{"engineering_rc_no_physical",
                                                                                     ComparabilityReason::kEngineeringRcNoPhysical}})}
                                    : std::nullopt;
  return result;
}
template <typename Recipe>
Recipe parseRecipe(const Json& value, std::string_view field)
{
  exactObject(value, field, {"logical_name", "source_text"});
  return {text(value.at("logical_name"), std::string(field) + ".logical_name"), text(value.at("source_text"), std::string(field) + ".source_text")};
}
ReplayProgram parseReplay(const Json& value)
{
  exactObject(value, "replay", {"header", "workload"});
  ReplayProgram result;
  result.header = parseHeader(value.at("header"));
  const Json& workload = value.at("workload");
  if (!workload.is_object() || !workload.contains("kind"))
    throw WireError(BufferTraceContractCode::kInvalidTaggedUnion, "replay.workload", "missing workload discriminator");
  const std::string kind = text(workload.at("kind"), "replay.workload.kind");
  if (kind == "synthetic") {
    exactObject(workload, "replay.workload", {"kind", "source_recipe", "batches"});
    const auto& recipe = workload.at("source_recipe");
    exactObject(recipe, "replay.workload.source_recipe", {"tech_lef", "cell_lef", "liberty", "verilog", "sdc", "top"});
    SyntheticArtifactRecipe source{parseRecipe<TechLefRecipe>(recipe.at("tech_lef"), "replay.workload.source_recipe.tech_lef"),
                                   parseRecipe<CellLefRecipe>(recipe.at("cell_lef"), "replay.workload.source_recipe.cell_lef"),
                                   parseRecipe<LibertyRecipe>(recipe.at("liberty"), "replay.workload.source_recipe.liberty"),
                                   parseRecipe<VerilogRecipe>(recipe.at("verilog"), "replay.workload.source_recipe.verilog"),
                                   parseRecipe<SdcRecipe>(recipe.at("sdc"), "replay.workload.source_recipe.sdc"),
                                   text(recipe.at("top"), "replay.workload.source_recipe.top")};
    const Json& baseline_json = workload.at("batches").empty() ? workload.at("batches") : workload.at("batches").at(0).at("baseline");
    result.workload = SyntheticReplayPayload{std::move(source), workload.at("batches").empty() ? timing_edit::BaselineSemanticIdentity{} : timing_edit::json_detail::baseline(baseline_json, "replay.workload.baseline"), workload.at("batches").empty()
                                                                      ? std::vector<timing_edit::EditBatch>{}
                                                                      : parsePlans(workload.at("batches"), baseline_json, "replay.workload.batches")};
  } else if (kind == "real_design") {
    exactObject(workload, "replay.workload", {"kind", "artifacts", "baseline", "batches"});
    const auto& sources = workload.at("artifacts");
    SourceArtifactRefs refs = json_detail::parseSources(sources);
    const bool default_template = workload.at("batches").empty() && workload.at("baseline") == defaultBaselineJson();
    auto baseline_value = default_template ? timing_edit::BaselineSemanticIdentity{} : timing_edit::json_detail::baseline(workload.at("baseline"), "replay.workload.baseline");
    result.workload = RealReplayPayload{std::move(refs), std::move(baseline_value),
                                        parsePlans(workload.at("batches"), workload.at("baseline"), "replay.workload.batches")};
  } else {
    throw WireError(BufferTraceContractCode::kInvalidTaggedUnion, "replay.workload.kind", "unknown workload discriminator");
  }
  return result;
}
SelectionProfile parseProfile(const Json& value)
{
  exactObject(value, "generation_only.profile", {"id", "top_k", "rounds", "max_total_insertions", "analysis", "operating_point", "rc_profile"});
  const auto id = enumValue<ProfileId>(value.at("id"), "generation_only.profile.id",
                                      {{"paper_protocol_top1000_x1", ProfileId::kPaperProtocolTop1000X1},
                                       {"paper_protocol_top100_x10", ProfileId::kPaperProtocolTop100X10},
                                       {"engineering_smoke", ProfileId::kEngineeringSmoke}});
  if (text(value.at("analysis"), "generation_only.profile.analysis") != "cpu_full")
    throw WireError(BufferTraceContractCode::kInvalidProfile, "generation_only.profile.analysis", "only cpu_full is supported");
  const auto& point = value.at("operating_point");
  const auto& rc = value.at("rc_profile");
  exactObject(point, "generation_only.profile.operating_point", {"input_slew_ns", "output_load_pf"});
  exactObject(rc, "generation_only.profile.rc_profile", {"id", "upstream_resistance_kohm", "buffer_resistance_kohm"});
  return {id, u64(value.at("top_k"), "generation_only.profile.top_k"), u64(value.at("rounds"), "generation_only.profile.rounds"),
          u64(value.at("max_total_insertions"), "generation_only.profile.max_total_insertions"), AnalysisMode::kCpuFull,
          {number(point.at("input_slew_ns"), "generation_only.profile.operating_point.input_slew_ns"),
           number(point.at("output_load_pf"), "generation_only.profile.operating_point.output_load_pf")},
          {text(rc.at("id"), "generation_only.profile.rc_profile.id"),
           number(rc.at("upstream_resistance_kohm"), "generation_only.profile.rc_profile.upstream_resistance_kohm"),
           number(rc.at("buffer_resistance_kohm"), "generation_only.profile.rc_profile.buffer_resistance_kohm")}};
}
GainComputationReceipt parseNumeric(const Json& value, std::string_view field)
{
  exactObject(value, field, {"arrival_ns", "required_ns", "slack_ns", "upstream_resistance_kohm", "candidate_load_pf", "buffer_capacitance_pf",
                             "delta_capacitance_pf", "benefit_ns", "buffer_resistance_kohm", "penalty_ns", "benefit_minus_penalty_ns",
                             "buffer_delay_ns", "gain_ns"});
  const auto get = [&](std::string_view name) { return number(value.at(std::string(name)), std::string(field) + "." + std::string(name)); };
  return {get("arrival_ns"), get("required_ns"), get("slack_ns"), get("upstream_resistance_kohm"), get("candidate_load_pf"),
          get("buffer_capacitance_pf"), get("delta_capacitance_pf"), get("benefit_ns"), get("buffer_resistance_kohm"), get("penalty_ns"),
          get("benefit_minus_penalty_ns"), get("buffer_delay_ns"), get("gain_ns")};
}
const timing_edit::BaselineSemanticIdentity& replayBaseline(const ReplayProgram& replay)
{
  if (replay.baseline() == timing_edit::BaselineSemanticIdentity{})
    throw WireError(BufferTraceContractCode::kInvalidValue, "generation_only", "generation receipt requires a replay baseline");
  return replay.baseline();
}
GenerationReceipt parseGeneration(const Json& value, const ReplayProgram& replay)
{
  exactObject(value, "generation_only", {"profile", "rounds", "stop_reason"});
  GenerationReceipt result;
  result.profile = parseProfile(value.at("profile"));
  const auto parse_round = [&](const Json& round, std::string field) {
    exactObject(round, field, {"batch_ordinal", "evaluations", "accepted_tokens"});
    GenerationRoundReceipt parsed;
    parsed.batch_ordinal = u64(round.at("batch_ordinal"), field + ".batch_ordinal");
    parsed.evaluations = vectorOf<CandidateEvaluationReceipt>(round.at("evaluations"), field + ".evaluations", [&](const Json& evaluation, std::string eval_field) {
      exactObject(evaluation, eval_field, {"net", "driver", "candidate_pin", "buffer", "decision"},
                  {"numeric", "rejection_reason", "accepted_creation_token"});
      const Json* token_value = evaluation.contains("accepted_creation_token") ? &evaluation.at("accepted_creation_token") : nullptr;
      CandidateEvaluationReceipt receipt{
          timing_edit::json_detail::netRef(evaluation.at("net"), eval_field + ".net"),
          timing_edit::json_detail::nodeRef(evaluation.at("driver"), eval_field + ".driver"),
          timing_edit::json_detail::nodeRef(evaluation.at("candidate_pin"), eval_field + ".candidate_pin"),
          timing_edit::json_detail::libraryCell(evaluation.at("buffer"), eval_field + ".buffer"),
          std::nullopt, CandidateDecision::kRejected, std::nullopt, std::nullopt};
      if (evaluation.contains("numeric")) receipt.numeric = parseNumeric(evaluation.at("numeric"), eval_field + ".numeric");
      receipt.decision = enumValue<CandidateDecision>(evaluation.at("decision"), eval_field + ".decision",
                                                     {{"accepted", CandidateDecision::kAccepted}, {"non_critical_net", CandidateDecision::kNonCriticalNet},
                                                      {"lower_ranked_candidate", CandidateDecision::kLowerRankedCandidate}, {"rejected", CandidateDecision::kRejected}});
      if (evaluation.contains("rejection_reason"))
        receipt.rejection_reason = enumValue<CandidateRejectionReason>(evaluation.at("rejection_reason"), eval_field + ".rejection_reason",
            {{"invalid_semantic_reference", CandidateRejectionReason::kInvalidSemanticReference}, {"invalid_moved_sink_set", CandidateRejectionReason::kInvalidMovedSinkSet},
             {"invalid_buffer_cell", CandidateRejectionReason::kInvalidBufferCell}, {"invalid_environment_inputs", CandidateRejectionReason::kInvalidEnvironmentInputs},
             {"timing_not_materialized", CandidateRejectionReason::kTimingNotMaterialized}, {"non_finite_timing", CandidateRejectionReason::kNonFiniteTiming},
             {"slack_mismatch", CandidateRejectionReason::kSlackMismatch}, {"negative_operand", CandidateRejectionReason::kNegativeOperand},
             {"non_finite_intermediate", CandidateRejectionReason::kNonFiniteIntermediate}, {"non_positive_gain", CandidateRejectionReason::kNonPositiveGain}});
      if (token_value) receipt.accepted_creation_token = creationToken(*token_value, eval_field + ".accepted_creation_token");
      return receipt;
    });
    parsed.accepted_tokens = vectorOf<timing_edit::CreationToken>(round.at("accepted_tokens"), field + ".accepted_tokens", creationToken);
    return parsed;
  };
  result.rounds = vectorOf<GenerationRoundReceipt>(value.at("rounds"), "generation_only.rounds", parse_round);
  result.stop_reason = enumValue<GenerationStopReason>(value.at("stop_reason"), "generation_only.stop_reason",
      {{"empty_accepted_batch", GenerationStopReason::kEmptyAcceptedBatch}, {"rounds_reached", GenerationStopReason::kRoundsReached},
       {"max_total_insertions_reached", GenerationStopReason::kMaxTotalInsertionsReached}});
  static_cast<void>(replayBaseline(replay));
  return result;
}
Json toReplay(const ReplayProgram& value)
{
  Json header{{"schema_version", value.header.schema_version}, {"selection_mode", token(value.header.selection_mode)},
              {"semantic_workload_kind", token(value.header.workload_kind)}, {"profile_id", token(value.header.profile_id)},
              {"comparability_status", token(value.header.comparability_status)}};
  if (value.header.comparability_reason) header["comparability_reason"] = token(*value.header.comparability_reason);
  Json workload = std::visit([](const auto& payload) -> Json {
    using Payload = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<Payload, SyntheticReplayPayload>) {
      const auto recipe = [](const auto& item) { return Json{{"logical_name", item.logical_name}, {"source_text", item.source_text}}; };
      return {{"kind", "synthetic"}, {"source_recipe", {{"tech_lef", recipe(payload.source_recipe.tech_lef)},
               {"cell_lef", recipe(payload.source_recipe.cell_lef)}, {"liberty", recipe(payload.source_recipe.liberty)},
               {"verilog", recipe(payload.source_recipe.verilog)}, {"sdc", recipe(payload.source_recipe.sdc)}, {"top", payload.source_recipe.top}}},
              {"batches", payload.batches.empty() ? Json::array() : plansJson(payload.baseline, payload.batches)}};
    } else {
      const Json plan_values = payload.batches.empty() ? Json::array() : plansJson(payload.baseline, payload.batches);
      const Json baseline_json = payload.baseline == timing_edit::BaselineSemanticIdentity{} ? defaultBaselineJson()
                                 : plan_values.empty() ? timing_edit::json_detail::toJson(payload.baseline)
                                                       : plan_values.front().at("baseline");
      return {{"kind", "real_design"}, {"artifacts", json_detail::sourcesToJson(payload.artifacts)},
              {"baseline", baseline_json}, {"batches", plan_values}};
    }
  }, value.workload);
  return {{"header", std::move(header)}, {"workload", std::move(workload)}};
}
Json toProfile(const SelectionProfile& value)
{
  return {{"id", token(value.id)}, {"top_k", value.top_k}, {"rounds", value.rounds}, {"max_total_insertions", value.max_total_insertions},
          {"analysis", token(value.analysis)}, {"operating_point", {{"input_slew_ns", value.operating_point.input_slew_ns},
                                                                    {"output_load_pf", value.operating_point.output_load_pf}}},
          {"rc_profile", {{"id", value.rc_profile.id}, {"upstream_resistance_kohm", value.rc_profile.upstream_resistance_kohm},
                          {"buffer_resistance_kohm", value.rc_profile.buffer_resistance_kohm}}}};
}
Json toNumeric(const GainComputationReceipt& value)
{
  return {{"arrival_ns", value.arrival_ns}, {"required_ns", value.required_ns}, {"slack_ns", value.slack_ns},
          {"upstream_resistance_kohm", value.upstream_resistance_kohm}, {"candidate_load_pf", value.candidate_load_pf},
          {"buffer_capacitance_pf", value.buffer_capacitance_pf}, {"delta_capacitance_pf", value.delta_capacitance_pf},
          {"benefit_ns", value.benefit_ns}, {"buffer_resistance_kohm", value.buffer_resistance_kohm}, {"penalty_ns", value.penalty_ns},
          {"benefit_minus_penalty_ns", value.benefit_minus_penalty_ns}, {"buffer_delay_ns", value.buffer_delay_ns}, {"gain_ns", value.gain_ns}};
}
Json toGeneration(const GenerationReceipt& value, const ReplayProgram& replay)
{
  Json rounds = Json::array();
  static_cast<void>(replayBaseline(replay));
  for (const auto& round : value.rounds) {
    Json evaluations = Json::array();
    for (const auto& evaluation : round.evaluations) {
      Json item{{"net", timing_edit::json_detail::toJson(evaluation.net)},
                {"driver", timing_edit::json_detail::toJson(evaluation.driver)},
                {"candidate_pin", timing_edit::json_detail::toJson(evaluation.candidate_pin)},
                {"buffer", timing_edit::json_detail::toJson(evaluation.buffer)},
                {"decision", token(evaluation.decision)}};
      if (evaluation.numeric) item["numeric"] = toNumeric(*evaluation.numeric);
      if (evaluation.rejection_reason) item["rejection_reason"] = token(*evaluation.rejection_reason);
      if (evaluation.accepted_creation_token) item["accepted_creation_token"] = toJson(*evaluation.accepted_creation_token);
      evaluations.push_back(std::move(item));
    }
    Json accepted = Json::array();
    for (const auto& creation_token : round.accepted_tokens) accepted.push_back(toJson(creation_token));
    rounds.push_back({{"batch_ordinal", round.batch_ordinal}, {"evaluations", std::move(evaluations)}, {"accepted_tokens", std::move(accepted)}});
  }
  return {{"profile", toProfile(value.profile)}, {"rounds", std::move(rounds)}, {"stop_reason", token(value.stop_reason)}};
}
template <typename Result, typename Parse>
Result parseResult(std::string_view text_value, Parse parse)
{
  try {
    return parse(parseClosedWorldJson(text_value));
  } catch (const WireError& error) {
    return BufferTraceContractViolations{{error.code, error.field, error.what()}};
  } catch (const timing_edit::json_detail::WireError& error) {
    return BufferTraceContractViolations{{BufferTraceContractCode::kInvalidValue, error.field, error.what()}};
  } catch (const nlohmann::json::exception& error) {
    return BufferTraceContractViolations{{BufferTraceContractCode::kInvalidValue, "json", error.what()}};
  } catch (const std::exception& error) {
    return BufferTraceContractViolations{{BufferTraceContractCode::kInvalidValue, "json", error.what()}};
  }
}
ReplayProgram parseAndValidateReplay(const Json& value, bool template_only)
{
  ReplayProgram result = parseReplay(value);
  const auto errors = template_only ? validateReplayTemplate(result) : validateReplayProgram(result);
  if (!errors.empty()) throw WireError(errors.front().code, errors.front().field, errors.front().message);
  return result;
}
}  // namespace
ReplayProgramJsonParseResult parseReplayProgramJson(std::string_view json_text)
{
  return parseResult<ReplayProgramJsonParseResult>(json_text, [](const Json& value) { return parseAndValidateReplay(value, false); });
}
ReplayProgramJsonParseResult parseReplayTemplateJson(std::string_view json_text)
{
  return parseResult<ReplayProgramJsonParseResult>(json_text, [](const Json& value) { return parseAndValidateReplay(value, true); });
}
BufferTraceArtifactJsonParseResult parseBufferTraceArtifactJson(std::string_view json_text)
{
  return parseResult<BufferTraceArtifactJsonParseResult>(json_text, [](const Json& value) {
    exactObject(value, "artifact", {"replay", "generation_only"});
    ReplayProgram replay = parseReplay(value.at("replay"));
    GenerationReceipt generation = parseGeneration(value.at("generation_only"), replay);
    BufferTraceArtifact result{std::move(replay), std::move(generation)};
    if (const auto errors = validateBufferTraceArtifact(result); !errors.empty())
      throw WireError(errors.front().code, errors.front().field, errors.front().message);
    return result;
  });
}
std::string replayProgramToJson(const ReplayProgram& program)
{
  if (const auto errors = validateReplayProgram(program); !errors.empty())
    throw std::invalid_argument(errors.front().field + ": " + errors.front().message);
  return toReplay(program).dump();
}
std::string replayTemplateToJson(const ReplayProgram& program)
{
  if (const auto errors = validateReplayTemplate(program); !errors.empty())
    throw std::invalid_argument(errors.front().field + ": " + errors.front().message);
  return toReplay(program).dump();
}
std::string bufferTraceArtifactToJson(const BufferTraceArtifact& artifact)
{
  if (const auto errors = validateBufferTraceArtifact(artifact); !errors.empty())
    throw std::invalid_argument(errors.front().field + ": " + errors.front().message);
  return Json{{"replay", toReplay(artifact.replay)},
              {"generation_only", toGeneration(artifact.generation_only, artifact.replay)}}.dump();
}
}  // namespace ista::buffer_trace
