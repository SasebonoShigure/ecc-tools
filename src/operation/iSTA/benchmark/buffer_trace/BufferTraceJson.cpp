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
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include "TimingEditTrace.hpp"
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
  std::map<int, std::set<std::string>> keys;
  const Json::parser_callback_t duplicate_rejector = [&](int depth, Json::parse_event_t event, Json& parsed) {
    if (event == Json::parse_event_t::object_start)
      keys[depth + 1].clear();
    else if (event == Json::parse_event_t::key) {
      const std::string key = parsed.get<std::string>();
      if (!keys[depth].insert(key).second)
        throw WireError(BufferTraceContractCode::kDuplicate, "json", "duplicate object key: " + key);
    }
    return true;
  };
  return Json::parse(text.begin(), text.end(), duplicate_rejector, true, false);
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
Json proxyTable()
{
  return {{"present", false}, {"variable1", "none"}, {"variable2", "none"}};
}
Json proxyLibraryCell()
{
  const Json signature{{"library_name", "__buffer_wire_proxy_lib__"},
                       {"arc_index", 0},
                       {"sense", "positive_unate"},
                       {"trigger_transition", "none"},
                       {"check_transition", "none"},
                       {"sdf_condition", ""},
                       {"delay_tables", Json::array({proxyTable(), proxyTable()})},
                       {"slew_tables", Json::array({proxyTable(), proxyTable()})}};
  return {{"library_name", "__buffer_wire_proxy_lib__"}, {"cell_name", "BUF_PROXY"}, {"input_port", "A"}, {"output_port", "Y"},
          {"ordered_arc_signatures", Json::array({signature})}};
}
std::string proxySuffix(std::uint64_t batch, std::uint64_t accepted)
{
  std::string index = std::to_string(accepted);
  return std::to_string(batch) + "_" + std::string(20 - index.size(), '0') + index;
}
Json proxyNode(const std::string& suffix)
{
  return {{"kind", "existing"},
          {"baseline_key", {{"kind", "port"}, {"full_name", "__buffer_wire_pin_" + suffix},
                            {"instance_name", ""}, {"pin_name", ""}}}};
}
Json proxyToken(std::uint64_t ordinal, std::string profile = "__buffer_wire_proxy__", std::uint64_t seed = 0,
                std::string case_id = "__buffer_wire_proxy__")
{
  return {{"schema_version", 1}, {"case_id", std::move(case_id)}, {"profile_id", std::move(profile)}, {"seed", seed},
          {"batch_ordinal", ordinal}, {"accepted_ordinal", 0}};
}
Json proxyEdit(std::uint64_t ordinal, const Json& token_value)
{
  const std::string suffix = proxySuffix(ordinal, token_value.at("accepted_ordinal").get<std::uint64_t>());
  return {{"original_net", {{"kind", "existing"}, {"baseline_name", "__buffer_wire_net_" + suffix}}},
          {"moved_sinks", Json::array({proxyNode(suffix)})}, {"library_cell", proxyLibraryCell()},
          {"placement_inputs", {{"kind", "unplaced"}}}, {"rc_inputs", {{"kind", "no_parasitic"}}}, {"creation_token", token_value}};
}
Json identity(const Json& baseline, std::uint64_t generation)
{
  return {{"generation", generation}, {"current_fingerprints", baseline.at("fingerprints")},
          {"canonical_pre_state", baseline.at("canonical_pre_state")}};
}
Json stage3Envelope(const Json& baseline, const std::vector<Json>& plan_edits)
{
  Json batches = Json::array();
  std::string profile = "__buffer_wire_proxy__";
  std::uint64_t seed = 0;
  for (std::size_t index = 0; index < plan_edits.size(); ++index) {
    const auto& edits = plan_edits[index];
    const auto& first_token = edits.at(0).at("creation_token");
    if (index == 0) {
      profile = first_token.at("profile_id").get<std::string>();
      seed = first_token.at("seed").get<std::uint64_t>();
    }
    bool full = false;
    for (const auto& edit : edits)
      full = full || (edit.at("rc_inputs").at("kind") == "parasitic" && edit.at("rc_inputs").at("policy") == "full_with_record");
    batches.push_back({{"ordinal", index + 1}, {"expected_pre_identity", identity(baseline, index)}, {"edits", edits},
                       {"policy_receipt", {{"disposition", full ? "full_with_record" : "supported"},
                                           {"reasons", full ? Json::array({"stage4_wire_proxy_full"}) : Json::array()}}},
                       {"expected_post_identity", identity(baseline, index + 1)}});
  }
  return {{"schema_version", timing_edit::kTimingEditTraceSchema}, {"workload_kind", "synthetic"}, {"seed", seed}, {"profile", profile},
          {"baseline", baseline}, {"source_artifacts", Json::array()}, {"batches", std::move(batches)},
          {"replay_command", {{"executable", "ista_buffer_wire_proxy"}, {"arguments", {"--trace", "{trace}", "--prefix", "{prefix}"}}}}};
}
timing_edit::TimingEditTrace parseStage3(const Json& wire)
{
  auto parsed = timing_edit::parseTimingEditTraceJson(wire.dump());
  if (const auto* errors = std::get_if<timing_edit::TimingEditContractViolations>(&parsed))
    throw WireError(BufferTraceContractCode::kInvalidValue, "stage3", errors->empty() ? "Stage 3 parse failed" : errors->front().message);
  return std::get<timing_edit::TimingEditTrace>(std::move(parsed));
}
timing_edit::BaselineSemanticIdentity parseBaseline(const Json& value)
{
  const std::vector<Json> edits{Json::array({proxyEdit(1, proxyToken(1))})};
  return parseStage3(stage3Envelope(value, edits)).baseline;
}
std::vector<timing_edit::DesignEditPlan> parsePlans(const Json& value, const Json& baseline_json, std::string_view field)
{
  if (!value.is_array())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "expected array");
  if (value.empty())
    return {};
  std::vector<Json> edits;
  edits.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const std::string plan_field = std::string(field) + "[" + std::to_string(index) + "]";
    exactObject(value[index], plan_field, {"schema_version", "baseline", "expected_generation", "edits"});
    if (text(value[index].at("schema_version"), plan_field + ".schema_version") != timing_edit::kDesignEditPlanSchema)
      throw WireError(BufferTraceContractCode::kSchemaMismatch, plan_field + ".schema_version", "unknown design plan schema");
    if (u64(value[index].at("expected_generation"), plan_field + ".expected_generation") != index)
      throw WireError(BufferTraceContractCode::kGenerationMismatch, plan_field + ".expected_generation", "batches must be contiguous from zero");
    if (value[index].at("baseline") != baseline_json)
      throw WireError(BufferTraceContractCode::kIdentityMismatch, plan_field + ".baseline", "baseline identity mismatch");
    if (!value[index].at("edits").is_array() || value[index].at("edits").empty())
      throw WireError(BufferTraceContractCode::kInvalidValue, plan_field + ".edits", "at least one edit is required");
    edits.push_back(value[index].at("edits"));
  }
  auto trace = parseStage3(stage3Envelope(baseline_json, edits));
  std::vector<timing_edit::DesignEditPlan> result;
  result.reserve(trace.batches.size());
  for (std::size_t index = 0; index < trace.batches.size(); ++index)
    result.push_back(timing_edit::DesignEditPlan{.baseline = trace.baseline,
                                                 .expected_generation = timing_edit::TransactionGeneration{index},
                                                 .edits = std::move(trace.batches[index].edits)});
  return result;
}
timing_edit::TimingEditTrace makeStage3Trace(const timing_edit::BaselineSemanticIdentity& baseline,
                                             const std::vector<timing_edit::DesignEditPlan>& plans)
{
  timing_edit::TimingEditTrace trace;
  trace.profile = plans.front().edits.front().creation_token.profile_id;
  trace.seed = plans.front().edits.front().creation_token.seed;
  trace.baseline = baseline;
  trace.replay_command = {"ista_buffer_wire_proxy", {"--trace", "{trace}", "--prefix", "{prefix}"}};
  for (std::size_t index = 0; index < plans.size(); ++index) {
    bool full = false;
    for (const auto& edit : plans[index].edits)
      if (const auto* rc = std::get_if<timing_edit::ParasiticRcInputs>(&edit.rc_inputs))
        full = full || rc->parasitic_policy == timing_edit::ParasiticEditPolicy::kFullWithRecord;
    timing_edit::CommittedEditIdentity pre{timing_edit::TransactionGeneration{index}, baseline.fingerprints, baseline.canonical_pre_state};
    timing_edit::CommittedEditIdentity post{timing_edit::TransactionGeneration{index + 1}, baseline.fingerprints, baseline.canonical_pre_state};
    trace.batches.push_back({index + 1, std::move(pre), plans[index].edits,
                             {full ? timing_edit::EditDisposition::kFullWithRecord : timing_edit::EditDisposition::kSupported,
                              full ? std::vector<std::string>{"stage4_wire_proxy_full"} : std::vector<std::string>{}},
                             std::move(post)});
  }
  return trace;
}
Json plansJson(const timing_edit::BaselineSemanticIdentity& baseline, const std::vector<timing_edit::DesignEditPlan>& plans)
{
  const Json trace = Json::parse(timing_edit::timingEditTraceToJson(makeStage3Trace(baseline, plans)));
  Json result = Json::array();
  for (std::size_t index = 0; index < plans.size(); ++index)
    result.push_back({{"schema_version", timing_edit::kDesignEditPlanSchema}, {"baseline", trace.at("baseline")},
                      {"expected_generation", index}, {"edits", trace.at("batches").at(index).at("edits")}});
  return result;
}
std::vector<Json> referenceTokens(const Json& net, const Json& sink)
{
  std::vector<Json> result;
  for (const Json* reference : {&net, &sink})
    if (reference->at("kind") == "created") result.push_back(reference->at("creation_token"));
  return result;
}
template <typename Ref>
std::optional<timing_edit::CreationToken> referenceToken(const Ref& reference)
{
  return std::visit([](const auto& value) -> std::optional<timing_edit::CreationToken> {
    using Value = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<Value, timing_edit::CreatedNodeRef> || std::is_same_v<Value, timing_edit::CreatedNetRef>)
      return value.creation_token;
    return std::nullopt;
  }, reference);
}
std::vector<Json> proxyJsonBatches(std::uint64_t ordinal, const Json& identity, const std::vector<Json>& references)
{
  std::vector<Json> batches;
  for (std::uint64_t batch = 1; batch < ordinal; ++batch) {
    std::uint64_t maximum = 0;
    for (const auto& reference : references)
      if (reference.at("batch_ordinal") == batch) maximum = std::max(maximum, reference.at("accepted_ordinal").get<std::uint64_t>());
    Json edits = Json::array();
    for (std::uint64_t accepted = 0; accepted <= maximum; ++accepted) {
      Json token_value = proxyToken(batch, identity.at("profile_id"), identity.at("seed"), identity.at("case_id"));
      token_value["accepted_ordinal"] = accepted;
      for (const auto& reference : references)
        if (reference.at("batch_ordinal") == batch && reference.at("accepted_ordinal") == accepted) token_value = reference;
      edits.push_back(proxyEdit(batch, token_value));
    }
    batches.push_back(std::move(edits));
  }
  return batches;
}
timing_edit::BufferInsertionMutation parseProxyEdit(const Json& baseline, const Json& net, const Json& sink, const Json& library,
                                                    const Json* accepted_token)
{
  const std::vector<Json> references = referenceTokens(net, sink);
  std::uint64_t referenced = 0;
  for (const auto& reference : references)
    referenced = std::max(referenced, reference.at("batch_ordinal").get<std::uint64_t>());
  Json token_value = accepted_token ? *accepted_token : references.empty() ? proxyToken(1) : references.front();
  const std::uint64_t ordinal = accepted_token ? accepted_token->at("batch_ordinal").get<std::uint64_t>()
                                                : std::max<std::uint64_t>(referenced + 1, 1);
  if (!accepted_token) {
    token_value["batch_ordinal"] = ordinal;
    token_value["accepted_ordinal"] = 0;
  }
  std::vector<Json> batches = proxyJsonBatches(ordinal, token_value, references);
  Json target = proxyEdit(ordinal, token_value);
  target["original_net"] = net;
  target["moved_sinks"] = Json::array({sink});
  target["library_cell"] = library;
  batches.push_back(Json::array({std::move(target)}));
  return parseStage3(stage3Envelope(baseline, batches)).batches.back().edits.front();
}
Json proxyEditJson(const timing_edit::BaselineSemanticIdentity& baseline, const timing_edit::StableNetRef& net,
                   const timing_edit::StableNodeRef& sink, const timing_edit::LibraryCellRef& library,
                   const timing_edit::CreationToken* accepted_token)
{
  std::vector<timing_edit::CreationToken> references;
  if (auto value = referenceToken(net)) references.push_back(*value);
  if (auto value = referenceToken(sink)) references.push_back(*value);
  std::uint64_t referenced = 0;
  for (const auto& value : references) referenced = std::max(referenced, value.batch_ordinal);
  timing_edit::CreationToken token_value
      = accepted_token ? *accepted_token
                       : references.empty() ? timing_edit::CreationToken{.case_id = "__buffer_wire_proxy__",
                                                                           .profile_id = "__buffer_wire_proxy__",
                                                                           .batch_ordinal = 1}
                                            : references.front();
  if (!accepted_token) {
    token_value.batch_ordinal = std::max<std::uint64_t>(referenced + 1, 1);
    token_value.accepted_ordinal = 0;
  }
  const std::uint64_t ordinal = token_value.batch_ordinal;
  std::vector<timing_edit::DesignEditPlan> plans;
  for (std::uint64_t batch = 1; batch < ordinal; ++batch) {
    std::uint64_t maximum = 0;
    for (const auto& reference : references)
      if (reference.batch_ordinal == batch) maximum = std::max(maximum, reference.accepted_ordinal);
    std::vector<timing_edit::BufferInsertionMutation> edits;
    for (std::uint64_t accepted = 0; accepted <= maximum; ++accepted) {
      timing_edit::CreationToken dummy = token_value;
      dummy.batch_ordinal = batch;
      dummy.accepted_ordinal = accepted;
      for (const auto& reference : references)
        if (reference.batch_ordinal == batch && reference.accepted_ordinal == accepted) dummy = reference;
      const std::string suffix = proxySuffix(batch, accepted);
      edits.push_back({timing_edit::ExistingNetRef{"__buffer_wire_net_" + suffix},
                       {timing_edit::ExistingNodeRef{{csr::NodeKind::kPort, "__buffer_wire_pin_" + suffix, "", ""}}},
                       library, timing_edit::UnplacedInputs{}, timing_edit::NoParasiticRcInputs{}, std::move(dummy)});
    }
    plans.push_back({.baseline = baseline, .expected_generation = timing_edit::TransactionGeneration{batch - 1}, .edits = std::move(edits)});
  }
  timing_edit::BufferInsertionMutation target{net, {sink}, library, timing_edit::UnplacedInputs{}, timing_edit::NoParasiticRcInputs{}, token_value};
  plans.push_back({.baseline = baseline, .expected_generation = timing_edit::TransactionGeneration{ordinal - 1}, .edits = {std::move(target)}});
  return plansJson(baseline, plans).back().at("edits").at(0);
}
Json defaultBaselineJson()
{
  return {{"design_name", ""}, {"fingerprints", {{"schema_version", csr::kCsrSchemaVersion}, {"topology", ""},
      {"clock_constraint", ""}, {"library", ""}, {"corner", ""}, {"parasitic", ""}, {"timing_input", ""}}},
      {"canonical_pre_state", {{"instances", Json::array()}, {"nets", Json::array()}, {"nodes", Json::array()}, {"arcs", Json::array()}}}};
}
Json baselineJson(const timing_edit::BaselineSemanticIdentity& baseline)
{
  csr::LibraryArcSignature arc;
  arc.library_name = "__buffer_wire_proxy_lib__";
  arc.sense = csr::TimingSense::kPositiveUnate;
  const timing_edit::LibraryCellRef library{arc.library_name, "BUF_PROXY", "A", "Y", {std::move(arc)}};
  const timing_edit::CreationToken creation_token{.case_id = "__buffer_wire_proxy__", .profile_id = "__buffer_wire_proxy__",
                                                   .batch_ordinal = 1};
  const timing_edit::BufferInsertionMutation edit{timing_edit::ExistingNetRef{"__buffer_wire_net__"},
      {timing_edit::ExistingNodeRef{{csr::NodeKind::kPort, "__buffer_wire_pin__", "", ""}}}, library,
      timing_edit::UnplacedInputs{}, timing_edit::NoParasiticRcInputs{}, creation_token};
  return plansJson(baseline, {{.baseline = baseline, .expected_generation = timing_edit::TransactionGeneration{0}, .edits = {edit}}})
      .front().at("baseline");
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
    const Json baseline_json = workload.at("batches").empty() ? Json{} : workload.at("batches").at(0).at("baseline");
    result.workload = SyntheticReplayPayload{std::move(source), workload.at("batches").empty()
                                                                      ? std::vector<timing_edit::DesignEditPlan>{}
                                                                      : parsePlans(workload.at("batches"), baseline_json, "replay.workload.batches")};
  } else if (kind == "real_design") {
    exactObject(workload, "replay.workload", {"kind", "artifacts", "baseline", "batches"});
    const auto& sources = workload.at("artifacts");
    SourceArtifactRefs refs = json_detail::parseSources(sources);
    const bool default_template = workload.at("batches").empty() && workload.at("baseline") == defaultBaselineJson();
    auto baseline_value = default_template ? timing_edit::BaselineSemanticIdentity{} : parseBaseline(workload.at("baseline"));
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
  return std::visit([](const auto& payload) -> const timing_edit::BaselineSemanticIdentity& {
    using Payload = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<Payload, RealReplayPayload>)
      return payload.baseline;
    else {
      if (payload.batches.empty())
        throw WireError(BufferTraceContractCode::kInvalidValue, "generation_only", "generation receipt requires a replay baseline");
      return payload.batches.front().baseline;
    }
  }, replay.workload);
}
Json replayBaselineJson(const Json& replay)
{
  const auto& workload = replay.at("workload");
  return workload.at("kind") == "real_design" ? workload.at("baseline") : workload.at("batches").at(0).at("baseline");
}
GenerationReceipt parseGeneration(const Json& value, const ReplayProgram& replay, const Json& replay_json)
{
  exactObject(value, "generation_only", {"profile", "rounds", "stop_reason"});
  GenerationReceipt result;
  result.profile = parseProfile(value.at("profile"));
  const Json baseline_json = replayBaselineJson(replay_json);
  const auto parse_round = [&](const Json& round, std::string field) {
    exactObject(round, field, {"batch_ordinal", "evaluations", "accepted_tokens"});
    GenerationRoundReceipt parsed;
    parsed.batch_ordinal = u64(round.at("batch_ordinal"), field + ".batch_ordinal");
    parsed.evaluations = vectorOf<CandidateEvaluationReceipt>(round.at("evaluations"), field + ".evaluations", [&](const Json& evaluation, std::string eval_field) {
      exactObject(evaluation, eval_field, {"net", "driver", "candidate_pin", "buffer", "decision"},
                  {"numeric", "rejection_reason", "accepted_creation_token"});
      const Json* token_value = evaluation.contains("accepted_creation_token") ? &evaluation.at("accepted_creation_token") : nullptr;
      auto candidate = parseProxyEdit(baseline_json, evaluation.at("net"), evaluation.at("candidate_pin"), evaluation.at("buffer"), token_value);
      auto driver = parseProxyEdit(baseline_json, evaluation.at("net"), evaluation.at("driver"), evaluation.at("buffer"), token_value);
      CandidateEvaluationReceipt receipt{candidate.original_net, driver.moved_sinks.front(), candidate.moved_sinks.front(), candidate.library_cell,
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
              {"batches", payload.batches.empty() ? Json::array() : plansJson(payload.batches.front().baseline, payload.batches)}};
    } else {
      const Json plan_values = payload.batches.empty() ? Json::array() : plansJson(payload.baseline, payload.batches);
      const Json baseline_json = payload.baseline == timing_edit::BaselineSemanticIdentity{} ? defaultBaselineJson()
                                 : plan_values.empty() ? baselineJson(payload.baseline)
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
  const auto& baseline = replayBaseline(replay);
  for (const auto& round : value.rounds) {
    Json evaluations = Json::array();
    for (const auto& evaluation : round.evaluations) {
      const auto* accepted = evaluation.accepted_creation_token ? &*evaluation.accepted_creation_token : nullptr;
      const Json candidate = proxyEditJson(baseline, evaluation.net, evaluation.candidate_pin, evaluation.buffer, accepted);
      const Json driver = proxyEditJson(baseline, evaluation.net, evaluation.driver, evaluation.buffer, accepted);
      Json item{{"net", candidate.at("original_net")}, {"driver", driver.at("moved_sinks").at(0)},
                {"candidate_pin", candidate.at("moved_sinks").at(0)}, {"buffer", candidate.at("library_cell")},
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
    GenerationReceipt generation = parseGeneration(value.at("generation_only"), replay, value.at("replay"));
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
