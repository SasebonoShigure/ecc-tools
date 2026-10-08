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
#include "TimingEditTrace.hpp"
#include "TimingEditJsonInternal.hpp"
#include "StrictJson.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

#include "json.hpp"

namespace ista::timing_edit {
namespace json_detail {

using Json = nlohmann::json;

Json parseClosedWorldJson(std::string_view json_text)
{
  try {
    return ista::json_detail::parseUniqueKeyJson(json_text, true);
  } catch (const ista::json_detail::DuplicateJsonKey& error) {
    throw WireError("trace", error.what());
  }
}

void exactObject(const Json& value, std::string_view field, std::initializer_list<std::string_view> expected)
{
  if (!value.is_object())
    throw WireError(std::string(field), "expected object");
  if (value.size() != expected.size())
    throw WireError(std::string(field), "object key set does not match closed schema");
  for (std::string_view key : expected)
    if (value.find(std::string(key)) == value.end())
      throw WireError(std::string(field) + "." + std::string(key), "required field is absent");
  for (auto item = value.begin(); item != value.end(); ++item)
    if (std::find(expected.begin(), expected.end(), item.key()) == expected.end())
      throw WireError(std::string(field) + "." + item.key(), "unknown field");
}

const Json& array(const Json& value, std::string_view field)
{
  if (!value.is_array())
    throw WireError(std::string(field), "expected array");
  return value;
}

std::string string(const Json& value, std::string_view field)
{
  if (!value.is_string())
    throw WireError(std::string(field), "expected string");
  return value.get<std::string>();
}

std::uint64_t u64(const Json& value, std::string_view field)
{
  if (!value.is_number_unsigned())
    throw WireError(std::string(field), "expected unsigned integer");
  return value.get<std::uint64_t>();
}

std::uint32_t u32(const Json& value, std::string_view field)
{
  const auto result = u64(value, field);
  if (result > std::numeric_limits<std::uint32_t>::max())
    throw WireError(std::string(field), "unsigned integer exceeds uint32");
  return static_cast<std::uint32_t>(result);
}

std::int32_t i32(const Json& value, std::string_view field)
{
  if (!value.is_number_integer())
    throw WireError(std::string(field), "expected signed integer");
  if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
    throw WireError(std::string(field), "unsigned integer exceeds int32");
  const auto result = value.get<std::int64_t>();
  if (result < std::numeric_limits<std::int32_t>::min() || result > std::numeric_limits<std::int32_t>::max())
    throw WireError(std::string(field), "signed integer exceeds int32");
  return static_cast<std::int32_t>(result);
}

double number(const Json& value, std::string_view field)
{
  if (!value.is_number())
    throw WireError(std::string(field), "expected number");
  return value.get<double>();
}

bool boolean(const Json& value, std::string_view field)
{
  if (!value.is_boolean())
    throw WireError(std::string(field), "expected boolean");
  return value.get<bool>();
}

template <typename T, typename Parse>
std::vector<T> vectorOf(const Json& value, std::string_view field, Parse parse)
{
  const auto& values = array(value, field);
  std::vector<T> result;
  result.reserve(values.size());
  for (std::size_t index = 0; index < values.size(); ++index)
    result.push_back(parse(values[index], std::string(field) + "[" + std::to_string(index) + "]"));
  return result;
}

template <typename Enum>
Enum enumValue(std::string_view value, std::string_view field, std::initializer_list<std::pair<std::string_view, Enum>> choices)
{
  for (const auto& [name, result] : choices)
    if (value == name)
      return result;
  throw WireError(std::string(field), "unknown enum token");
}

csr::NodeKind nodeKind(std::string_view value, std::string_view field)
{
  return enumValue<csr::NodeKind>(value, field, {{"port", csr::NodeKind::kPort}, {"instance_pin", csr::NodeKind::kInstancePin}});
}

csr::ArcKind arcKind(std::string_view value, std::string_view field)
{
  return enumValue<csr::ArcKind>(value, field, {{"cell", csr::ArcKind::kCell}, {"net", csr::ArcKind::kNet}});
}

csr::TableVariable tableVariable(std::string_view value, std::string_view field)
{
  return enumValue<csr::TableVariable>(value, field, {{"none", csr::TableVariable::kNone},
                                                      {"input_transition", csr::TableVariable::kInputTransition},
                                                      {"output_capacitance", csr::TableVariable::kOutputCapacitance},
                                                      {"constrained_transition", csr::TableVariable::kConstrainedTransition}});
}

csr::TimingSense timingSense(std::string_view value, std::string_view field)
{
  return enumValue<csr::TimingSense>(value, field, {{"none_positive_fallback", csr::TimingSense::kNoneUsesPositiveFallback},
                                                    {"positive_unate", csr::TimingSense::kPositiveUnate},
                                                    {"negative_unate", csr::TimingSense::kNegativeUnate},
                                                    {"non_unate", csr::TimingSense::kNonUnate}});
}

csr::Transition transition(std::string_view value, std::string_view field)
{
  return enumValue<csr::Transition>(value, field,
                                    {{"rise", csr::Transition::kRise}, {"fall", csr::Transition::kFall}, {"none", csr::Transition::kNone}});
}

CreationToken creationToken(const Json& value, std::string_view field)
{
  exactObject(value, field, {"schema_version", "case_id", "profile_id", "seed", "batch_ordinal", "accepted_ordinal"});
  return CreationToken{u32(value.at("schema_version"), std::string(field) + ".schema_version"),
                       string(value.at("case_id"), std::string(field) + ".case_id"),
                       string(value.at("profile_id"), std::string(field) + ".profile_id"),
                       u64(value.at("seed"), std::string(field) + ".seed"),
                       u64(value.at("batch_ordinal"), std::string(field) + ".batch_ordinal"),
                       u64(value.at("accepted_ordinal"), std::string(field) + ".accepted_ordinal")};
}

csr::NodeKey nodeKey(const Json& value, std::string_view field)
{
  exactObject(value, field, {"kind", "full_name", "instance_name", "pin_name"});
  return csr::NodeKey{nodeKind(string(value.at("kind"), std::string(field) + ".kind"), std::string(field) + ".kind"),
                      string(value.at("full_name"), std::string(field) + ".full_name"),
                      string(value.at("instance_name"), std::string(field) + ".instance_name"),
                      string(value.at("pin_name"), std::string(field) + ".pin_name")};
}

csr::TimingTableSignature tableSignature(const Json& value, std::string_view field)
{
  exactObject(value, field, {"present", "variable1", "variable2"});
  return csr::TimingTableSignature{boolean(value.at("present"), std::string(field) + ".present"),
                                   tableVariable(string(value.at("variable1"), std::string(field) + ".variable1"), std::string(field) + ".variable1"),
                                   tableVariable(string(value.at("variable2"), std::string(field) + ".variable2"), std::string(field) + ".variable2")};
}

csr::LibraryArcSignature librarySignature(const Json& value, std::string_view field)
{
  exactObject(value, field, {"library_name", "arc_index", "sense", "trigger_transition", "check_transition", "sdf_condition", "delay_tables", "slew_tables"});
  auto delays = vectorOf<csr::TimingTableSignature>(value.at("delay_tables"), std::string(field) + ".delay_tables", tableSignature);
  auto slews = vectorOf<csr::TimingTableSignature>(value.at("slew_tables"), std::string(field) + ".slew_tables", tableSignature);
  if (delays.size() != csr::kTransitionCount || slews.size() != csr::kTransitionCount)
    throw WireError(std::string(field), "delay/slew table arrays must have exactly two entries");
  csr::LibraryArcSignature result;
  result.library_name = string(value.at("library_name"), std::string(field) + ".library_name");
  result.arc_index = i32(value.at("arc_index"), std::string(field) + ".arc_index");
  result.sense = timingSense(string(value.at("sense"), std::string(field) + ".sense"), std::string(field) + ".sense");
  result.trigger_transition = transition(string(value.at("trigger_transition"), std::string(field) + ".trigger_transition"), std::string(field) + ".trigger_transition");
  result.check_transition = transition(string(value.at("check_transition"), std::string(field) + ".check_transition"), std::string(field) + ".check_transition");
  result.sdf_condition = string(value.at("sdf_condition"), std::string(field) + ".sdf_condition");
  std::copy(delays.begin(), delays.end(), result.delay_tables.begin());
  std::copy(slews.begin(), slews.end(), result.slew_tables.begin());
  return result;
}

csr::ArcKey arcKey(const Json& value, std::string_view field)
{
  exactObject(value, field, {"source", "sink", "kind", "owner_name", "library_source_port", "library_sink_port", "library_arcs", "parallel_ordinal"});
  csr::ArcKey result;
  result.base.source = nodeKey(value.at("source"), std::string(field) + ".source");
  result.base.sink = nodeKey(value.at("sink"), std::string(field) + ".sink");
  result.base.kind = arcKind(string(value.at("kind"), std::string(field) + ".kind"), std::string(field) + ".kind");
  result.base.owner_name = string(value.at("owner_name"), std::string(field) + ".owner_name");
  result.base.library_source_port = string(value.at("library_source_port"), std::string(field) + ".library_source_port");
  result.base.library_sink_port = string(value.at("library_sink_port"), std::string(field) + ".library_sink_port");
  result.base.library_arcs = vectorOf<csr::LibraryArcSignature>(value.at("library_arcs"), std::string(field) + ".library_arcs", librarySignature);
  result.parallel_ordinal = u64(value.at("parallel_ordinal"), std::string(field) + ".parallel_ordinal");
  return result;
}

StableNodeRef nodeRef(const Json& value, std::string_view field)
{
  if (!value.is_object())
    throw WireError(std::string(field), "expected object");
  const std::string kind = string(value.at("kind"), std::string(field) + ".kind");
  if (kind == "existing") {
    exactObject(value, field, {"kind", "baseline_key"});
    return ExistingNodeRef{nodeKey(value.at("baseline_key"), std::string(field) + ".baseline_key")};
  }
  if (kind == "created") {
    exactObject(value, field, {"kind", "creation_token", "role"});
    auto role = enumValue<CreatedNodeRole>(string(value.at("role"), std::string(field) + ".role"), std::string(field) + ".role",
                          {{"input", CreatedNodeRole::kInput}, {"output", CreatedNodeRole::kOutput}});
    return CreatedNodeRef{creationToken(value.at("creation_token"), std::string(field) + ".creation_token"), role};
  }
  throw WireError(std::string(field) + ".kind", "unknown stable node reference tag");
}

StableNetRef netRef(const Json& value, std::string_view field)
{
  if (!value.is_object())
    throw WireError(std::string(field), "expected object");
  const std::string kind = string(value.at("kind"), std::string(field) + ".kind");
  if (kind == "existing") {
    exactObject(value, field, {"kind", "baseline_name"});
    return ExistingNetRef{string(value.at("baseline_name"), std::string(field) + ".baseline_name")};
  }
  if (kind == "created") {
    exactObject(value, field, {"kind", "creation_token", "role"});
    if (string(value.at("role"), std::string(field) + ".role") != "output_net")
      throw WireError(std::string(field) + ".role", "unknown created net role");
    return CreatedNetRef{creationToken(value.at("creation_token"), std::string(field) + ".creation_token"), CreatedNetRole::kOutputNet};
  }
  throw WireError(std::string(field) + ".kind", "unknown stable net reference tag");
}

LibraryCellRef libraryCell(const Json& value, std::string_view field)
{
  exactObject(value, field, {"library_name", "cell_name", "input_port", "output_port", "ordered_arc_signatures"});
  return LibraryCellRef{string(value.at("library_name"), std::string(field) + ".library_name"),
                        string(value.at("cell_name"), std::string(field) + ".cell_name"),
                        string(value.at("input_port"), std::string(field) + ".input_port"),
                        string(value.at("output_port"), std::string(field) + ".output_port"),
                        vectorOf<csr::LibraryArcSignature>(value.at("ordered_arc_signatures"), std::string(field) + ".ordered_arc_signatures", librarySignature)};
}

PlacementInputs placement(const Json& value, std::string_view field)
{
  if (!value.is_object())
    throw WireError(std::string(field), "expected object");
  const std::string kind = string(value.at("kind"), std::string(field) + ".kind");
  if (kind == "unplaced") {
    exactObject(value, field, {"kind"});
    return UnplacedInputs{};
  }
  if (kind == "placed") {
    exactObject(value, field, {"kind", "x_microns", "y_microns"});
    return PlacedInputs{number(value.at("x_microns"), std::string(field) + ".x_microns"),
                        number(value.at("y_microns"), std::string(field) + ".y_microns")};
  }
  throw WireError(std::string(field) + ".kind", "unknown placement tag");
}

RcInputs rcInputs(const Json& value, std::string_view field)
{
  if (!value.is_object())
    throw WireError(std::string(field), "expected object");
  const std::string kind = string(value.at("kind"), std::string(field) + ".kind");
  if (kind == "no_parasitic") {
    exactObject(value, field, {"kind"});
    return NoParasiticRcInputs{};
  }
  if (kind == "parasitic") {
    exactObject(value, field, {"kind", "policy"});
    auto policy = enumValue<ParasiticEditPolicy>(string(value.at("policy"), std::string(field) + ".policy"), std::string(field) + ".policy",
                            {{"error", ParasiticEditPolicy::kError}, {"full_with_record", ParasiticEditPolicy::kFullWithRecord},
                             {"pre_route_tree", ParasiticEditPolicy::kPreRouteTree}});
    return ParasiticRcInputs{policy};
  }
  throw WireError(std::string(field) + ".kind", "unknown RC input tag");
}

BufferInsertionMutation edit(const Json& value, std::string_view field)
{
  exactObject(value, field, {"original_net", "moved_sinks", "library_cell", "placement_inputs", "rc_inputs", "creation_token"});
  return BufferInsertionMutation{netRef(value.at("original_net"), std::string(field) + ".original_net"),
                                 vectorOf<StableNodeRef>(value.at("moved_sinks"), std::string(field) + ".moved_sinks", nodeRef),
                                 libraryCell(value.at("library_cell"), std::string(field) + ".library_cell"),
                                 placement(value.at("placement_inputs"), std::string(field) + ".placement_inputs"),
                                 rcInputs(value.at("rc_inputs"), std::string(field) + ".rc_inputs"),
                                 creationToken(value.at("creation_token"), std::string(field) + ".creation_token")};
}

ComponentFingerprints fingerprints(const Json& value, std::string_view field)
{
  exactObject(value, field, {"schema_version", "topology", "clock_constraint", "library", "corner", "parasitic", "timing_input"});
  return ComponentFingerprints{string(value.at("schema_version"), std::string(field) + ".schema_version"),
                               string(value.at("topology"), std::string(field) + ".topology"),
                               string(value.at("clock_constraint"), std::string(field) + ".clock_constraint"),
                               string(value.at("library"), std::string(field) + ".library"),
                               string(value.at("corner"), std::string(field) + ".corner"),
                               string(value.at("parasitic"), std::string(field) + ".parasitic"),
                               string(value.at("timing_input"), std::string(field) + ".timing_input")};
}

CanonicalPreStateEvidence preState(const Json& value, std::string_view field)
{
  exactObject(value, field, {"instances", "nets", "nodes", "arcs"});
  return CanonicalPreStateEvidence{vectorOf<std::string>(value.at("instances"), std::string(field) + ".instances", string),
                                   vectorOf<std::string>(value.at("nets"), std::string(field) + ".nets", string),
                                   vectorOf<csr::NodeKey>(value.at("nodes"), std::string(field) + ".nodes", nodeKey),
                                   vectorOf<csr::ArcKey>(value.at("arcs"), std::string(field) + ".arcs", arcKey)};
}

BaselineSemanticIdentity baseline(const Json& value, std::string_view field)
{
  exactObject(value, field, {"design_name", "fingerprints", "canonical_pre_state"});
  return BaselineSemanticIdentity{string(value.at("design_name"), std::string(field) + ".design_name"),
                                  fingerprints(value.at("fingerprints"), std::string(field) + ".fingerprints"),
                                  preState(value.at("canonical_pre_state"), std::string(field) + ".canonical_pre_state")};
}

CommittedEditIdentity committed(const Json& value, std::string_view field)
{
  exactObject(value, field, {"generation", "current_fingerprints", "canonical_pre_state"});
  return CommittedEditIdentity{TransactionGeneration{u64(value.at("generation"), std::string(field) + ".generation")},
                               fingerprints(value.at("current_fingerprints"), std::string(field) + ".current_fingerprints"),
                               preState(value.at("canonical_pre_state"), std::string(field) + ".canonical_pre_state")};
}

PolicyReceipt receipt(const Json& value, std::string_view field)
{
  exactObject(value, field, {"disposition", "reasons"});
  auto disposition = enumValue<EditDisposition>(string(value.at("disposition"), std::string(field) + ".disposition"), std::string(field) + ".disposition",
                               {{"supported", EditDisposition::kSupported}, {"full_with_record", EditDisposition::kFullWithRecord}});
  return PolicyReceipt{disposition, vectorOf<std::string>(value.at("reasons"), std::string(field) + ".reasons", string)};
}

TimingEditTraceBatch batch(const Json& value, std::string_view field)
{
  exactObject(value, field, {"ordinal", "expected_pre_identity", "edits", "policy_receipt", "expected_post_identity"});
  return TimingEditTraceBatch{u64(value.at("ordinal"), std::string(field) + ".ordinal"),
                              committed(value.at("expected_pre_identity"), std::string(field) + ".expected_pre_identity"),
                              vectorOf<BufferInsertionMutation>(value.at("edits"), std::string(field) + ".edits", edit),
                              receipt(value.at("policy_receipt"), std::string(field) + ".policy_receipt"),
                              committed(value.at("expected_post_identity"), std::string(field) + ".expected_post_identity")};
}

TimingEditTrace fromJson(const Json& value)
{
  exactObject(value, "trace", {"schema_version", "workload_kind", "seed", "profile", "baseline", "source_artifacts", "batches", "replay_command"});
  TimingEditTrace result;
  result.schema_version = string(value.at("schema_version"), "trace.schema_version");
  result.workload_kind = enumValue<TraceWorkloadKind>(string(value.at("workload_kind"), "trace.workload_kind"), "trace.workload_kind",
                                   {{"synthetic", TraceWorkloadKind::kSynthetic}, {"real_design", TraceWorkloadKind::kRealDesign}});
  result.seed = u64(value.at("seed"), "trace.seed");
  result.profile = string(value.at("profile"), "trace.profile");
  result.baseline = baseline(value.at("baseline"), "trace.baseline");
  result.source_artifacts = vectorOf<SourceArtifactIdentity>(value.at("source_artifacts"), "trace.source_artifacts", [](const Json& item, std::string_view field) {
    exactObject(item, field, {"artifact_role", "logical_name", "content_sha256"});
    return SourceArtifactIdentity{string(item.at("artifact_role"), std::string(field) + ".artifact_role"),
                                  string(item.at("logical_name"), std::string(field) + ".logical_name"),
                                  string(item.at("content_sha256"), std::string(field) + ".content_sha256")};
  });
  result.batches = vectorOf<TimingEditTraceBatch>(value.at("batches"), "trace.batches", batch);
  const auto& replay = value.at("replay_command");
  exactObject(replay, "trace.replay_command", {"executable", "arguments"});
  result.replay_command = ReplayCommand{string(replay.at("executable"), "trace.replay_command.executable"),
                                        vectorOf<std::string>(replay.at("arguments"), "trace.replay_command.arguments", string)};
  return result;
}

Json toJson(const CreationToken& value)
{
  return {{"schema_version", value.schema_version}, {"case_id", value.case_id}, {"profile_id", value.profile_id}, {"seed", value.seed},
          {"batch_ordinal", value.batch_ordinal}, {"accepted_ordinal", value.accepted_ordinal}};
}

Json toJson(const csr::NodeKey& value)
{
  return {{"kind", csr::token(value.kind)}, {"full_name", value.full_name}, {"instance_name", value.instance_name}, {"pin_name", value.pin_name}};
}

Json toJson(const csr::TimingTableSignature& value)
{
  return {{"present", value.present}, {"variable1", csr::token(value.variable1)}, {"variable2", csr::token(value.variable2)}};
}

Json toJson(const csr::LibraryArcSignature& value)
{
  Json delays = Json::array();
  Json slews = Json::array();
  for (const auto& table : value.delay_tables) delays.push_back(toJson(table));
  for (const auto& table : value.slew_tables) slews.push_back(toJson(table));
  return {{"library_name", value.library_name}, {"arc_index", value.arc_index}, {"sense", csr::token(value.sense)},
          {"trigger_transition", csr::token(value.trigger_transition)}, {"check_transition", csr::token(value.check_transition)},
          {"sdf_condition", value.sdf_condition}, {"delay_tables", std::move(delays)}, {"slew_tables", std::move(slews)}};
}

Json toJson(const csr::ArcKey& value)
{
  Json arcs = Json::array();
  for (const auto& signature : value.base.library_arcs) arcs.push_back(toJson(signature));
  return {{"source", toJson(value.base.source)}, {"sink", toJson(value.base.sink)}, {"kind", csr::token(value.base.kind)},
          {"owner_name", value.base.owner_name}, {"library_source_port", value.base.library_source_port},
          {"library_sink_port", value.base.library_sink_port}, {"library_arcs", std::move(arcs)}, {"parallel_ordinal", value.parallel_ordinal}};
}

Json toJson(const StableNodeRef& value)
{
  return std::visit([](const auto& ref) -> Json {
    using Ref = std::decay_t<decltype(ref)>;
    if constexpr (std::is_same_v<Ref, ExistingNodeRef>)
      return {{"kind", "existing"}, {"baseline_key", toJson(ref.baseline_key)}};
    else
      return {{"kind", "created"}, {"creation_token", toJson(ref.creation_token)}, {"role", token(ref.role)}};
  }, value);
}

Json toJson(const StableNetRef& value)
{
  return std::visit([](const auto& ref) -> Json {
    using Ref = std::decay_t<decltype(ref)>;
    if constexpr (std::is_same_v<Ref, ExistingNetRef>)
      return {{"kind", "existing"}, {"baseline_name", ref.baseline_name}};
    else
      return {{"kind", "created"}, {"creation_token", toJson(ref.creation_token)}, {"role", token(ref.role)}};
  }, value);
}

Json toJson(const LibraryCellRef& value)
{
  Json signatures = Json::array();
  for (const auto& signature : value.ordered_arc_signatures) signatures.push_back(toJson(signature));
  return {{"library_name", value.library_name}, {"cell_name", value.cell_name}, {"input_port", value.input_port},
          {"output_port", value.output_port}, {"ordered_arc_signatures", std::move(signatures)}};
}

Json toJson(const PlacementInputs& value)
{
  return std::visit([](const auto& placement_value) -> Json {
    using Placement = std::decay_t<decltype(placement_value)>;
    if constexpr (std::is_same_v<Placement, UnplacedInputs>)
      return {{"kind", "unplaced"}};
    else
      return {{"kind", "placed"}, {"x_microns", placement_value.x_microns}, {"y_microns", placement_value.y_microns}};
  }, value);
}

Json toJson(const RcInputs& value)
{
  return std::visit([](const auto& rc) -> Json {
    using Rc = std::decay_t<decltype(rc)>;
    if constexpr (std::is_same_v<Rc, NoParasiticRcInputs>)
      return {{"kind", "no_parasitic"}};
    else
      return {{"kind", "parasitic"}, {"policy", token(rc.parasitic_policy)}};
  }, value);
}

Json toJson(const BufferInsertionMutation& value)
{
  Json sinks = Json::array();
  for (const auto& sink : value.moved_sinks) sinks.push_back(toJson(sink));
  return {{"original_net", toJson(value.original_net)}, {"moved_sinks", std::move(sinks)}, {"library_cell", toJson(value.library_cell)},
          {"placement_inputs", toJson(value.placement_inputs)}, {"rc_inputs", toJson(value.rc_inputs)}, {"creation_token", toJson(value.creation_token)}};
}

Json toJson(const ComponentFingerprints& value)
{
  return {{"schema_version", value.schema_version}, {"topology", value.topology}, {"clock_constraint", value.clock_constraint},
          {"library", value.library}, {"corner", value.corner}, {"parasitic", value.parasitic}, {"timing_input", value.timing_input}};
}

Json toJson(const CanonicalPreStateEvidence& value)
{
  Json nodes = Json::array();
  Json arcs = Json::array();
  for (const auto& node : value.nodes) nodes.push_back(toJson(node));
  for (const auto& arc : value.arcs) arcs.push_back(toJson(arc));
  return {{"instances", value.instances}, {"nets", value.nets}, {"nodes", std::move(nodes)}, {"arcs", std::move(arcs)}};
}

Json toJson(const BaselineSemanticIdentity& value)
{
  return {{"design_name", value.design_name}, {"fingerprints", toJson(value.fingerprints)}, {"canonical_pre_state", toJson(value.canonical_pre_state)}};
}

Json toJson(const CommittedEditIdentity& value)
{
  return {{"generation", value.generation.value()}, {"current_fingerprints", toJson(value.current_fingerprints)},
          {"canonical_pre_state", toJson(value.canonical_pre_state)}};
}

Json toJson(const PolicyReceipt& value)
{
  return {{"disposition", token(value.disposition)}, {"reasons", value.reasons}};
}

Json toJson(const TimingEditTraceBatch& value)
{
  Json edits = Json::array();
  for (const auto& edit_value : value.edits) edits.push_back(toJson(edit_value));
  return {{"ordinal", value.ordinal}, {"expected_pre_identity", toJson(value.expected_pre_identity)}, {"edits", std::move(edits)},
          {"policy_receipt", toJson(value.policy_receipt)}, {"expected_post_identity", toJson(value.expected_post_identity)}};
}

Json toJson(const TimingEditTrace& value)
{
  Json artifacts = Json::array();
  Json batches = Json::array();
  for (const auto& artifact : value.source_artifacts)
    artifacts.push_back({{"artifact_role", artifact.artifact_role}, {"logical_name", artifact.logical_name}, {"content_sha256", artifact.content_sha256}});
  for (const auto& batch_value : value.batches) batches.push_back(toJson(batch_value));
  return {{"schema_version", value.schema_version}, {"workload_kind", token(value.workload_kind)}, {"seed", value.seed}, {"profile", value.profile},
          {"baseline", toJson(value.baseline)}, {"source_artifacts", std::move(artifacts)}, {"batches", std::move(batches)},
          {"replay_command", {{"executable", value.replay_command.executable}, {"arguments", value.replay_command.arguments}}}};
}

}  // namespace json_detail

TimingEditTraceParseResult parseTimingEditTraceJson(std::string_view json_text)
{
  try {
    TimingEditTrace result = json_detail::fromJson(json_detail::parseClosedWorldJson(json_text));
    if (auto errors = validateTimingEditTrace(result); !errors.empty())
      return errors;
    return result;
  } catch (const json_detail::WireError& error) {
    return TimingEditContractViolations{{TimingEditContractCode::kInvalidValue, error.field, error.what()}};
  } catch (const nlohmann::json::exception& error) {
    return TimingEditContractViolations{{TimingEditContractCode::kInvalidValue, "trace", error.what()}};
  } catch (const std::exception& error) {
    return TimingEditContractViolations{{TimingEditContractCode::kInvalidValue, "trace", error.what()}};
  }
}

std::string timingEditTraceToJson(const TimingEditTrace& value)
{
  if (auto errors = validateTimingEditTrace(value); !errors.empty())
    throw std::invalid_argument(errors.front().field + ": " + errors.front().message);
  return json_detail::toJson(value).dump();
}

}  // namespace ista::timing_edit
