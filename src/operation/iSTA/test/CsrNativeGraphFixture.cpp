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
#include "CsrNativeGraphFixture.hpp"

#include <algorithm>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Arc.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "Net.hpp"
#include "Pin.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingCellPort.hpp"
#include "TimingPoint.hpp"
#include "TimingTable.hpp"

namespace ista::csr::test {
namespace {

Pin makePin(const std::string& full_name, const std::string& local_name, const std::string& instance_name, const std::string& net_name,
            ista::PinDirection direction, bool is_port)
{
  Pin pin;
  pin.set_full_name(full_name);
  pin.set_pin_name(local_name);
  pin.set_instance_name(instance_name);
  pin.set_net_name(net_name);
  pin.set_direction(direction);
  pin.set_is_port(is_port);
  return pin;
}

void addPort(Database& database, const std::string& name, ista::PinDirection direction, const std::string& net_name = {})
{
  database.get_pin_map().emplace(name, makePin(name, name, {}, net_name, direction, true));
  database.get_timing_point_map().emplace(name, TimingPoint{});
}

void addInstancePin(Database& database, const std::string& instance_name, const std::string& local_name, ista::PinDirection direction,
                    const std::string& net_name = {})
{
  const std::string full_name = instance_name + ":" + local_name;
  database.get_pin_map().emplace(full_name, makePin(full_name, local_name, instance_name, net_name, direction, false));
  database.get_timing_point_map().emplace(full_name, TimingPoint{});
}

void addNet(Database& database, const std::string& name, std::vector<std::string> pins)
{
  Net net;
  net.set_net_name(name);
  net.set_pin_name_list(std::move(pins));
  database.get_net_map().emplace(name, std::move(net));
}

void addInstance(Database& database, const std::string& name, const std::string& cell, std::vector<std::string> pins)
{
  Instance instance;
  instance.set_instance_name(name);
  instance.set_cell_name(cell);
  instance.set_pin_name_list(std::move(pins));
  database.get_instance_map().emplace(name, std::move(instance));
}

std::size_t addArc(Database& database, const std::string& source, const std::string& sink, ArcType type, const std::string& owner, bool is_clock = false,
                   bool is_disabled = false, bool is_loop_disabled = false)
{
  Arc arc;
  arc.set_arc_name(owner + ":" + source + "->" + sink);
  arc.set_source_pin(source);
  arc.set_sink_pin(sink);
  arc.set_owner_name(owner);
  arc.set_type(type);
  arc.set_is_clock_arc(is_clock);
  arc.set_is_disable_arc(is_disabled);
  arc.set_is_loop_disable(is_loop_disabled);
  database.get_arc_list().push_back(std::move(arc));
  const std::size_t index = database.get_arc_list().size() - 1;
  database.get_outgoing_arc_list_map()[source].push_back(index);
  database.get_incoming_arc_list_map()[sink].push_back(index);
  return index;
}

std::size_t addNetArc(Database& database, const std::string& source, const std::string& sink, const std::string& net, bool is_clock = false,
                      bool is_disabled = false, bool is_loop_disabled = false)
{
  return addArc(database, source, sink, ArcType::kNet, net, is_clock, is_disabled, is_loop_disabled);
}

std::size_t addUnboundCellArc(Database& database, const std::string& source, const std::string& sink, const std::string& instance, bool is_clock = false,
                              bool is_disabled = false, bool is_loop_disabled = false)
{
  return addArc(database, source, sink, ArcType::kCell, instance, is_clock, is_disabled, is_loop_disabled);
}

TimingCellPort makeCellPort(const std::string& name, bool is_input, bool is_output)
{
  TimingCellPort port;
  port.set_port_name(name);
  port.set_is_input(is_input);
  port.set_is_output(is_output);
  return port;
}

TimingTable makeTable(TimingTableVariableType variable1, TimingTableVariableType variable2)
{
  TimingTable table;
  table.set_variable_type1(variable1);
  table.set_variable_type2(variable2);
  table.set_value_list({0.25});
  return table;
}

void addBoundCell(Database& database, bool rich_signature)
{
  TimingCellArc cell_arc;
  cell_arc.set_source_port("A");
  cell_arc.set_sink_port("Y");
  if (rich_signature) {
    TimingArc child;
    child.set_library_name("slow.lib");
    child.set_arc_idx(-7);
    child.set_sense(TimingArcSense::kNegative);
    child.set_trigger_trans_type(TransType::kRise);
    child.set_check_trans_type(TransType::kNone);
    child.set_sdf_cond("EN");
    child.get_delay_table_map().emplace(TransType::kRise, makeTable(TimingTableVariableType::kInputTransition, TimingTableVariableType::kOutputCapacitance));
    child.get_slew_table_map().emplace(TransType::kFall, makeTable(TimingTableVariableType::kConstrainedTransition, TimingTableVariableType::kNone));
    cell_arc.get_timing_arc_list().push_back(std::move(child));
  }

  TimingCell cell;
  cell.set_cell_name("BUF_X1");
  cell.set_library_name("slow.lib");
  cell.get_port_map().emplace("A", makeCellPort("A", true, false));
  cell.get_port_map().emplace("Y", makeCellPort("Y", false, true));
  cell.get_cell_arc_list().push_back(std::move(cell_arc));
  database.get_timing_library().get_cell_map().emplace("BUF_X1", std::move(cell));
}

std::size_t addBoundCellArc(Database& database, const std::string& instance, bool is_clock = false, bool is_disabled = false, bool is_loop_disabled = false)
{
  const std::string source = instance + ":A";
  const std::string sink = instance + ":Y";
  const std::size_t index = addArc(database, source, sink, ArcType::kCell, instance, is_clock, is_disabled, is_loop_disabled);
  Arc& arc = database.get_arc_list()[index];
  arc.set_library_source_port("A");
  arc.set_library_sink_port("Y");
  TimingCell& cell = database.get_timing_library().get_cell_map().at("BUF_X1");
  arc.set_timing_cell_arc(&cell.get_cell_arc_list().at(0));
  return index;
}

void setTimingState(Database& database, std::vector<std::string> order, const std::map<std::string, std::int32_t>& levels)
{
  database.get_timing_order_list() = std::move(order);
  for (const auto& [name, level] : levels) {
    database.get_timing_point_map().at(name).set_level(level);
  }
}

void setStartEnd(Database& database, std::vector<std::string> starts, std::vector<std::string> ends)
{
  database.get_start_point_list() = std::move(starts);
  database.get_end_point_list() = std::move(ends);
}

void populateChain(Database& database, bool rich_signature)
{
  addPort(database, "in", ista::PinDirection::kInput, "n_in");
  addInstancePin(database, "u0", "A", ista::PinDirection::kInput, "n_in");
  addInstancePin(database, "u0", "Y", ista::PinDirection::kOutput, "n_out");
  addPort(database, "out", ista::PinDirection::kOutput, "n_out");
  addNet(database, "n_in", {"in", "u0:A"});
  addNet(database, "n_out", {"u0:Y", "out"});
  addInstance(database, "u0", "BUF_X1", {"u0:A", "u0:Y"});
  addBoundCell(database, rich_signature);
  addNetArc(database, "in", "u0:A", "n_in");
  addBoundCellArc(database, "u0");
  addNetArc(database, "u0:Y", "out", "n_out");
  setStartEnd(database, {"in"}, {"out"});
  setTimingState(database, {"in", "u0:A", "u0:Y", "out"}, {{"in", 1}, {"u0:A", 2}, {"u0:Y", 3}, {"out", 4}});
}

void populatePortGraph(Database& database, const std::vector<std::string>& nodes, const std::vector<std::pair<std::size_t, std::size_t>>& edges,
                       const std::vector<std::string>& starts, const std::vector<std::string>& ends)
{
  for (const std::string& node : nodes) {
    addPort(database, node, ista::PinDirection::kInout, "shared");
  }
  addNet(database, "shared", nodes);
  for (const auto& [source, sink] : edges) {
    addNetArc(database, nodes.at(source), nodes.at(sink), "shared");
  }
  setStartEnd(database, starts, ends);
}

void populateDiamond(Database& database)
{
  populatePortGraph(database, {"root", "left", "right", "sink"}, {{0, 2}, {0, 1}, {2, 3}, {1, 3}}, {"root"}, {"sink"});
  setTimingState(database, {"root", "right", "left", "sink"}, {{"root", 1}, {"left", 2}, {"right", 2}, {"sink", 3}});
}

void populateMultiRoot(Database& database)
{
  populatePortGraph(database, {"z", "a", "sink"}, {{0, 2}, {1, 2}}, {"z", "a"}, {"sink"});
  setTimingState(database, {"a", "z", "sink"}, {{"a", 1}, {"z", 1}, {"sink", 2}});
}

void populateParallel(Database& database, bool flags)
{
  populatePortGraph(database, {"aa", "b"}, {}, {"aa"}, {"b"});
  addNetArc(database, "aa", "b", "shared", flags, flags, false);
  addNetArc(database, "aa", "b", "shared", false, false, flags);
  auto& outgoing = database.get_outgoing_arc_list_map().at("aa");
  auto& incoming = database.get_incoming_arc_list_map().at("b");
  std::reverse(outgoing.begin(), outgoing.end());
  std::reverse(incoming.begin(), incoming.end());
  if (flags) {
    setTimingState(database, {"aa", "b"}, {{"aa", 1}, {"b", 1}});
  } else {
    setTimingState(database, {"aa", "b"}, {{"aa", 1}, {"b", 2}});
  }
}

void populateUnboundFallback(Database& database)
{
  addInstancePin(database, "fallback", "A", ista::PinDirection::kInput);
  addInstancePin(database, "fallback", "Y", ista::PinDirection::kOutput);
  addInstance(database, "fallback", "NO_LIB_CELL", {"fallback:A", "fallback:Y"});
  addUnboundCellArc(database, "fallback:A", "fallback:Y", "fallback");
  setStartEnd(database, {"fallback:A"}, {"fallback:Y"});
  setTimingState(database, {"fallback:A", "fallback:Y"}, {{"fallback:A", 1}, {"fallback:Y", 2}});
}

void populateActiveCycle(Database& database)
{
  populatePortGraph(database, {"cycle_a", "cycle_b"}, {{0, 1}, {1, 0}}, {}, {});
  setTimingState(database, {}, {{"cycle_a", 0}, {"cycle_b", 0}});
}

void populateUtf8(Database& database)
{
  populatePortGraph(database, {"é", "中"}, {{0, 1}}, {"é"}, {"中"});
  setTimingState(database, {"é", "中"}, {{"é", 1}, {"中", 2}});
}

std::uint64_t nextRandom(std::uint64_t& state)
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

}  // namespace

CsrNativeGraphFixture::CsrNativeGraphFixture() : _database(std::make_unique<Database>())
{
}
CsrNativeGraphFixture::~CsrNativeGraphFixture() = default;
CsrNativeGraphFixture::CsrNativeGraphFixture(CsrNativeGraphFixture&&) noexcept = default;
CsrNativeGraphFixture& CsrNativeGraphFixture::operator=(CsrNativeGraphFixture&&) noexcept = default;

Database& CsrNativeGraphFixture::database()
{
  return *_database;
}

const Database& CsrNativeGraphFixture::database() const
{
  return *_database;
}

CsrNativeGraphFixture makeNativeGraphFixture(NativeGraphFixtureCase fixture_case)
{
  CsrNativeGraphFixture fixture;
  Database& database = fixture.database();
  switch (fixture_case) {
    case NativeGraphFixtureCase::kEmpty:
      break;
    case NativeGraphFixtureCase::kSingleton:
      addPort(database, "only", ista::PinDirection::kInout);
      setStartEnd(database, {"only"}, {"only"});
      setTimingState(database, {"only"}, {{"only", 1}});
      break;
    case NativeGraphFixtureCase::kChain:
      populateChain(database, false);
      break;
    case NativeGraphFixtureCase::kDiamond:
      populateDiamond(database);
      break;
    case NativeGraphFixtureCase::kMultiRoot:
      populateMultiRoot(database);
      break;
    case NativeGraphFixtureCase::kParallel:
      populateParallel(database, false);
      break;
    case NativeGraphFixtureCase::kFlags:
      populateParallel(database, true);
      break;
    case NativeGraphFixtureCase::kSparseAdjacency:
      addPort(database, "absent", ista::PinDirection::kInout);
      setTimingState(database, {"absent"}, {{"absent", 1}});
      break;
    case NativeGraphFixtureCase::kUnboundFallback:
      populateUnboundFallback(database);
      break;
    case NativeGraphFixtureCase::kMixedFallback:
      populateUnboundFallback(database);
      addBoundCell(database, false);
      addInstancePin(database, "bound", "A", ista::PinDirection::kInput);
      addInstancePin(database, "bound", "Y", ista::PinDirection::kOutput);
      addInstance(database, "bound", "BUF_X1", {"bound:A", "bound:Y"});
      addBoundCellArc(database, "bound");
      addBoundCellArc(database, "bound");
      setStartEnd(database, {"fallback:A", "bound:A"}, {"fallback:Y", "bound:Y"});
      setTimingState(database, {"bound:A", "fallback:A", "bound:Y", "fallback:Y"},
                     {{"fallback:A", 1}, {"bound:A", 1}, {"fallback:Y", 2}, {"bound:Y", 2}});
      break;
    case NativeGraphFixtureCase::kBoundSignature:
      populateChain(database, true);
      break;
    case NativeGraphFixtureCase::kActiveCycle:
      populateActiveCycle(database);
      break;
    case NativeGraphFixtureCase::kUtf8:
      populateUtf8(database);
      break;
  }
  return fixture;
}

CsrNativeGraphFixture makePropertyGraphFixture(std::uint64_t seed)
{
  CsrNativeGraphFixture fixture;
  Database& database = fixture.database();
  std::uint64_t state = seed == 0 ? 0x9e3779b97f4a7c15ULL : seed;
  const std::size_t node_count = 4 + static_cast<std::size_t>(nextRandom(state) % 5);
  std::vector<std::string> nodes;
  nodes.reserve(node_count);
  for (std::size_t index = 0; index < node_count; ++index) {
    nodes.push_back((index % 2 == 0 ? "a_long_" : "z") + std::to_string(index));
  }
  populatePortGraph(database, nodes, {}, {}, {});
  addNetArc(database, nodes[0], nodes[1], "shared");
  addNetArc(database, nodes[0], nodes[1], "shared");
  addNetArc(database, nodes[0], nodes[1], "shared", false, true, false);
  addNetArc(database, nodes[0], nodes[1], "shared", false, false, true);
  addNetArc(database, nodes[1], nodes[3], "shared");

  for (std::size_t source = 0; source < node_count; ++source) {
    for (std::size_t sink = source + 1; sink < node_count; ++sink) {
      if (sink == 2 || (source == 0 && sink == 1) || (source == 1 && sink == 3)) {
        continue;
      }
      if ((nextRandom(state) & 3U) != 0U) {
        continue;
      }
      const std::size_t copies = ((nextRandom(state) & 7U) == 0U) ? 2 : 1;
      for (std::size_t copy = 0; copy < copies; ++copy) {
        const bool disabled = (nextRandom(state) & 15U) == 0U;
        const bool loop_disabled = !disabled && (nextRandom(state) & 15U) == 0U;
        addNetArc(database, nodes[source], nodes[sink], "shared", false, disabled, loop_disabled);
      }
    }
  }

  std::map<std::string, std::size_t> indegrees;
  std::map<std::string, std::int32_t> levels;
  std::queue<std::string> ready;
  for (const auto& [name, point] : database.get_timing_point_map()) {
    static_cast<void>(point);
    std::size_t indegree = 0;
    const auto incoming = database.get_incoming_arc_list_map().find(name);
    if (incoming != database.get_incoming_arc_list_map().end()) {
      for (std::size_t arc_index : incoming->second) {
        const Arc& arc = database.get_arc_list().at(arc_index);
        if (!arc.get_is_disable_arc() && !arc.get_is_loop_disable()) {
          ++indegree;
        }
      }
    }
    indegrees.emplace(name, indegree);
    levels.emplace(name, indegree == 0 ? 1 : 0);
    if (indegree == 0) {
      ready.push(name);
      database.get_start_point_list().push_back(name);
    }
  }
  while (!ready.empty()) {
    const std::string source = ready.front();
    ready.pop();
    database.get_timing_order_list().push_back(source);
    const auto outgoing = database.get_outgoing_arc_list_map().find(source);
    if (outgoing == database.get_outgoing_arc_list_map().end()) {
      continue;
    }
    for (std::size_t arc_index : outgoing->second) {
      const Arc& arc = database.get_arc_list().at(arc_index);
      if (arc.get_is_disable_arc() || arc.get_is_loop_disable()) {
        continue;
      }
      const std::string& sink = arc.get_sink_pin();
      levels.at(sink) = std::max(levels.at(sink), levels.at(source) + 1);
      if (--indegrees.at(sink) == 0) {
        ready.push(sink);
      }
    }
  }
  for (auto& [name, point] : database.get_timing_point_map()) {
    point.set_level(levels.at(name));
    bool has_active_outgoing = false;
    const auto outgoing = database.get_outgoing_arc_list_map().find(name);
    if (outgoing != database.get_outgoing_arc_list_map().end()) {
      for (std::size_t arc_index : outgoing->second) {
        const Arc& arc = database.get_arc_list().at(arc_index);
        has_active_outgoing = has_active_outgoing || (!arc.get_is_disable_arc() && !arc.get_is_loop_disable());
      }
    }
    if (!has_active_outgoing) {
      database.get_end_point_list().push_back(name);
    }
  }
  return fixture;
}

}  // namespace ista::csr::test
