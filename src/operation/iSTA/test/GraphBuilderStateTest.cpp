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

#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "GraphBuilder.hpp"
#include "Logger.hpp"
#include "Utility.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using ArcSignature = std::tuple<std::string, std::string, ista::ArcType, std::string>;

struct GraphSnapshot
{
  std::size_t timing_point_count = 0;
  std::vector<ArcSignature> arc_list;
  std::map<std::string, std::vector<ArcSignature>> outgoing_arc_map;
  std::map<std::string, std::vector<ArcSignature>> incoming_arc_map;
  std::vector<std::string> start_point_list;
  std::vector<std::string> end_point_list;
  std::vector<std::string> timing_order_list;
  std::map<std::string, int32_t> level_map;

  bool operator==(const GraphSnapshot&) const = default;
};

class SingletonScope
{
 public:
  SingletonScope()
  {
    ista::Logger::initInst();
    ista::Utility::initInst();
    ista::DataManager::initInst();
    ista::DelayCalculator::initInst();
    ista::GraphBuilder::initInst();
    ista::DelayCalculator::getInst().init();
  }

  ~SingletonScope()
  {
    ista::GraphBuilder::destroyInst();
    ista::DelayCalculator::getInst().destroy();
    ista::DelayCalculator::destroyInst();
    ista::DataManager::destroyInst();
    ista::Utility::destroyInst();
    ista::Logger::destroyInst();
  }

  SingletonScope(const SingletonScope&) = delete;
  SingletonScope& operator=(const SingletonScope&) = delete;
};

ista::Pin makePin(const std::string& full_name, const std::string& pin_name, const std::string& instance_name,
                  const std::string& net_name, ista::PinDirection direction, bool is_port)
{
  ista::Pin pin;
  pin.set_full_name(full_name);
  pin.set_pin_name(pin_name);
  pin.set_instance_name(instance_name);
  pin.set_net_name(net_name);
  pin.set_direction(direction);
  pin.set_is_port(is_port);
  return pin;
}

ista::Net makeNet(const std::string& net_name, const std::vector<std::string>& pin_name_list)
{
  ista::Net net;
  net.set_net_name(net_name);
  net.set_pin_name_list(pin_name_list);
  return net;
}

ista::TimingCellPort makeTimingCellPort(const std::string& port_name, bool is_input, bool is_output)
{
  ista::TimingCellPort port;
  port.set_port_name(port_name);
  port.set_is_input(is_input);
  port.set_is_output(is_output);
  return port;
}

void populateStaticChain(ista::Database& database)
{
  database.get_pin_map().emplace("data_in",
                                 makePin("data_in", "data_in", "", "n_in", ista::PinDirection::kInput, true));
  database.get_pin_map().emplace("u0:A", makePin("u0:A", "A", "u0", "n_in", ista::PinDirection::kInput, false));
  database.get_pin_map().emplace("u0:Y", makePin("u0:Y", "Y", "u0", "n_out", ista::PinDirection::kOutput, false));
  database.get_pin_map().emplace("data_out",
                                 makePin("data_out", "data_out", "", "n_out", ista::PinDirection::kOutput, true));

  database.get_net_map().emplace("n_in", makeNet("n_in", {"data_in", "u0:A"}));
  database.get_net_map().emplace("n_out", makeNet("n_out", {"u0:Y", "data_out"}));

  ista::Instance instance;
  instance.set_instance_name("u0");
  instance.set_cell_name("BUF_X1");
  instance.set_pin_name_list({"u0:A", "u0:Y"});
  database.get_instance_map().emplace("u0", instance);

  ista::TimingCellArc cell_arc;
  cell_arc.set_source_port("A");
  cell_arc.set_sink_port("Y");

  ista::TimingCell timing_cell;
  timing_cell.set_cell_name("BUF_X1");
  timing_cell.get_port_map().emplace("A", makeTimingCellPort("A", true, false));
  timing_cell.get_port_map().emplace("Y", makeTimingCellPort("Y", false, true));
  timing_cell.get_cell_arc_list().push_back(cell_arc);
  database.get_timing_library().get_cell_map().emplace("BUF_X1", timing_cell);
}

ArcSignature getArcSignature(ista::Arc& arc)
{
  return {arc.get_source_pin(), arc.get_sink_pin(), arc.get_type(), arc.get_owner_name()};
}

std::vector<ArcSignature> getArcSignatureList(ista::Database& database, const std::vector<std::size_t>& arc_idx_list)
{
  std::vector<ArcSignature> signatures;
  for (std::size_t arc_idx : arc_idx_list) {
    if (arc_idx >= database.get_arc_list().size()) {
      throw std::runtime_error("adjacency list contains an invalid arc index");
    }
    signatures.push_back(getArcSignature(database.get_arc_list()[arc_idx]));
  }
  return signatures;
}

GraphSnapshot captureGraphSnapshot(ista::Database& database)
{
  GraphSnapshot snapshot;
  snapshot.timing_point_count = database.get_timing_point_map().size();
  for (ista::Arc& arc : database.get_arc_list()) {
    snapshot.arc_list.push_back(getArcSignature(arc));
  }
  for (auto& [pin_name, arc_idx_list] : database.get_outgoing_arc_list_map()) {
    snapshot.outgoing_arc_map.emplace(pin_name, getArcSignatureList(database, arc_idx_list));
  }
  for (auto& [pin_name, arc_idx_list] : database.get_incoming_arc_list_map()) {
    snapshot.incoming_arc_map.emplace(pin_name, getArcSignatureList(database, arc_idx_list));
  }
  snapshot.start_point_list = database.get_start_point_list();
  snapshot.end_point_list = database.get_end_point_list();
  snapshot.timing_order_list = database.get_timing_order_list();
  for (auto& [pin_name, timing_point] : database.get_timing_point_map()) {
    snapshot.level_map.emplace(pin_name, timing_point.get_level());
  }
  return snapshot;
}

bool expect(bool condition, const std::string& message)
{
  if (!condition) {
    std::cerr << "GraphBuilderStateTest: " << message << '\n';
  }
  return condition;
}

bool hasUniqueEntries(const std::vector<std::string>& values)
{
  return std::set<std::string>(values.begin(), values.end()).size() == values.size();
}

bool validateExpectedStaticChain(const GraphSnapshot& snapshot, const std::string& build_name)
{
  const ArcSignature input_net_arc = {"data_in", "u0:A", ista::ArcType::kNet, "n_in"};
  const ArcSignature cell_arc = {"u0:A", "u0:Y", ista::ArcType::kCell, "u0"};
  const ArcSignature output_net_arc = {"u0:Y", "data_out", ista::ArcType::kNet, "n_out"};
  const std::vector<ArcSignature> expected_arcs = {cell_arc, input_net_arc, output_net_arc};
  const std::map<std::string, std::vector<ArcSignature>> expected_outgoing = {
      {"data_in", {input_net_arc}}, {"data_out", {}}, {"u0:A", {cell_arc}}, {"u0:Y", {output_net_arc}}};
  const std::map<std::string, std::vector<ArcSignature>> expected_incoming = {
      {"data_in", {}}, {"data_out", {output_net_arc}}, {"u0:A", {input_net_arc}}, {"u0:Y", {cell_arc}}};
  const std::map<std::string, int32_t> expected_levels = {{"data_in", 1}, {"data_out", 4}, {"u0:A", 2}, {"u0:Y", 3}};

  const auto cell_arc_count = std::count_if(snapshot.arc_list.begin(), snapshot.arc_list.end(), [](const ArcSignature& arc) {
    return std::get<2>(arc) == ista::ArcType::kCell;
  });
  const auto net_arc_count = std::count_if(snapshot.arc_list.begin(), snapshot.arc_list.end(), [](const ArcSignature& arc) {
    return std::get<2>(arc) == ista::ArcType::kNet;
  });

  bool valid = true;
  valid = expect(snapshot.timing_point_count == 4, build_name + " should contain exactly four timing points") && valid;
  valid = expect(snapshot.arc_list == expected_arcs, build_name + " should contain one cell arc and two net arcs in build order") && valid;
  valid = expect(cell_arc_count == 1 && net_arc_count == 2, build_name + " arc type counts should not be duplicated") && valid;
  valid = expect(snapshot.outgoing_arc_map == expected_outgoing, build_name + " outgoing adjacency should match the static chain") && valid;
  valid = expect(snapshot.incoming_arc_map == expected_incoming, build_name + " incoming adjacency should match the static chain") && valid;
  valid = expect(snapshot.start_point_list == std::vector<std::string>{"data_in"}, build_name + " should have one start point") && valid;
  valid = expect(snapshot.end_point_list == std::vector<std::string>{"data_out"}, build_name + " should have one end point") && valid;
  valid = expect(snapshot.timing_order_list == std::vector<std::string>{"data_in", "u0:A", "u0:Y", "data_out"},
                 build_name + " should have a complete, stable topological order")
          && valid;
  valid = expect(hasUniqueEntries(snapshot.start_point_list), build_name + " start points should be unique") && valid;
  valid = expect(hasUniqueEntries(snapshot.end_point_list), build_name + " end points should be unique") && valid;
  valid = expect(hasUniqueEntries(snapshot.timing_order_list), build_name + " timing order should not contain duplicate nodes") && valid;
  valid = expect(snapshot.level_map == expected_levels, build_name + " levels should increase from one through four") && valid;
  return valid;
}

bool testRepeatedBuildOnUnchangedDatabase()
{
  ista::Database& database = ista::DataManager::getInst().getDatabase();
  populateStaticChain(database);

  // This test characterizes build() called repeatedly on one unchanged, static database.
  // Mutation between builds and cross-session reset behavior are intentionally outside its contract.
  ista::GraphBuilder::getInst().build();
  const GraphSnapshot first_snapshot = captureGraphSnapshot(database);
  ista::GraphBuilder::getInst().build();
  const GraphSnapshot second_snapshot = captureGraphSnapshot(database);

  bool valid = validateExpectedStaticChain(first_snapshot, "first build");
  valid = validateExpectedStaticChain(second_snapshot, "second build") && valid;
  valid = expect(second_snapshot == first_snapshot, "repeated build on an unchanged database should preserve the complete graph snapshot") && valid;
  return valid;
}

}  // namespace

int main()
{
  SingletonScope singleton_scope;
  try {
    return testRepeatedBuildOnUnchangedDatabase() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "GraphBuilderStateTest: " << error.what() << '\n';
    return 1;
  }
}
