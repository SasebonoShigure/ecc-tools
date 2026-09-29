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

#include "CsrRealDifferentialWire.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "TimingSemanticKey.hpp"
#include "StrictJson.hpp"
namespace ista::csr::real_diff {
namespace {
[[noreturn]] void fail(std::string_view label, std::string_view message)
{
  throw std::runtime_error(std::string(label) + ": " + std::string(message));
}

std::string requireString(const Json& value, std::string_view label)
{
  if (!value.is_string()) {
    fail(label, "expected a string");
  }
  return value.get<std::string>();
}

std::int32_t requireI32(const Json& value, std::string_view label)
{
  if (!value.is_number_integer() || value.is_boolean()) {
    fail(label, "expected an I32 integer");
  }
  const std::int64_t result = value.get<std::int64_t>();
  if (result < std::numeric_limits<std::int32_t>::min() || result > std::numeric_limits<std::int32_t>::max()) {
    fail(label, "integer is outside I32");
  }
  return static_cast<std::int32_t>(result);
}

std::uint64_t requireU64(const Json& value, std::string_view label)
{
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (!value.is_number_integer() || value.is_boolean()) {
    fail(label, "expected a U64 integer");
  }
  const std::int64_t result = value.get<std::int64_t>();
  if (result < 0) {
    fail(label, "U64 cannot be negative");
  }
  return static_cast<std::uint64_t>(result);
}

constexpr std::array kNodeKinds{NodeKind::kPort, NodeKind::kInstancePin};
constexpr std::array kArcKinds{ArcKind::kCell, ArcKind::kNet};
constexpr std::array kAnalyses{AnalysisMode::kMax, AnalysisMode::kMin};
constexpr std::array kPathSources{PathSource::kInput, PathSource::kRegister};
constexpr std::array kTransitions{Transition::kRise, Transition::kFall, Transition::kNone};
constexpr std::array kTimingSenses{TimingSense::kNoneUsesPositiveFallback, TimingSense::kPositiveUnate, TimingSense::kNegativeUnate, TimingSense::kNonUnate};
constexpr std::array kTableVariables{TableVariable::kNone, TableVariable::kInputTransition, TableVariable::kOutputCapacitance,
                                     TableVariable::kConstrainedTransition};

template <typename Enum, std::size_t Size>
Enum enumFromToken(std::string_view value, std::string_view label, const std::array<Enum, Size>& choices)
{
  for (Enum result : choices) {
    if (token(result) == value) {
      return result;
    }
  }
  fail(label, "unknown enum token");
}

Transition transitionFromToken(std::string_view value, std::string_view label)
{
  return enumFromToken(value, label, kTransitions);
}

template <typename Range, typename Encode>
Json jsonArray(const Range& values, Encode encode)
{
  Json result = Json::array();
  for (const auto& value : values) {
    result.push_back(encode(value));
  }
  return result;
}

template <typename T, typename Decode>
std::vector<T> vectorFromJson(const Json& value, std::string_view label, Decode decode)
{
  if (!value.is_array()) {
    fail(label, "expected an array");
  }
  std::vector<T> result;
  for (const Json& item : value) {
    result.push_back(decode(item, label));
  }
  return result;
}

Json tableToJson(const TimingTableSignature& table)
{
  return {{"present", table.present}, {"variable1", token(table.variable1)}, {"variable2", token(table.variable2)}};
}

TimingTableSignature tableFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(value, {"present", "variable1", "variable2"}, label);
  if (!value.at("present").is_boolean()) {
    fail(label, "table present flag is not boolean");
  }
  return {value.at("present").get<bool>(), enumFromToken(requireString(value.at("variable1"), label), label, kTableVariables),
          enumFromToken(requireString(value.at("variable2"), label), label, kTableVariables)};
}

Json signatureToJson(const LibraryArcSignature& signature)
{
  return {{"library_name", signature.library_name},
          {"arc_index", signature.arc_index},
          {"sense", token(signature.sense)},
          {"trigger_transition", token(signature.trigger_transition)},
          {"check_transition", token(signature.check_transition)},
          {"sdf_condition", signature.sdf_condition},
          {"delay_tables", jsonArray(signature.delay_tables, tableToJson)},
          {"slew_tables", jsonArray(signature.slew_tables, tableToJson)}};
}

LibraryArcSignature signatureFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(value, {"library_name", "arc_index", "sense", "trigger_transition", "check_transition", "sdf_condition", "delay_tables", "slew_tables"},
                   label);
  if (!value.at("delay_tables").is_array() || !value.at("slew_tables").is_array() || value.at("delay_tables").size() != kTransitionCount
      || value.at("slew_tables").size() != kTransitionCount) {
    fail(label, "library signature tables have the wrong shape");
  }
  LibraryArcSignature signature;
  signature.library_name = requireString(value.at("library_name"), label);
  signature.arc_index = requireI32(value.at("arc_index"), label);
  signature.sense = enumFromToken(requireString(value.at("sense"), label), label, kTimingSenses);
  signature.trigger_transition = transitionFromToken(requireString(value.at("trigger_transition"), label), label);
  signature.check_transition = transitionFromToken(requireString(value.at("check_transition"), label), label);
  signature.sdf_condition = requireString(value.at("sdf_condition"), label);
  for (std::size_t index = 0; index < kTransitionCount; ++index) {
    signature.delay_tables[index] = tableFromJson(value.at("delay_tables").at(index), label);
    signature.slew_tables[index] = tableFromJson(value.at("slew_tables").at(index), label);
  }
  return signature;
}

template <typename T, typename Encode>
Json optionalToJson(const std::optional<T>& value, Encode encode)
{
  return value ? encode(*value) : Json(nullptr);
}

template <typename T, typename Decode>
std::optional<T> optionalFromJson(const Json& value, std::string_view label, Decode decode)
{
  if (value.is_null()) {
    return std::nullopt;
  }
  return decode(value, label);
}

template <typename T, typename Encode>
Json laneMatrixToJson(const NeutralLaneMatrix<T>& matrix, Encode encode)
{
  return jsonArray(matrix, [&](const auto& analysis) { return jsonArray(analysis, [&](const auto& value) { return optionalToJson(value, encode); }); });
}

template <typename T, typename Decode>
NeutralLaneMatrix<T> laneMatrixFromJson(const Json& value, std::string_view label, Decode decode)
{
  if (!value.is_array() || value.size() != kAnalysisCount) {
    fail(label, "lane matrix has the wrong analysis extent");
  }
  NeutralLaneMatrix<T> result;
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    const Json& row = value.at(analysis);
    if (!row.is_array() || row.size() != kTransitionCount) {
      fail(label, "lane matrix has the wrong transition extent");
    }
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      result[analysis][transition] = optionalFromJson<T>(row.at(transition), label, decode);
    }
  }
  return result;
}

template <typename T, typename Encode>
Json arcLaneMatrixToJson(const NeutralArcLaneMatrix<T>& matrix, Encode encode)
{
  return jsonArray(matrix, [&](const auto& analysis) {
    return jsonArray(analysis, [&](const auto& input) { return jsonArray(input, [&](const auto& value) { return optionalToJson(value, encode); }); });
  });
}

template <typename T, typename Decode>
NeutralArcLaneMatrix<T> arcLaneMatrixFromJson(const Json& value, std::string_view label, Decode decode)
{
  if (!value.is_array() || value.size() != kAnalysisCount) {
    fail(label, "arc lane matrix has the wrong analysis extent");
  }
  NeutralArcLaneMatrix<T> result;
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    const Json& inputs = value.at(analysis);
    if (!inputs.is_array() || inputs.size() != kTransitionCount) {
      fail(label, "arc lane matrix has the wrong input extent");
    }
    for (std::size_t input = 0; input < kTransitionCount; ++input) {
      const Json& outputs = inputs.at(input);
      if (!outputs.is_array() || outputs.size() != kTransitionCount) {
        fail(label, "arc lane matrix has the wrong output extent");
      }
      for (std::size_t output = 0; output < kTransitionCount; ++output) {
        result[analysis][input][output] = optionalFromJson<T>(outputs.at(output), label, decode);
      }
    }
  }
  return result;
}

Json clockToJson(const NeutralClockState& state)
{
  auto number = [](double value) { return numberToJson(value); };
  auto node = [](const NodeKey& value) { return nodeKeyToJson(value); };
  auto transition = [](Transition value) { return Json(token(value)); };
  return {{"arrivals", laneMatrixToJson(state.arrivals, number)},
          {"slews", laneMatrixToJson(state.slews, number)},
          {"predecessor_nodes", laneMatrixToJson(state.predecessor_nodes, node)},
          {"predecessor_arc_delays", laneMatrixToJson(state.predecessor_arc_delays, number)},
          {"predecessor_transitions", laneMatrixToJson(state.predecessor_transitions, transition)}};
}

NeutralClockState clockFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(value, {"arrivals", "slews", "predecessor_nodes", "predecessor_arc_delays", "predecessor_transitions"}, label);
  auto number = [](const Json& item, std::string_view where) { return numberFromJson(item, where); };
  auto node = [](const Json& item, std::string_view where) { return nodeKeyFromJson(item, where); };
  auto transition = [](const Json& item, std::string_view where) { return transitionFromToken(requireString(item, where), where); };
  NeutralClockState state;
  state.arrivals = laneMatrixFromJson<double>(value.at("arrivals"), label, number);
  state.slews = laneMatrixFromJson<double>(value.at("slews"), label, number);
  state.predecessor_nodes = laneMatrixFromJson<NodeKey>(value.at("predecessor_nodes"), label, node);
  state.predecessor_arc_delays = laneMatrixFromJson<double>(value.at("predecessor_arc_delays"), label, number);
  state.predecessor_transitions = laneMatrixFromJson<Transition>(value.at("predecessor_transitions"), label, transition);
  return state;
}

Json pathToJson(const NeutralPath& path)
{
  return {{"analysis", token(path.analysis)},
          {"path_source", token(path.source)},
          {"transition", token(path.lane_transition)},
          {"clock_tag", path.clock_tag},
          {"arrival", numberToJson(path.arrival)},
          {"slew", numberToJson(path.slew)},
          {"launch", numberToJson(path.launch)},
          {"start_node", nodeKeyToJson(path.start_node)},
          {"predecessor_node", optionalToJson(path.predecessor_node, nodeKeyToJson)},
          {"predecessor_arc", optionalToJson(path.predecessor_arc, arcKeyToJson)},
          {"predecessor_arc_delay", numberToJson(path.predecessor_arc_delay)},
          {"clock_name", path.clock_name},
          {"state_transition", token(path.state_transition)},
          {"predecessor_transition", token(path.predecessor_transition)},
          {"crpr_node", optionalToJson(path.crpr_node, nodeKeyToJson)},
          {"crpr_transition", token(path.crpr_transition)}};
}

NeutralPath pathFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(value,
                   {"analysis", "path_source", "transition", "clock_tag", "arrival", "slew", "launch", "start_node", "predecessor_node", "predecessor_arc",
                    "predecessor_arc_delay", "clock_name", "state_transition", "predecessor_transition", "crpr_node", "crpr_transition"},
                   label);
  NeutralPath path;
  path.analysis = enumFromToken(requireString(value.at("analysis"), label), label, kAnalyses);
  path.source = enumFromToken(requireString(value.at("path_source"), label), label, kPathSources);
  path.lane_transition = transitionFromToken(requireString(value.at("transition"), label), label);
  path.clock_tag = requireString(value.at("clock_tag"), label);
  path.arrival = numberFromJson(value.at("arrival"), label);
  path.slew = numberFromJson(value.at("slew"), label);
  path.launch = numberFromJson(value.at("launch"), label);
  path.start_node = nodeKeyFromJson(value.at("start_node"), label);
  path.predecessor_node = optionalFromJson<NodeKey>(value.at("predecessor_node"), label, nodeKeyFromJson);
  path.predecessor_arc = optionalFromJson<ArcKey>(value.at("predecessor_arc"), label, arcKeyFromJson);
  path.predecessor_arc_delay = numberFromJson(value.at("predecessor_arc_delay"), label);
  path.clock_name = requireString(value.at("clock_name"), label);
  path.state_transition = transitionFromToken(requireString(value.at("state_transition"), label), label);
  path.predecessor_transition = transitionFromToken(requireString(value.at("predecessor_transition"), label), label);
  path.crpr_node = optionalFromJson<NodeKey>(value.at("crpr_node"), label, nodeKeyFromJson);
  path.crpr_transition = transitionFromToken(requireString(value.at("crpr_transition"), label), label);
  if (path.predecessor_node.has_value() != path.predecessor_arc.has_value()) {
    fail(label, "path predecessor presence differs");
  }
  return path;
}

Json nodeToJson(const NeutralNode& node)
{
  return {{"key", nodeKeyToJson(node.key)},
          {"level", node.level},
          {"is_clock_point", node.is_clock_point},
          {"arrival", numberToJson(node.arrival)},
          {"required", numberToJson(node.required)},
          {"slack", numberToJson(node.slack)},
          {"launch", numberToJson(node.launch)},
          {"clock_name", optionalToJson(node.clock_name, [](const std::string& value) { return Json(value); })},
          {"predecessor_node", optionalToJson(node.predecessor_node, nodeKeyToJson)},
          {"predecessor_arc", optionalToJson(node.predecessor_arc, arcKeyToJson)},
          {"effective_clock", clockToJson(node.effective_clock)},
          {"physical_clock", clockToJson(node.physical_clock)},
          {"data_slews", laneMatrixToJson(node.data_slews, numberToJson)},
          {"paths", jsonArray(node.paths, pathToJson)}};
}

NeutralNode nodeFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(value,
                   {"key", "level", "is_clock_point", "arrival", "required", "slack", "launch", "clock_name", "predecessor_node", "predecessor_arc",
                    "effective_clock", "physical_clock", "data_slews", "paths"},
                   label);
  if (!value.at("is_clock_point").is_boolean() || !value.at("paths").is_array()) {
    fail(label, "node bool or paths shape is invalid");
  }
  NeutralNode node;
  node.key = nodeKeyFromJson(value.at("key"), label);
  node.level = requireI32(value.at("level"), label);
  node.is_clock_point = value.at("is_clock_point").get<bool>();
  node.arrival = numberFromJson(value.at("arrival"), label);
  node.required = numberFromJson(value.at("required"), label);
  node.slack = numberFromJson(value.at("slack"), label);
  node.launch = numberFromJson(value.at("launch"), label);
  node.clock_name
      = optionalFromJson<std::string>(value.at("clock_name"), label, [](const Json& item, std::string_view where) { return requireString(item, where); });
  node.predecessor_node = optionalFromJson<NodeKey>(value.at("predecessor_node"), label, nodeKeyFromJson);
  node.predecessor_arc = optionalFromJson<ArcKey>(value.at("predecessor_arc"), label, arcKeyFromJson);
  if (node.predecessor_node.has_value() != node.predecessor_arc.has_value()) {
    fail(label, "node predecessor presence differs");
  }
  node.effective_clock = clockFromJson(value.at("effective_clock"), label);
  node.physical_clock = clockFromJson(value.at("physical_clock"), label);
  node.data_slews
      = laneMatrixFromJson<double>(value.at("data_slews"), label, [](const Json& item, std::string_view where) { return numberFromJson(item, where); });
  for (const Json& item : value.at("paths")) {
    node.paths.push_back(pathFromJson(item, label));
  }
  return node;
}

Json arcDelayToJson(const NeutralArcDelay& value)
{
  return {{"delay", numberToJson(value.delay)}, {"is_fallback", value.is_fallback}};
}

NeutralArcDelay arcDelayFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(value, {"delay", "is_fallback"}, label);
  if (!value.at("is_fallback").is_boolean()) {
    fail(label, "timing-arc fallback is not boolean");
  }
  return {numberFromJson(value.at("delay"), label), value.at("is_fallback").get<bool>()};
}

Json arcToJson(const NeutralArc& arc)
{
  auto transition = [](const auto& value) { return optionalToJson(value, [](Transition item) { return Json(token(item)); }); };
  auto entry = [](const NeutralArcEntry& value) {
    return Json{{"timing_arc_index", value.timing_arc_index}, {"lanes", arcLaneMatrixToJson(value.lanes, arcDelayToJson)}};
  };
  return {{"key", arcKeyToJson(arc.key)},
          {"delay", numberToJson(arc.delay)},
          {"min_delay", numberToJson(arc.min_delay)},
          {"max_delay", numberToJson(arc.max_delay)},
          {"transition_delays", laneMatrixToJson(arc.transition_delays, numberToJson)},
          {"input_output_delays", arcLaneMatrixToJson(arc.input_output_delays, numberToJson)},
          {"graph_delays", arcLaneMatrixToJson(arc.graph_delays, numberToJson)},
          {"transition_types", jsonArray(arc.transition_types, transition)},
          {"timing_arcs", jsonArray(arc.timing_arcs, entry)}};
}

NeutralArc arcFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(
      value, {"key", "delay", "min_delay", "max_delay", "transition_delays", "input_output_delays", "graph_delays", "transition_types", "timing_arcs"}, label);
  if (!value.at("transition_types").is_array() || value.at("transition_types").size() != kTransitionCount || !value.at("timing_arcs").is_array()) {
    fail(label, "Arc transition or timing-arc shape is invalid");
  }
  NeutralArc arc;
  arc.key = arcKeyFromJson(value.at("key"), label);
  arc.delay = numberFromJson(value.at("delay"), label);
  arc.min_delay = numberFromJson(value.at("min_delay"), label);
  arc.max_delay = numberFromJson(value.at("max_delay"), label);
  auto number = [](const Json& item, std::string_view where) { return numberFromJson(item, where); };
  arc.transition_delays = laneMatrixFromJson<double>(value.at("transition_delays"), label, number);
  arc.input_output_delays = arcLaneMatrixFromJson<double>(value.at("input_output_delays"), label, number);
  arc.graph_delays = arcLaneMatrixFromJson<double>(value.at("graph_delays"), label, number);
  for (std::size_t index = 0; index < kTransitionCount; ++index) {
    arc.transition_types[index] = optionalFromJson<Transition>(value.at("transition_types").at(index), label, [](const Json& item, std::string_view where) {
      return transitionFromToken(requireString(item, where), where);
    });
  }
  for (const Json& item : value.at("timing_arcs")) {
    requireExactKeys(item, {"timing_arc_index", "lanes"}, label);
    arc.timing_arcs.push_back(
        {requireI32(item.at("timing_arc_index"), label), arcLaneMatrixFromJson<NeutralArcDelay>(item.at("lanes"), label, arcDelayFromJson)});
  }
  return arc;
}

Json rowToJson(const NeutralAdjacencyRow& row)
{
  return {{"node", nodeKeyToJson(row.node)}, {"arcs", jsonArray(row.arcs, arcKeyToJson)}};
}

NeutralAdjacencyRow rowFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(value, {"node", "arcs"}, label);
  if (!value.at("arcs").is_array()) {
    fail(label, "adjacency arcs are not an array");
  }
  NeutralAdjacencyRow row;
  row.node = nodeKeyFromJson(value.at("node"), label);
  for (const Json& item : value.at("arcs")) {
    row.arcs.push_back(arcKeyFromJson(item, label));
  }
  return row;
}

bool pathLess(const NeutralPath& lhs, const NeutralPath& rhs)
{
  return std::tuple{token(lhs.analysis), token(lhs.source), token(lhs.lane_transition), lhs.clock_tag}
         < std::tuple{token(rhs.analysis), token(rhs.source), token(rhs.lane_transition), rhs.clock_tag};
}

template <typename Item, typename Key>
std::size_t findKey(const std::vector<Item>& items, const Key& key)
{
  const auto found = std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.key == key; });
  if (found == items.end()) {
    fail("neutral wire", "semantic key does not resolve");
  }
  return static_cast<std::size_t>(std::distance(items.begin(), found));
}

void validateWire(const NeutralWire& wire)
{
  for (std::size_t index = 1; index < wire.nodes.size(); ++index) {
    if (!nodeKeyLess(wire.nodes[index - 1].key, wire.nodes[index].key)) {
      fail("neutral wire", "nodes are not strict canonical-key order");
    }
  }
  for (std::size_t index = 1; index < wire.arcs.size(); ++index) {
    if (!arcKeyLess(wire.arcs[index - 1].key, wire.arcs[index].key)) {
      fail("neutral wire", "arcs are not strict canonical-key order");
    }
  }
  if (wire.out_rows.size() != wire.nodes.size() || wire.in_rows.size() != wire.nodes.size() || wire.timing_order.size() != wire.nodes.size()) {
    fail("neutral wire", "row or timing-order cardinality differs from nodes");
  }

  std::set<std::size_t> order_nodes;
  for (const NodeKey& key : wire.timing_order) {
    if (!order_nodes.insert(findKey(wire.nodes, key)).second) {
      fail("neutral wire", "timing order contains a duplicate node");
    }
  }

  std::vector<std::size_t> outgoing_count(wire.arcs.size());
  std::vector<std::size_t> incoming_count(wire.arcs.size());
  for (std::size_t node = 0; node < wire.nodes.size(); ++node) {
    if (wire.out_rows[node].node != wire.nodes[node].key || wire.in_rows[node].node != wire.nodes[node].key) {
      fail("neutral wire", "row nodes do not follow canonical node order");
    }
    for (const ArcKey& key : wire.out_rows[node].arcs) {
      const std::size_t arc = findKey(wire.arcs, key);
      if (wire.arcs[arc].key.base.source != wire.nodes[node].key || ++outgoing_count[arc] != 1) {
        fail("neutral wire", "outgoing row does not preserve one source-owned Arc");
      }
    }
    for (const ArcKey& key : wire.in_rows[node].arcs) {
      const std::size_t arc = findKey(wire.arcs, key);
      if (wire.arcs[arc].key.base.sink != wire.nodes[node].key || ++incoming_count[arc] != 1) {
        fail("neutral wire", "incoming row does not preserve one sink-owned Arc");
      }
    }
    const auto& paths = wire.nodes[node].paths;
    for (std::size_t index = 1; index < paths.size(); ++index) {
      if (!pathLess(paths[index - 1], paths[index])) {
        fail("neutral wire", "node paths are not strict canonical lane/tag order");
      }
    }
  }
  if (std::any_of(outgoing_count.begin(), outgoing_count.end(), [](std::size_t count) { return count != 1; })
      || std::any_of(incoming_count.begin(), incoming_count.end(), [](std::size_t count) { return count != 1; })) {
    fail("neutral wire", "an Arc is absent from an adjacency direction");
  }
  for (const NeutralArc& arc : wire.arcs) {
    for (std::size_t index = 1; index < arc.timing_arcs.size(); ++index) {
      if (arc.timing_arcs[index - 1].timing_arc_index >= arc.timing_arcs[index].timing_arc_index) {
        fail("neutral wire", "timing Arc indices are not strict ascending");
      }
    }
  }
}

}  // namespace

Json parseStrictJson(std::string_view bytes, std::string_view label)
{
  try {
    return ista::json_detail::parseUniqueKeyJson(bytes);
  } catch (const ista::json_detail::DuplicateJsonKey& error) {
    fail(label, error.what());
  } catch (const nlohmann::json::exception& error) {
    fail(label, std::string("invalid JSON: ") + error.what());
  }
}

void requireExactKeys(const Json& value, std::initializer_list<std::string_view> keys, std::string_view label)
{
  if (!value.is_object() || value.size() != keys.size()) {
    fail(label, "object has extra or missing keys");
  }
  for (std::string_view key : keys) {
    if (!value.contains(std::string(key))) {
      fail(label, "object has extra or missing keys");
    }
  }
}

Json numberToJson(double value)
{
  if (std::isnan(value)) {
    fail("neutral number", "NaN is forbidden");
  }
  if (std::isinf(value)) {
    return {{"kind", std::signbit(value) ? "neg_inf" : "pos_inf"}};
  }
  const bool negative_zero = value == 0.0 && std::signbit(value);
  return {{"kind", "finite"}, {"value", value == 0.0 ? 0.0 : value}, {"negative_zero", negative_zero}};
}

double numberFromJson(const Json& value, std::string_view label)
{
  if (!value.is_object() || !value.contains("kind") || !value.at("kind").is_string()) {
    fail(label, "number is not a typed object");
  }
  const std::string kind = value.at("kind").get<std::string>();
  if (kind == "pos_inf" || kind == "neg_inf") {
    requireExactKeys(value, {"kind"}, label);
    return kind == "pos_inf" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
  }
  if (kind != "finite") {
    fail(label, "unknown number kind");
  }
  requireExactKeys(value, {"kind", "value", "negative_zero"}, label);
  if (!value.at("value").is_number() || value.at("value").is_boolean() || !value.at("negative_zero").is_boolean()) {
    fail(label, "finite number payload has the wrong type");
  }
  const double result = value.at("value").get<double>();
  const bool negative_zero = value.at("negative_zero").get<bool>();
  if (!std::isfinite(result) || (result == 0.0 && std::signbit(result)) || (negative_zero && result != 0.0)) {
    fail(label, "finite number payload is nonfinite or has invalid signed zero");
  }
  return negative_zero ? std::copysign(0.0, -1.0) : result;
}

Json nodeKeyToJson(const NodeKey& key)
{
  if (const auto error = validateNodeKey(key)) {
    fail("node key", *error);
  }
  return {{"kind", token(key.kind)}, {"full_name", key.full_name}, {"instance_name", key.instance_name}, {"pin_name", key.pin_name}};
}

NodeKey nodeKeyFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(value, {"kind", "full_name", "instance_name", "pin_name"}, label);
  NodeKey key{enumFromToken(requireString(value.at("kind"), label), label, kNodeKinds), requireString(value.at("full_name"), label),
              requireString(value.at("instance_name"), label), requireString(value.at("pin_name"), label)};
  if (const auto error = validateNodeKey(key)) {
    fail(label, *error);
  }
  return key;
}

Json arcKeyToJson(const ArcKey& key)
{
  if (const auto error = validateArcKeyBase(key.base)) {
    fail("Arc key", *error);
  }
  return {{"source", nodeKeyToJson(key.base.source)},
          {"sink", nodeKeyToJson(key.base.sink)},
          {"kind", token(key.base.kind)},
          {"owner_name", key.base.owner_name},
          {"library_source_port", key.base.library_source_port},
          {"library_sink_port", key.base.library_sink_port},
          {"ordered_library_arc_semantic_signature", jsonArray(key.base.library_arcs, signatureToJson)},
          {"parallel_ordinal", key.parallel_ordinal}};
}

ArcKey arcKeyFromJson(const Json& value, std::string_view label)
{
  requireExactKeys(
      value, {"source", "sink", "kind", "owner_name", "library_source_port", "library_sink_port", "ordered_library_arc_semantic_signature", "parallel_ordinal"},
      label);
  if (!value.at("ordered_library_arc_semantic_signature").is_array()) {
    fail(label, "ordered library signatures are not an array");
  }
  ArcKey key;
  key.base.source = nodeKeyFromJson(value.at("source"), label);
  key.base.sink = nodeKeyFromJson(value.at("sink"), label);
  key.base.kind = enumFromToken(requireString(value.at("kind"), label), label, kArcKinds);
  key.base.owner_name = requireString(value.at("owner_name"), label);
  key.base.library_source_port = requireString(value.at("library_source_port"), label);
  key.base.library_sink_port = requireString(value.at("library_sink_port"), label);
  for (const Json& signature : value.at("ordered_library_arc_semantic_signature")) {
    key.base.library_arcs.push_back(signatureFromJson(signature, label));
  }
  key.parallel_ordinal = requireU64(value.at("parallel_ordinal"), label);
  if (const auto error = validateArcKeyBase(key.base)) {
    fail(label, *error);
  }
  return key;
}

Json neutralWireToJson(const NeutralWire& wire)
{
  validateWire(wire);
  return {{"timing_order", jsonArray(wire.timing_order, nodeKeyToJson)},
          {"out_rows", jsonArray(wire.out_rows, rowToJson)},
          {"in_rows", jsonArray(wire.in_rows, rowToJson)},
          {"nodes", jsonArray(wire.nodes, nodeToJson)},
          {"arcs", jsonArray(wire.arcs, arcToJson)}};
}

NeutralWire neutralWireFromJson(const Json& value)
{
  requireExactKeys(value, {"timing_order", "out_rows", "in_rows", "nodes", "arcs"}, "neutral wire");
  NeutralWire wire;
  wire.timing_order = vectorFromJson<NodeKey>(value.at("timing_order"), "timing_order", nodeKeyFromJson);
  wire.out_rows = vectorFromJson<NeutralAdjacencyRow>(value.at("out_rows"), "out_rows", rowFromJson);
  wire.in_rows = vectorFromJson<NeutralAdjacencyRow>(value.at("in_rows"), "in_rows", rowFromJson);
  wire.nodes = vectorFromJson<NeutralNode>(value.at("nodes"), "nodes", nodeFromJson);
  wire.arcs = vectorFromJson<NeutralArc>(value.at("arcs"), "arcs", arcFromJson);
  validateWire(wire);
  return wire;
}

std::string encodeNeutralWire(const NeutralWire& wire)
{
  return neutralWireToJson(wire).dump();
}

NeutralWire decodeNeutralWire(std::string_view bytes)
{
  return neutralWireFromJson(parseStrictJson(bytes, "neutral wire"));
}

}  // namespace ista::csr::real_diff
