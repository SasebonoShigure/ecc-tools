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
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

#include "CsrRealDifferentialProbeSupport.hpp"
#include "Database.hpp"

namespace ista::csr::real_diff {

using DatabaseWork = std::function<Json(Database&, std::uint64_t, const Json&)>;

struct ProbeExecution
{
  Json scenario;
  Json payload;
};

struct CanarySelection
{
  std::size_t delay_arc_native_rank{0};
  ArcKey delay_arc_key;
  NodeKey clock_node;
  NodeKey data_node;
  NodeKey endpoint_node;
};

// Owns the exact initialize -> guarded pre-full -> production full -> guarded
// post-full flow. The callback runs after the first existing-graph native
// stabilization while the post-full operation guard remains held.
[[nodiscard]] ProbeExecution executeProbeScenario(const ProbeOptions& options, const DatabaseWork& work);

// Re-runs timing on the existing graph only. It never rebuilds topology,
// levels, or timing order.
void runNativeExistingGraphTiming(Database& database);

namespace scenario_detail {

inline Json disabledDelayReceipt(const Arc& arc)
{
  const TimingArcDelayState& state = arc.get_timing_arc_delay_state();
  bool fallback_is_subset = !state.fallback_key_set.empty();
  for (const auto& [index, analysis, input, output] : state.fallback_key_set) {
    const auto arc_lane = state.delay_map.find(index);
    if (arc_lane == state.delay_map.end() || !arc_lane->second.contains(analysis) || !arc_lane->second.at(analysis).contains(input)
        || !arc_lane->second.at(analysis).at(input).contains(output)) {
      fallback_is_subset = false;
    }
  }
  const bool input_initialized = !arc.get_input_output_delay_map().empty() && !arc.get_trans_delay_map().empty() && !arc.get_trans_type_map().empty()
                                 && std::isfinite(arc.get_delay()) && std::isfinite(arc.get_delay_min()) && std::isfinite(arc.get_delay_max());
  if (!arc.get_graph_delay_map().empty() || !input_initialized || state.delay_map.empty() || !fallback_is_subset) {
    throw std::runtime_error("disabled Arc typed delay/fallback witness differs");
  }
  return {{"graph_delay_typed_empty", true},
          {"input_output_delay_initialized", true},
          {"timing_arc_delay_initialized", true},
          {"timing_arc_fallback_initialized", true}};
}

}  // namespace scenario_detail

// The reference lane supplies semantic keys selected from its actual snapshot
// input. Installation changes only present derived fields owned by the four
// native stages. Verification runs after native stabilization and marks every
// recorded field as restored to its pre-canary value.
namespace canary_detail {

[[noreturn]] inline void fail(std::string_view message)
{
  throw std::runtime_error(std::string(message));
}

inline bool active(const Arc& arc)
{
  return !arc.get_is_disable_arc() && !arc.get_is_loop_disable();
}

inline bool same(double lhs, double rhs)
{
  return lhs == rhs && (lhs != 0.0 || std::signbit(lhs) == std::signbit(rhs));
}

inline const TimingPoint& node(const Database& database, const Json& key)
{
  return database.get_timing_point_map().at(nodeKeyFromJson(key, "canary node").full_name);
}

}  // namespace canary_detail

[[nodiscard]] inline Json installGuaranteedOverwriteCanaries(Database& database, const CanarySelection& selection)
{
  Arc& arc = database.get_arc_list().at(selection.delay_arc_native_rank);
  if (!canary_detail::active(arc) || arc.get_source_pin() != selection.delay_arc_key.base.source.full_name
      || arc.get_sink_pin() != selection.delay_arc_key.base.sink.full_name || arc.get_graph_delay_map().empty()) {
    canary_detail::fail("delay canary Arc binding differs");
  }
  auto analysis = arc.get_graph_delay_map().begin();
  auto input = analysis->second.begin();
  auto output = input->second.begin();
  const double delay_before = output->second;
  TimingPoint& clock = database.get_timing_point_map().at(selection.clock_node.full_name);
  TimingPoint& data = database.get_timing_point_map().at(selection.data_node.full_name);
  TimingPoint& endpoint = database.get_timing_point_map().at(selection.endpoint_node.full_name);
  const bool clock_before = clock.get_is_clock_point();
  const double data_before = data.get_arrival();
  const double required_before = endpoint.get_required();
  const double slack_before = endpoint.get_slack();
  output->second = analysis->first == AnalysisType::kMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
  clock.set_is_clock_point(false);
  data.set_arrival(std::numeric_limits<double>::infinity());
  endpoint.set_required(-std::numeric_limits<double>::infinity());
  endpoint.set_slack(std::numeric_limits<double>::infinity());
  const bool delay_before_after_differ = !canary_detail::same(delay_before, output->second);
  const bool clock_before_after_differ = clock_before != clock.get_is_clock_point();
  const bool data_before_after_differ = !canary_detail::same(data_before, data.get_arrival());
  const bool required_before_after_differ = !canary_detail::same(required_before, endpoint.get_required());
  const bool slack_before_after_differ = !canary_detail::same(slack_before, endpoint.get_slack());
  if (!delay_before_after_differ) {
    canary_detail::fail("delay canary did not change its field");
  }
  if (!clock_before_after_differ) {
    canary_detail::fail("clock canary did not change its field");
  }
  if (!data_before_after_differ) {
    canary_detail::fail("data-arrival canary did not change its field");
  }
  if (!required_before_after_differ) {
    canary_detail::fail("required-time canary did not change its field");
  }
  if (!slack_before_after_differ) {
    canary_detail::fail("slack canary did not change its field");
  }
  Json fields = Json::array();
  fields.push_back({{"stage", "DelayCalculator"},
                    {"semantic_key", arcKeyToJson(selection.delay_arc_key)},
                    {"field", "graph_delay"},
                    {"before_after_differ", delay_before_after_differ},
                    {"rank", selection.delay_arc_native_rank},
                    {"a", static_cast<std::int32_t>(analysis->first)},
                    {"i", static_cast<std::int32_t>(input->first)},
                    {"o", static_cast<std::int32_t>(output->first)},
                    {"before", numberToJson(delay_before)}});
  fields.push_back({{"stage", "ClockPropagator"},
                    {"semantic_key", nodeKeyToJson(selection.clock_node)},
                    {"field", "is_clock_point"},
                    {"before_after_differ", clock_before_after_differ},
                    {"before", clock_before}});
  fields.push_back({{"stage", "TimingPropagator"},
                    {"semantic_key", nodeKeyToJson(selection.data_node)},
                    {"field", "arrival"},
                    {"before_after_differ", data_before_after_differ},
                    {"before", numberToJson(data_before)}});
  fields.push_back({{"stage", "TimingAnalyzer"},
                    {"semantic_key", nodeKeyToJson(selection.endpoint_node)},
                    {"field", "required+slack"},
                    {"before_after_differ", required_before_after_differ && slack_before_after_differ},
                    {"required_before", numberToJson(required_before)},
                    {"slack_before", numberToJson(slack_before)}});
  return {{"stage_fields", std::move(fields)}};
}

inline void verifyGuaranteedOverwriteCanaries(const Database& database, Json& receipt)
{
  requireExactKeys(receipt, {"stage_fields"}, "canary receipt");
  Json& fields = receipt.at("stage_fields");
  if (!fields.is_array() || fields.size() != 4) {
    canary_detail::fail("canary receipt stage cardinality differs");
  }
  for (Json& field : fields) {
    const std::string stage = field.at("stage").get<std::string>();
    const Json& witnessed_difference = field.at("before_after_differ");
    if (!witnessed_difference.is_boolean() || !witnessed_difference.get<bool>()) {
      canary_detail::fail("canary did not record a witnessed before/after difference");
    }
    const bool before_after_differ = witnessed_difference.get<bool>();
    bool overwritten = false;
    if (stage == "DelayCalculator" && field.at("field") == "graph_delay") {
      const Arc& arc = database.get_arc_list().at(field.at("rank").get<std::size_t>());
      const auto analysis = static_cast<AnalysisType>(field.at("a").get<std::int32_t>());
      const auto input = static_cast<TransType>(field.at("i").get<std::int32_t>());
      const auto output = static_cast<TransType>(field.at("o").get<std::int32_t>());
      overwritten = canary_detail::same(arc.get_graph_delay_map().at(analysis).at(input).at(output), numberFromJson(field.at("before"), "delay before"));
    } else if (stage == "ClockPropagator" && field.at("field") == "is_clock_point") {
      overwritten = canary_detail::node(database, field.at("semantic_key")).get_is_clock_point() == field.at("before").get<bool>();
    } else if (stage == "TimingPropagator" && field.at("field") == "arrival") {
      overwritten
          = canary_detail::same(canary_detail::node(database, field.at("semantic_key")).get_arrival(), numberFromJson(field.at("before"), "arrival before"));
    } else if (stage == "TimingAnalyzer" && field.at("field") == "required+slack") {
      const TimingPoint& point = canary_detail::node(database, field.at("semantic_key"));
      overwritten = canary_detail::same(point.get_required(), numberFromJson(field.at("required_before"), "required before"))
                    && canary_detail::same(point.get_slack(), numberFromJson(field.at("slack_before"), "slack before"));
    }
    if (!overwritten) {
      canary_detail::fail("native stabilization did not overwrite a guaranteed canary");
    }
    const Json key = field.at("semantic_key"), owned = field.at("field");
    field = {{"stage", stage}, {"semantic_key", key}, {"field", owned}, {"before_after_differ", before_after_differ}};
    for (const char* name : {"active", "reachable", "present", "post_rerun_overwritten"}) {
      field[name] = true;
    }
  }
}

}  // namespace ista::csr::real_diff
