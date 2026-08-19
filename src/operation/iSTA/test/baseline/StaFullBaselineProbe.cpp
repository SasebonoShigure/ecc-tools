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
// See the Mulan PSL v2 for more details.
// ***************************************************************************************

#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DataManager.hpp"
#include "IncrementalStaSession.hpp"
#include "STAInterface.hpp"
#include "StaFullBaselineEncoding.hpp"
#include "idm.h"
#include "json.hpp"

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace ista::baseline_test;

struct Options
{
  fs::path tech_lef;
  fs::path cell_lef;
  fs::path liberty;
  fs::path verilog;
  fs::path sdc;
  std::string top;
  std::string case_name;
  fs::path run_dir;
  fs::path snapshot;
  std::string expected_clock;
};
std::string ordinalSuffix(std::size_t ordinal)
{
  std::ostringstream stream;
  stream << "#parallel=" << std::setw(20) << std::setfill('0') << ordinal;
  return stream.str();
}

Json arcIdentity(ista::Arc& arc)
{
  return Json::array({arc.get_source_pin(), arc.get_sink_pin(), arcTypeName(arc.get_type()), arc.get_owner_name(), arc.get_library_source_port(),
                      arc.get_library_sink_port(), arc.get_is_clock_arc(), arc.get_is_disable_arc(), arc.get_is_loop_disable()});
}

struct ArcKeys
{
  std::vector<std::string> keys;
  std::vector<std::size_t> ordinals;
};

ArcKeys makeArcKeys(std::vector<ista::Arc>& arcs)
{
  ArcKeys result;
  result.keys.reserve(arcs.size());
  result.ordinals.reserve(arcs.size());
  std::map<std::string, std::size_t> next_ordinal;
  for (ista::Arc& arc : arcs) {
    std::string base = arcIdentity(arc).dump();
    std::size_t ordinal = next_ordinal[base]++;
    result.keys.push_back(base + ordinalSuffix(ordinal));
    result.ordinals.push_back(ordinal);
  }
  return result;
}

Json arcReference(std::size_t index, const ArcKeys& keys)
{
  if (index == std::numeric_limits<std::size_t>::max()) {
    return sentinel("missing");
  }
  if (index >= keys.keys.size()) {
    throw std::runtime_error("STA state contains an out-of-range arc reference");
  }
  return keys.keys[index];
}

std::map<int32_t, std::string> makeTimingArcKeys(ista::Arc& arc)
{
  std::map<int32_t, std::string> result;
  std::map<std::string, std::size_t> next_ordinal;
  ista::TimingCellArc* cell_arc = arc.get_timing_cell_arc();
  if (cell_arc == nullptr) {
    return result;
  }
  for (ista::TimingArc& timing_arc : cell_arc->get_timing_arc_list()) {
    Json identity = Json::array({timing_arc.get_library_name(), arcSenseName(timing_arc.get_sense()), transName(timing_arc.get_trigger_trans_type()),
                                 transName(timing_arc.get_check_trans_type()), timing_arc.get_sdf_cond()});
    std::string base = identity.dump();
    std::string key = base + ordinalSuffix(next_ordinal[base]++);
    if (!result.emplace(timing_arc.get_arc_idx(), key).second) {
      throw std::runtime_error("duplicate timing-arc index inside one cell arc");
    }
  }
  return result;
}

std::vector<std::string> sorted(std::vector<std::string> values)
{
  std::ranges::sort(values);
  return values;
}

void sortRecords(Json& records, const char* key)
{
  std::sort(records.begin(), records.end(),
            [key](const Json& lhs, const Json& rhs) { return lhs.at(key).get<std::string>() < rhs.at(key).get<std::string>(); });
}

Json encodeAdjacency(ista::Database& database, const ArcKeys& keys, bool incoming)
{
  Json result = Json::object();
  auto& adjacency = incoming ? database.get_incoming_arc_list_map() : database.get_outgoing_arc_list_map();
  for (auto& [node, pin] : database.get_pin_map()) {
    (void) pin;
    std::vector<std::string> semantic_arcs;
    if (auto it = adjacency.find(node); it != adjacency.end()) {
      for (std::size_t index : it->second) {
        if (index >= keys.keys.size()) {
          throw std::runtime_error("adjacency contains an out-of-range arc reference");
        }
        semantic_arcs.push_back(keys.keys[index]);
      }
    }
    result[node] = sorted(std::move(semantic_arcs));
  }
  return result;
}

Json encodeTopology(ista::Database& database, const ArcKeys& keys)
{
  Json nodes = Json::array();
  for (auto& [node, pin] : database.get_pin_map()) {
    if (pin.get_full_name() != node || node.empty()) {
      throw std::runtime_error("pin-map key is not the non-empty full pin name");
    }
    auto point_it = database.get_timing_point_map().find(node);
    if (point_it == database.get_timing_point_map().end()) {
      throw std::runtime_error("timing point missing for pin " + node);
    }
    std::string cell;
    if (auto instance_it = database.get_instance_map().find(pin.get_instance_name()); instance_it != database.get_instance_map().end()) {
      cell = instance_it->second.get_cell_name();
    }
    nodes.push_back({{"key", node},
                     {"direction", directionName(pin.get_direction())},
                     {"is_port", pin.get_is_port()},
                     {"instance", pin.get_instance_name()},
                     {"cell", cell},
                     {"net", pin.get_net_name()},
                     {"level", point_it->second.get_level()}});
  }

  Json arcs = Json::array();
  for (std::size_t index = 0; index < database.get_arc_list().size(); ++index) {
    ista::Arc& arc = database.get_arc_list()[index];
    arcs.push_back({{"key", keys.keys[index]},
                    {"identity", arcIdentity(arc)},
                    {"parallel_ordinal", keys.ordinals[index]},
                    {"source", arc.get_source_pin()},
                    {"sink", arc.get_sink_pin()},
                    {"type", arcTypeName(arc.get_type())},
                    {"owner", arc.get_owner_name()},
                    {"library_source_port", arc.get_library_source_port()},
                    {"library_sink_port", arc.get_library_sink_port()},
                    {"is_clock", arc.get_is_clock_arc()},
                    {"is_disabled", arc.get_is_disable_arc()},
                    {"is_loop_disabled", arc.get_is_loop_disable()}});
  }
  sortRecords(arcs, "key");

  return {{"nodes", nodes},
          {"arcs", arcs},
          {"incoming", encodeAdjacency(database, keys, true)},
          {"outgoing", encodeAdjacency(database, keys, false)},
          {"start_points", sorted(database.get_start_point_list())},
          {"end_points", sorted(database.get_end_point_list())},
          {"timing_order", database.get_timing_order_list()},
          {"counts",
           {{"instances", database.get_instance_map().size()},
            {"pins", database.get_pin_map().size()},
            {"nets", database.get_net_map().size()},
            {"arcs", database.get_arc_list().size()},
            {"timing_points", database.get_timing_point_map().size()},
            {"path_groups", database.get_timing_path_group_list().size()}}}};
}

Json encodeClockState(ista::TimingPoint& point, bool physical)
{
  auto encode_numeric = [](double value) { return encodeNumber(value); };
  auto encode_string = [](const std::string& value) { return Json(value); };
  auto encode_trans = [](ista::TransType value) { return Json(transName(value)); };
  if (physical) {
    return {{"arrival", encodeAnalysisTrans(point.get_physical_clock_arrival_map(), encode_numeric)},
            {"slew", encodeAnalysisTrans(point.get_physical_clock_slew_map(), encode_numeric)},
            {"predecessor", encodeAnalysisTrans(point.get_physical_clock_predecessor_map(), encode_string)},
            {"predecessor_arc_delay", encodeAnalysisTrans(point.get_physical_clock_predecessor_arc_delay_map(), encode_numeric)},
            {"predecessor_transition", encodeAnalysisTrans(point.get_physical_clock_predecessor_trans_type_map(), encode_trans)}};
  }
  return {{"arrival", encodeAnalysisTrans(point.get_clock_arrival_map(), encode_numeric)},
          {"slew", encodeAnalysisTrans(point.get_clock_slew_map(), encode_numeric)},
          {"predecessor", encodeAnalysisTrans(point.get_clock_predecessor_map(), encode_string)},
          {"predecessor_arc_delay", encodeAnalysisTrans(point.get_clock_predecessor_arc_delay_map(), encode_numeric)},
          {"predecessor_transition", encodeAnalysisTrans(point.get_clock_predecessor_trans_type_map(), encode_trans)}};
}

Json encodePathStates(ista::TimingPoint& point, const ArcKeys& keys)
{
  Json result = Json::array();
  for (auto& [analysis, by_source] : point.get_path_state_map()) {
    for (auto& [source, by_trans] : by_source) {
      for (auto& [trans, by_clock] : by_trans) {
        for (auto& [launch_clock, state] : by_clock) {
          result.push_back({{"analysis", analysisName(analysis)},
                            {"source_type", sourceName(source)},
                            {"transition", transName(trans)},
                            {"launch_clock", launch_clock},
                            {"arrival", encodeNumber(state.get_arrival())},
                            {"slew", encodeNumber(state.get_slew())},
                            {"launch_time", encodeNumber(state.get_launch_time())},
                            {"start_point", state.get_start_point()},
                            {"predecessor", state.get_predecessor()},
                            {"clock_name", state.get_clock_name()},
                            {"crpr_clock_pin", state.get_crpr_clock_pin()},
                            {"predecessor_arc", arcReference(state.get_predecessor_arc_idx(), keys)},
                            {"predecessor_arc_delay", encodeNumber(state.get_predecessor_arc_delay())},
                            {"state_transition", transName(state.get_trans_type())},
                            {"predecessor_transition", transName(state.get_predecessor_trans_type())},
                            {"crpr_clock_transition", transName(state.get_crpr_clock_trans_type())}});
        }
      }
    }
  }
  return result;
}

Json encodeTimingPoints(ista::Database& database, const ArcKeys& keys)
{
  Json result = Json::array();
  auto encode_numeric = [](double value) { return encodeNumber(value); };
  for (auto& [node, point] : database.get_timing_point_map()) {
    result.push_back({{"node", node},
                      {"arrival", encodeNumber(point.get_arrival())},
                      {"required", encodeNumber(point.get_required())},
                      {"slack", encodeNumber(point.get_slack())},
                      {"launch_time", encodeNumber(point.get_launch_time())},
                      {"level", point.get_level()},
                      {"predecessor", point.get_predecessor()},
                      {"clock_name", point.get_clock_name()},
                      {"predecessor_arc", arcReference(point.get_predecessor_arc_idx(), keys)},
                      {"is_clock_point", point.get_is_clock_point()},
                      {"data_slew", encodeAnalysisTrans(point.get_data_slew_map(), encode_numeric)},
                      {"clock_effective", encodeClockState(point, false)},
                      {"clock_physical", encodeClockState(point, true)},
                      {"path_states", encodePathStates(point, keys)}});
  }
  return result;
}

Json encodeArcTiming(ista::Database& database, const ArcKeys& keys)
{
  Json result = Json::array();
  auto encode_numeric = [](double value) { return encodeNumber(value); };
  for (std::size_t index = 0; index < database.get_arc_list().size(); ++index) {
    ista::Arc& arc = database.get_arc_list()[index];
    Json trans_map = Json::array();
    for (auto& [input, output] : arc.get_trans_type_map()) {
      trans_map.push_back({{"input_transition", transName(input)}, {"output_transition", transName(output)}});
    }
    std::map<int32_t, std::string> timing_arc_keys = makeTimingArcKeys(arc);
    Json timing_arc_delays = Json::array();
    for (auto& [timing_arc_index, analyses] : arc.get_timing_arc_delay_map()) {
      Json semantic_timing_arc = sentinel("missing");
      if (auto key_it = timing_arc_keys.find(timing_arc_index); key_it != timing_arc_keys.end()) {
        semantic_timing_arc = key_it->second;
      }
      for (auto& [analysis, inputs] : analyses) {
        for (auto& [input, outputs] : inputs) {
          for (auto& [output, delay] : outputs) {
            timing_arc_delays.push_back({{"timing_arc", semantic_timing_arc},
                                         {"analysis", analysisName(analysis)},
                                         {"input_transition", transName(input)},
                                         {"output_transition", transName(output)},
                                         {"value", encodeNumber(delay)}});
          }
        }
      }
    }
    std::sort(timing_arc_delays.begin(), timing_arc_delays.end(), [](const Json& lhs, const Json& rhs) { return lhs.dump() < rhs.dump(); });
    result.push_back({{"key", keys.keys[index]},
                      {"delay", encodeNumber(arc.get_delay())},
                      {"delay_max", encodeNumber(arc.get_delay_max())},
                      {"delay_min", encodeNumber(arc.get_delay_min())},
                      {"transition_delay", encodeAnalysisTrans(arc.get_trans_delay_map(), encode_numeric)},
                      {"input_output_delay", encodeAnalysisInputOutput(arc.get_input_output_delay_map(), encode_numeric)},
                      {"graph_delay", encodeAnalysisInputOutput(arc.get_graph_delay_map(), encode_numeric)},
                      {"timing_arc_delay", timing_arc_delays},
                      {"transition_map", trans_map}});
  }
  sortRecords(result, "key");
  return result;
}

Json encodePathPoint(ista::TimingPathPoint& point)
{
  return {{"pin", point.get_pin_name()},
          {"instance", point.get_instance_name()},
          {"cell", point.get_cell_name()},
          {"net", point.get_net_name()},
          {"arc_name", point.get_arc_name()},
          {"arc_source", point.get_source_pin()},
          {"arc_sink", point.get_sink_pin()},
          {"arc_type", arcTypeName(point.get_arc_type())},
          {"arc_delay", encodeNumber(point.get_arc_delay())},
          {"arrival", encodeNumber(point.get_arrival())},
          {"required", encodeNumber(point.get_required())},
          {"slack", encodeNumber(point.get_slack())},
          {"transition", transName(point.get_trans_type())}};
}

Json encodePathGroups(ista::Database& database)
{
  Json groups = Json::array();
  for (ista::TimingPathGroup& group : database.get_timing_path_group_list()) {
    Json endpoints = Json::array();
    for (auto& [endpoint_name, endpoint] : group.get_timing_path_end_map()) {
      Json paths = Json::array();
      for (ista::TimingPath& path : endpoint.get_timing_path_list()) {
        Json points = Json::array();
        for (ista::TimingPathPoint& point : path.get_point_list()) {
          points.push_back(encodePathPoint(point));
        }
        paths.push_back({{"start_point", path.get_start_point()},
                         {"end_point", path.get_end_point()},
                         {"analysis", analysisName(path.get_analysis_type())},
                         {"source_type", sourceName(path.get_source_type())},
                         {"transition", transName(path.get_trans_type())},
                         {"check_type", checkName(path.get_check_type())},
                         {"clock_name", path.get_clock_name()},
                         {"capture_clock_pin", path.get_capture_clock_pin()},
                         {"last_common_pin", path.get_last_common_pin()},
                         {"level", path.get_level()},
                         {"path_delay", encodeNumber(path.get_path_delay())},
                         {"required_time", encodeNumber(path.get_required_time())},
                         {"slack", encodeNumber(path.get_slack())},
                         {"cell_delay", encodeNumber(path.get_cell_delay())},
                         {"net_delay", encodeNumber(path.get_net_delay())},
                         {"launch_time", encodeNumber(path.get_launch_time())},
                         {"capture_time", encodeNumber(path.get_capture_time())},
                         {"launch_clock_network_delay", encodeNumber(path.get_launch_clock_network_delay())},
                         {"capture_clock_network_delay", encodeNumber(path.get_capture_clock_network_delay())},
                         {"clock_reconvergence_pessimism", encodeNumber(path.get_clock_reconvergence_pessimism())},
                         {"setup_time", encodeNumber(path.get_setup_time())},
                         {"check_time", encodeNumber(path.get_check_time())},
                         {"points", points}});
      }
      std::sort(paths.begin(), paths.end(), [](const Json& lhs, const Json& rhs) { return lhs.dump() < rhs.dump(); });
      endpoints.push_back({{"endpoint", endpoint_name}, {"path_count", paths.size()}, {"paths", paths}});
    }
    groups.push_back({{"name", group.get_group_name()}, {"endpoint_count", endpoints.size()}, {"endpoints", endpoints}});
  }
  sortRecords(groups, "name");
  return groups;
}

Json encodePowerValue(ista::PowerValue& value)
{
  return {{"internal", encodeNumber(value.get_internal_power())},
          {"switching", encodeNumber(value.get_switching_power())},
          {"leakage", encodeNumber(value.get_leakage_power())},
          {"total", encodeNumber(value.get_total_power())}};
}

Json encodePower(ista::Database& database)
{
  Json instances = Json::array();
  for (auto& [instance_name, instance] : database.get_instance_power_map()) {
    instances.push_back({{"instance", instance_name},
                         {"group", powerGroupName(instance.get_power_group_type())},
                         {"voltage", encodeNumber(instance.get_voltage())},
                         {"value", encodePowerValue(instance.get_power_value())}});
  }
  Json groups = Json::object();
  for (auto& [group, value] : database.get_power_summary().get_group_power_map()) {
    groups[powerGroupName(group)] = encodePowerValue(value);
  }
  return {{"instances", instances}, {"groups", groups}, {"total", encodePowerValue(database.get_power_summary().get_total_power_value())}};
}

Json encodeConstraints(ista::Database& database)
{
  Json clocks = Json::array();
  for (auto& [name, clock] : database.get_timing_constraint().get_clock_map()) {
    clocks.push_back({{"name", name},
                      {"sources", sorted(clock.get_source_list())},
                      {"period", encodeNumber(clock.get_period())},
                      {"rise_edge", encodeNumber(clock.get_rise_edge())},
                      {"fall_edge", encodeNumber(clock.get_fall_edge())},
                      {"is_propagated", clock.get_is_propagated()}});
  }
  Json ports = Json::array();
  for (auto& [name, port] : database.get_timing_constraint().get_port_constraint_map()) {
    ports.push_back({{"port", name},
                     {"clock", port.get_clock_name()},
                     {"input_delay_max", port.get_has_input_delay_max() ? encodeNumber(port.get_input_delay_max()) : sentinel("missing")},
                     {"input_delay_min", port.get_has_input_delay_min() ? encodeNumber(port.get_input_delay_min()) : sentinel("missing")},
                     {"output_delay_max", port.get_has_output_delay_max() ? encodeNumber(port.get_output_delay_max()) : sentinel("missing")},
                     {"output_delay_min", port.get_has_output_delay_min() ? encodeNumber(port.get_output_delay_min()) : sentinel("missing")},
                     {"input_transition", port.get_has_input_transition() ? encodeNumber(port.get_input_transition()) : sentinel("missing")},
                     {"load", port.get_has_load() ? encodeNumber(port.get_load()) : sentinel("missing")}});
  }
  Json cases = Json::object();
  for (auto& [pin, value] : database.get_timing_constraint().get_case_analysis_map()) {
    cases[pin] = value;
  }
  return {{"clocks", clocks}, {"ports", ports}, {"case_analysis", cases}};
}

Json buildSnapshot(const Options& options)
{
  ista::Database& database = STADM.getDatabase();
  ArcKeys arc_keys = makeArcKeys(database.get_arc_list());
  ista::TASummary& summary = database.get_summary().ta_summary;
  ista::Config& config = STADM.getConfig();
  ista::TimingLibrary& library = database.get_timing_library();
  Json snapshot
      = {{"schema_version", 1},
         {"probe", "ista-stage0-full-baseline-v1"},
         {"case", options.case_name},
         {"fixture",
          {{"case_name", options.case_name},
           {"top", options.top},
           {"inputs",
            {{"tech_lef", options.tech_lef.filename().string()},
             {"cell_lef", options.cell_lef.filename().string()},
             {"liberty", options.liberty.filename().string()},
             {"verilog", options.verilog.filename().string()},
             {"sdc", options.sdc.filename().string()}}}}},
         {"config",
          {{"thread_number", config.thread_number},
           {"output_timing_reports", config.output_timing_reports},
           {"output_timing_features", config.output_timing_features},
           {"timing_path_limit", config.timing_path_limit},
           {"max_paths", config.path_report_number},
           {"nworst", config.endpoint_path_report_number},
           {"timing_corner", config.timing_corner},
           {"timing_report_delay_type", config.timing_report_delay_type},
           {"timing_report_start_end_type", config.timing_report_start_end_type},
           {"is_path_report_number_specified", config.is_path_report_number_specified},
           {"slack_lesser_than", config.has_timing_report_slack_lesser_than ? encodeNumber(config.timing_report_slack_lesser_than) : sentinel("missing")},
           {"slack_greater_than", config.has_timing_report_slack_greater_than ? encodeNumber(config.timing_report_slack_greater_than) : sentinel("missing")}}},
         {"semantics",
          {{"canonical_time_unit", "ns"},
           {"node_key", "full_pin_name"},
           {"arc_key", "[source,sink,type,owner,library_source,library_sink,clock,disabled,loop_disabled]+parallel_ordinal"},
           {"missing", sentinel("missing")},
           {"positive_infinity", sentinel("pos_inf")},
           {"negative_infinity", sentinel("neg_inf")},
           {"nan", "invalid"}}},
         {"library_units",
          {{"source_time", timeUnitName(library.get_time_unit())},
           {"source_capacitance", capUnitName(library.get_cap_unit())},
           {"source_resistance", resistanceUnitName(library.get_resistance_unit())}}},
         {"constraints", encodeConstraints(database)},
         {"topology", encodeTopology(database, arc_keys)},
         {"timing",
          {{"points", encodeTimingPoints(database, arc_keys)},
           {"arcs", encodeArcTiming(database, arc_keys)},
           {"path_groups", encodePathGroups(database)},
           {"summary",
            {{"timing_path_count", summary.timing_path_num},
             {"checked_endpoint_count", summary.checked_end_point_num},
             {"unconstrained_endpoint_count", summary.unconstrained_end_point_num},
             {"violating_endpoint_count", summary.violating_end_point_num},
             {"worst_slack", encodeNumber(summary.worst_slack)},
             {"total_negative_slack", encodeNumber(summary.total_negative_slack)},
             {"worst_endpoint", summary.worst_end_point}}}}},
         {"power", encodePower(database)},
         {"complete", true}};
  return snapshot;
}

Options parseOptions(int argc, char* argv[])
{
  std::map<std::string, std::string> values;
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc || std::string(argv[index]).starts_with("--") == false) {
      throw std::runtime_error("arguments must be --option value pairs");
    }
    if (!values.emplace(argv[index], argv[index + 1]).second) {
      throw std::runtime_error("duplicate option: " + std::string(argv[index]));
    }
  }
  const std::vector<std::string> required
      = {"--tech-lef", "--cell-lef", "--liberty", "--verilog", "--sdc", "--top", "--case-name", "--run-dir", "--snapshot", "--expected-clock"};
  for (const std::string& name : required) {
    if (!values.contains(name) || values.at(name).empty()) {
      throw std::runtime_error("missing required option: " + name);
    }
  }
  if (values.size() != required.size()) {
    throw std::runtime_error("unknown command-line option");
  }
  return {values.at("--tech-lef"), values.at("--cell-lef"),  values.at("--liberty"), values.at("--verilog"),  values.at("--sdc"),
          values.at("--top"),      values.at("--case-name"), values.at("--run-dir"), values.at("--snapshot"), values.at("--expected-clock")};
}

void validateInputs(const Options& options)
{
  for (const fs::path* input : {&options.tech_lef, &options.cell_lef, &options.liberty, &options.verilog, &options.sdc}) {
    if (!fs::is_regular_file(*input)) {
      throw std::runtime_error("input file does not exist: " + input->string());
    }
  }
  fs::create_directories(options.run_dir);
  if (!options.snapshot.parent_path().empty()) {
    fs::create_directories(options.snapshot.parent_path());
  }
}

void loadDesign(const Options& options)
{
  if (!dmInst->readLef(std::vector<std::string>{options.tech_lef.string()}, true)) {
    throw std::runtime_error("failed to read technology LEF");
  }
  if (!dmInst->readLef(std::vector<std::string>{options.cell_lef.string()})) {
    throw std::runtime_error("failed to read cell LEF");
  }
  dmInst->get_config().set_sdc_path(options.sdc.string());
  if (!dmInst->readVerilog(options.verilog.string(), options.top)) {
    throw std::runtime_error("failed to read Verilog");
  }
}

void requireOracleState(const Options& options)
{
  ista::Database& database = STADM.getDatabase();
  if (!database.get_timing_constraint().get_clock_map().contains(options.expected_clock)) {
    throw std::runtime_error("expected clock was not created: " + options.expected_clock);
  }
  if (database.get_end_point_list().empty()) {
    throw std::runtime_error("full STA produced no endpoints");
  }
  if (database.get_timing_path_group_list().empty()) {
    throw std::runtime_error("full STA produced no timing path groups");
  }
}

void writeAtomically(const fs::path& path, const Json& value)
{
  if (!path.parent_path().empty()) {
    fs::create_directories(path.parent_path());
  }
  fs::path temporary = path;
  temporary += ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to open temporary snapshot: " + temporary.string());
  }
  output << value.dump(2) << '\n';
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write temporary snapshot: " + temporary.string());
  }
  std::error_code error;
  fs::rename(temporary, path, error);
  if (error) {
    throw std::runtime_error("failed to atomically publish snapshot: " + error.message());
  }
}

}  // namespace

int main(int argc, char* argv[])
{
  try {
    Options options = parseOptions(argc, argv);
    validateInputs(options);
    loadDesign(options);
    std::map<std::string, std::any> config = {{"-temp_directory_path", options.run_dir.string()},
                                              {"-thread_number", int32_t{1}},
                                              {"-output_timing_reports", int32_t{1}},
                                              {"-output_timing_features", int32_t{1}},
                                              {"-timing_path_limit", int32_t{20}},
                                              {"-max_paths", int32_t{20}},
                                              {"-nworst", int32_t{20}},
                                              {"-timing_corner", std::string{}}};
    Json first_snapshot;
    for (int run = 0; run < 2; ++run) {
      if (!dmInst->readLib(std::vector<std::string>{options.liberty.string()})) {
        throw std::runtime_error("failed to read Liberty");
      }
      const ista::StaUpdateStrategy strategy = run == 0 ? ista::StaUpdateStrategy::kFull : ista::StaUpdateStrategy::kIncremental;
      ista::IncrementalStaSession session({ista::StaBackend::kCpu, strategy, ista::StaFallbackPolicy::kError});
      ista::throwOnStaFailure(session.initialize(config), session.request());
      if (run == 0) {
        ista::IncrementalStaSession overlap({ista::StaBackend::kCpu, ista::StaUpdateStrategy::kFull, ista::StaFallbackPolicy::kError});
        if (overlap.initialize(config).code != ista::StaSessionStatusCode::kOwnerConflict) {
          throw std::runtime_error("active session did not reject a second session");
        }
        for (void (*blocked)() : std::array<void (*)(), 2>{+[] { STAI.destroySTA(); }, +[] { ista::STAInterface::destroyInst(); }}) {
          try {
            blocked();
          } catch (const std::runtime_error&) {
            continue;
          }
          throw std::runtime_error("active session did not reject a legacy/global destroy");
        }
      }
      ista::StaExecutionRecord record = ista::requireStaExecutionRecord(session.runFullBaseline(), session.request());
      const bool expected_record = run == 0 ? record.execution_kind == ista::StaExecutionKind::kFullUpdate && !record.fallback_reason
                                            : record.execution_kind == ista::StaExecutionKind::kBaselineOnly
                                                  && record.fallback_reason == ista::StaFallbackReason::kNotApplicableNoMutation;
      if (!expected_record || record.fallback_to_full) {
        throw std::runtime_error("full/baseline-only execution record mismatch");
      }
      requireOracleState(options);
      Json snapshot = buildSnapshot(options);
      if (run == 0) {
        first_snapshot = std::move(snapshot);
      } else {
        if (snapshot != first_snapshot)
          throw std::runtime_error("same-process STA sessions produced different snapshots");
        writeAtomically(options.snapshot, snapshot);
      }
      ista::throwOnStaFailure(session.close(), session.request());
    }
    ista::STAInterface::destroyInst();
    return 0;
  } catch (const std::exception& error) {
    try {
      ista::STAInterface::destroyInst();
    } catch (const std::exception& cleanup_error) {
      std::cerr << "StaFullBaselineProbe cleanup: " << cleanup_error.what() << '\n';
    }
    std::cerr << "StaFullBaselineProbe: " << error.what() << '\n';
    return 1;
  }
}
