// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Arc.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "Net.hpp"
#include "Pin.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingCellPort.hpp"
#include "TimingCheckArc.hpp"
#include "TimingClock.hpp"
#include "TimingPoint.hpp"
#include "TimingPortConstraint.hpp"
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

void addPort(Database& database, const std::string& name, ista::PinDirection direction, const std::string& net_name)
{
  database.get_pin_map().emplace(name, makePin(name, name, {}, net_name, direction, true));
  database.get_timing_point_map().emplace(name, TimingPoint{});
}

void addInstancePin(Database& database, const std::string& instance, const std::string& local_name, ista::PinDirection direction,
                    const std::string& net_name = {})
{
  const std::string full_name = instance + ":" + local_name;
  database.get_pin_map().emplace(full_name, makePin(full_name, local_name, instance, net_name, direction, false));
  database.get_timing_point_map().emplace(full_name, TimingPoint{});
}

void addNet(Database& database, const std::string& name, std::vector<std::string> pins, std::vector<std::string> drivers, std::vector<std::string> loads)
{
  Net net;
  net.set_net_name(name);
  net.set_pin_name_list(std::move(pins));
  net.set_driver_pin_list(std::move(drivers));
  net.set_load_pin_list(std::move(loads));
  if (!net.get_driver_pin_list().empty())
    net.set_driver_pin(net.get_driver_pin_list().front());
  database.get_net_map().emplace(name, std::move(net));
}

std::size_t addArc(Database& database, const std::string& source, const std::string& sink, ArcType kind, const std::string& owner, bool is_clock = false,
                   bool disabled = false, bool loop_disabled = false)
{
  Arc arc;
  arc.set_arc_name(owner + ":" + source + "->" + sink);
  arc.set_source_pin(source);
  arc.set_sink_pin(sink);
  arc.set_owner_name(owner);
  arc.set_type(kind);
  arc.set_is_clock_arc(is_clock);
  arc.set_is_disable_arc(disabled);
  arc.set_is_loop_disable(loop_disabled);
  database.get_arc_list().push_back(std::move(arc));
  const std::size_t rank = database.get_arc_list().size() - 1;
  database.get_outgoing_arc_list_map()[source].push_back(rank);
  database.get_incoming_arc_list_map()[sink].push_back(rank);
  return rank;
}

void addNetArc(Database& database, const std::string& source, const std::string& sink, const std::string& net, bool disabled = false)
{
  static_cast<void>(addArc(database, source, sink, ArcType::kNet, net, false, disabled));
}

TimingTable makePresentEmptyTable()
{
  TimingTable table;
  table.set_variable_type1(TimingTableVariableType::kNone);
  table.set_variable_type2(TimingTableVariableType::kNone);
  return table;
}

TimingTable makeConstantTable(double value)
{
  TimingTable table;
  table.set_value_list({value});
  return table;
}

TimingTable makeOneDimensionalTable()
{
  TimingTable table;
  table.set_variable_type1(TimingTableVariableType::kInputTransition);
  table.set_axis_list({{0.1, 0.3}});
  table.set_value_list({0.2, 0.4});
  return table;
}

TimingTable makeTwoDimensionalTable()
{
  TimingTable table;
  table.set_variable_type1(TimingTableVariableType::kInputTransition);
  table.set_variable_type2(TimingTableVariableType::kOutputCapacitance);
  table.set_axis_list({{0.1, 0.3}, {0.2, 0.5}});
  table.set_value_list({0.11, 0.12, 0.21, 0.22});
  return table;
}

TimingArc makeChild(std::int32_t index, TransType trigger, std::string condition)
{
  TimingArc child;
  child.set_library_name("slow.lib");
  child.set_arc_idx(index);
  child.set_sense(TimingArcSense::kPositive);
  child.set_trigger_trans_type(trigger);
  child.set_check_trans_type(TransType::kFall);
  child.set_sdf_cond(condition);
  child.set_time_unit_scale(1.0);
  child.set_cap_unit_scale(1.0);
  child.set_slew_derate(0.9);
  return child;
}

TimingCellPort makeCellPort(const std::string& name, bool input, bool output, bool clock, double capacitance)
{
  TimingCellPort port;
  port.set_port_name(name);
  port.set_is_input(input);
  port.set_is_output(output);
  port.set_is_clock(clock);
  port.set_capacitance(capacitance);
  if (input) {
    port.get_trans_capacitance_map()[AnalysisType::kMax][TransType::kRise] = capacitance + 0.01;
    port.get_trans_capacitance_map()[AnalysisType::kMin][TransType::kFall] = capacitance + 0.02;
  }
  return port;
}

TimingCheckArc makeLibraryCheck()
{
  TimingArc child = makeChild(31, TransType::kFall, "RESET_B");
  child.get_check_table_map().emplace(TransType::kRise, makeOneDimensionalTable());

  TimingCheckArc check;
  check.set_clock_port("CK");
  check.set_data_port("D");
  check.set_check_type(TimingCheckType::kSetup);
  check.set_check_time(0.15);
  check.set_clock_trans_type(TransType::kRise);
  check.get_timing_arc_list().push_back(std::move(child));
  return check;
}

void addSequentialLibrary(Database& database)
{
  TimingCellArc c2q;
  c2q.set_source_port("CK");
  c2q.set_sink_port("Q");
  c2q.set_delay(0.12);
  c2q.set_delay_min(0.08);
  c2q.set_delay_max(0.18);
  c2q.set_is_clock_arc(true);
  TimingArc c2q_child = makeChild(11, TransType::kFall, "ENABLE");
  c2q_child.get_delay_table_map().emplace(TransType::kRise, makePresentEmptyTable());
  c2q_child.get_slew_table_map().emplace(TransType::kFall, makeConstantTable(0.07));
  c2q.get_timing_arc_list().push_back(std::move(c2q_child));

  TimingCellArc data_to_q;
  data_to_q.set_source_port("D");
  data_to_q.set_sink_port("Q");
  data_to_q.set_delay(0.2);
  data_to_q.set_delay_min(0.1);
  data_to_q.set_delay_max(0.3);
  TimingArc data_child = makeChild(12, TransType::kRise, {});
  data_child.get_delay_table_map().emplace(TransType::kRise, makeTwoDimensionalTable());
  data_to_q.get_timing_arc_list().push_back(std::move(data_child));

  TimingCell cell;
  cell.set_cell_name("DFF_X1");
  cell.set_library_name("slow.lib");
  cell.set_is_sequential(true);
  cell.get_port_map().emplace("CK", makeCellPort("CK", true, false, true, 0.12));
  cell.get_port_map().emplace("D", makeCellPort("D", true, false, false, 0.2));
  cell.get_port_map().emplace("Q", makeCellPort("Q", false, true, false, 0.0));
  cell.get_cell_arc_list().push_back(std::move(c2q));
  cell.get_cell_arc_list().push_back(std::move(data_to_q));
  cell.get_check_arc_list().push_back(makeLibraryCheck());
  database.get_timing_library().get_cell_map().emplace("DFF_X1", std::move(cell));
}

TimingCheckArc makeInstanceCheck(const TimingCheckArc& library_check, const std::string& instance)
{
  TimingCheckArc check = library_check;
  check.set_clock_port(instance + ":CK");
  check.set_data_port(instance + ":D");
  return check;
}

void addSequentialInstance(Database& database, const std::string& name)
{
  const TimingCell& cell = database.get_timing_library().get_cell_map().at("DFF_X1");
  Instance instance;
  instance.set_instance_name(name);
  instance.set_cell_name("DFF_X1");
  instance.set_pin_name_list({name + ":CK", name + ":D", name + ":Q"});
  instance.set_clock_pin_name(name + ":CK");
  instance.set_data_pin_name(name + ":D");
  instance.set_output_pin_name(name + ":Q");
  instance.set_is_sequential(true);
  instance.get_check_arc_list().push_back(makeInstanceCheck(cell.get_check_arc_list().front(), name));
  database.get_instance_map().emplace(name, std::move(instance));
}

void addBoundCellArc(Database& database, const std::string& instance, std::size_t cell_arc_index)
{
  TimingCell& cell = database.get_timing_library().get_cell_map().at("DFF_X1");
  TimingCellArc& cell_arc = cell.get_cell_arc_list().at(cell_arc_index);
  const std::string source = instance + ":" + cell_arc.get_source_port();
  const std::string sink = instance + ":" + cell_arc.get_sink_port();
  const std::size_t rank = addArc(database, source, sink, ArcType::kCell, instance, cell_arc.get_is_clock_arc(), cell_arc.get_is_disable_arc());
  Arc& arc = database.get_arc_list().at(rank);
  arc.set_library_source_port(cell_arc.get_source_port());
  arc.set_library_sink_port(cell_arc.get_sink_port());
  arc.set_timing_cell_arc(&cell_arc);
}

void addRawConstraints(Database& database)
{
  TimingClock clock;
  clock.set_clock_name("embedded_clk");
  clock.set_source_list({"clk", "clk", "unresolved_clock_source"});
  clock.set_period(10.0);
  clock.set_rise_edge(-0.5);
  clock.set_fall_edge(0.0);
  clock.set_is_propagated(true);
  database.get_timing_constraint().get_clock_map().emplace("a_clock_key", std::move(clock));

  TimingClock empty_clock;
  empty_clock.set_clock_name("empty_embedded");
  empty_clock.set_period(-2.0);
  empty_clock.set_rise_edge(0.0);
  empty_clock.set_fall_edge(-1.0);
  database.get_timing_constraint().get_clock_map().emplace("z_empty_clock", std::move(empty_clock));

  TimingPortConstraint input;
  input.set_port_name("embedded_din");
  input.set_clock_name("a_clock_key");
  input.set_input_delay_max(1.5);
  input.set_input_delay_min(-0.25);
  input.set_input_transition(0.04);
  input.set_has_input_delay_max(true);
  input.set_has_input_delay_min(true);
  input.set_has_input_transition(true);
  database.get_timing_constraint().get_port_constraint_map().emplace("din", std::move(input));

  TimingPortConstraint output;
  output.set_port_name("embedded_out");
  output.set_clock_name("a_clock_key");
  output.set_output_delay_max(2.0);
  output.set_output_delay_min(-0.5);
  output.set_load(0.3);
  output.set_has_output_delay_max(true);
  output.set_has_output_delay_min(true);
  output.set_has_load(true);
  database.get_timing_constraint().get_port_constraint_map().emplace("out", std::move(output));

  TimingPortConstraint unresolved;
  unresolved.set_port_name("embedded_missing_port");
  database.get_timing_constraint().get_port_constraint_map().emplace("unresolved_port", std::move(unresolved));
  database.get_timing_constraint().get_case_analysis_map().emplace("din", true);
  database.get_timing_constraint().get_case_analysis_map().emplace("unresolved_case", false);
}

void populateChain(Database& database)
{
  addSequentialLibrary(database);
  addPort(database, "clk", ista::PinDirection::kInput, "clk_net");
  addPort(database, "din", ista::PinDirection::kInput, "din_net");
  addPort(database, "out", ista::PinDirection::kOutput, "out_net");
  addInstancePin(database, "ff0", "CK", ista::PinDirection::kInput, "clk_net");
  addInstancePin(database, "ff0", "D", ista::PinDirection::kInput, "din_net");
  addInstancePin(database, "ff0", "Q", ista::PinDirection::kOutput, "div_net");
  addInstancePin(database, "ff1", "CK", ista::PinDirection::kInput, "div_net");
  addInstancePin(database, "ff1", "D", ista::PinDirection::kInput);
  addInstancePin(database, "ff1", "Q", ista::PinDirection::kOutput, "out_net");
  addSequentialInstance(database, "ff0");
  addSequentialInstance(database, "ff1");

  addNet(database, "clk_net", {"clk", "ff0:CK"}, {"clk"}, {"ff0:CK"});
  addNet(database, "din_net", {"din", "ff0:D"}, {"din"}, {"ff0:D"});
  addNet(database, "div_net", {"ff0:Q", "ff1:CK"}, {"ff0:Q"}, {"ff1:CK"});
  addNet(database, "out_net", {"ff1:Q", "out"}, {"ff1:Q"}, {"out"});

  addNetArc(database, "clk", "ff0:CK", "clk_net");
  addNetArc(database, "din", "ff0:D", "din_net");
  addBoundCellArc(database, "ff0", 0);
  addBoundCellArc(database, "ff0", 1);
  addNetArc(database, "ff0:Q", "ff1:CK", "div_net", true);
  addBoundCellArc(database, "ff1", 0);
  addBoundCellArc(database, "ff1", 1);
  addNetArc(database, "ff1:Q", "out", "out_net");

  database.get_start_point_list() = {"clk", "din", "ff0:CK", "ff1:CK", "ff1:D"};
  database.get_end_point_list() = {"ff0:D", "ff1:D", "out"};
  addRawConstraints(database);
}

void populatePortGraph(Database& database, const std::vector<std::string>& names, const std::vector<std::pair<std::size_t, std::size_t>>& edges)
{
  for (std::size_t index = 0; index < names.size(); ++index)
    addPort(database, names[index], index == 0 ? ista::PinDirection::kInput : ista::PinDirection::kOutput, "shared");
  std::vector<std::string> loads(names.begin() + 1, names.end());
  addNet(database, "shared", names, {names.front()}, loads);
  for (const auto& [source, sink] : edges)
    addNetArc(database, names.at(source), names.at(sink), "shared");
  database.get_start_point_list() = {names.front()};
  database.get_end_point_list() = std::move(loads);
}

void populateDiamond(Database& database)
{
  populatePortGraph(database, {"root", "left", "right", "sink"}, {{0, 1}, {0, 2}, {0, 3}});

  TimingClock clock;
  clock.set_clock_name("diamond_clock");
  clock.set_source_list({"root"});
  clock.set_period(4.0);
  clock.set_fall_edge(2.0);
  database.get_timing_constraint().get_clock_map().emplace("diamond_key", std::move(clock));
}

std::uint64_t nextRandom(std::uint64_t& state)
{
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

void addBufferLibrary(Database& database)
{
  TimingCellArc arc;
  arc.set_source_port("A");
  arc.set_sink_port("Y");
  arc.set_delay(0.05);
  arc.set_delay_min(0.04);
  arc.set_delay_max(0.06);

  TimingCell cell;
  cell.set_cell_name("BUF_X1");
  cell.set_library_name("slow.lib");
  cell.get_port_map().emplace("A", makeCellPort("A", true, false, false, 0.1));
  cell.get_port_map().emplace("Y", makeCellPort("Y", false, true, false, 0.0));
  cell.get_cell_arc_list().push_back(std::move(arc));
  database.get_timing_library().get_cell_map().emplace("BUF_X1", std::move(cell));
}

void addBufferInstance(Database& database, const std::string& name, const std::string& input_net, const std::string& output_net)
{
  addInstancePin(database, name, "A", ista::PinDirection::kInput, input_net);
  addInstancePin(database, name, "Y", ista::PinDirection::kOutput, output_net);

  Instance instance;
  instance.set_instance_name(name);
  instance.set_cell_name("BUF_X1");
  instance.set_pin_name_list({name + ":A", name + ":Y"});
  instance.set_output_pin_name(name + ":Y");
  database.get_instance_map().emplace(name, std::move(instance));
}

void addBoundBufferArc(Database& database, const std::string& instance)
{
  TimingCell& cell = database.get_timing_library().get_cell_map().at("BUF_X1");
  TimingCellArc& cell_arc = cell.get_cell_arc_list().front();
  const std::string source = instance + ":A";
  const std::string sink = instance + ":Y";
  const std::size_t rank = addArc(database, source, sink, ArcType::kCell, instance);
  Arc& arc = database.get_arc_list().at(rank);
  arc.set_library_source_port(cell_arc.get_source_port());
  arc.set_library_sink_port(cell_arc.get_sink_port());
  arc.set_timing_cell_arc(&cell_arc);
}

void populateActiveCycle(Database& database)
{
  addBufferLibrary(database);
  addBufferInstance(database, "u0", "n1", "n0");
  addBufferInstance(database, "u1", "n0", "n1");
  addNet(database, "n0", {"u0:Y", "u1:A"}, {"u0:Y"}, {"u1:A"});
  addNet(database, "n1", {"u1:Y", "u0:A"}, {"u1:Y"}, {"u0:A"});
  addBoundBufferArc(database, "u0");
  addNetArc(database, "u0:Y", "u1:A", "n0");
  addBoundBufferArc(database, "u1");
  addNetArc(database, "u1:Y", "u0:A", "n1");
}

void disableBufferCellArcs(Database& database)
{
  TimingCellArc& cell_arc = database.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list().front();
  cell_arc.set_is_disable_arc(true);
  for (Arc& arc : database.get_arc_list())
    if (arc.get_type() == ArcType::kCell)
      arc.set_is_disable_arc(true);
}

void populateProperty(Database& database, std::uint64_t seed)
{
  if ((seed & 1U) != 0U) {
    populateActiveCycle(database);
    return;
  }

  std::uint64_t state = seed == 0 ? 0x9e3779b97f4a7c15ULL : seed;
  const std::size_t count = 4 + static_cast<std::size_t>(nextRandom(state) % 4);
  std::vector<std::string> names;
  names.reserve(count);
  for (std::size_t index = 0; index < count; ++index)
    names.push_back("p" + std::to_string(index));

  std::vector<std::pair<std::size_t, std::size_t>> edges;
  for (std::size_t sink = 1; sink < count; ++sink)
    edges.emplace_back(0, sink);
  populatePortGraph(database, names, edges);
}

}  // namespace

CsrReferenceInputNativeFixture::CsrReferenceInputNativeFixture() : _database(std::make_unique<Database>())
{
}

CsrReferenceInputNativeFixture::~CsrReferenceInputNativeFixture() = default;
CsrReferenceInputNativeFixture::CsrReferenceInputNativeFixture(CsrReferenceInputNativeFixture&&) noexcept = default;
CsrReferenceInputNativeFixture& CsrReferenceInputNativeFixture::operator=(CsrReferenceInputNativeFixture&&) noexcept = default;

Database& CsrReferenceInputNativeFixture::database()
{
  return *_database;
}

const Database& CsrReferenceInputNativeFixture::database() const
{
  return *_database;
}

CsrReferenceInputNativeFixture makeReferenceNativeFixture(ReferenceNativeFixtureCase fixture_case)
{
  CsrReferenceInputNativeFixture fixture;
  switch (fixture_case) {
    case ReferenceNativeFixtureCase::kEmpty:
      break;
    case ReferenceNativeFixtureCase::kChain:
      populateChain(fixture.database());
      break;
    case ReferenceNativeFixtureCase::kDiamond:
      populateDiamond(fixture.database());
      break;
    case ReferenceNativeFixtureCase::kSingleton:
      populatePortGraph(fixture.database(), {"only"}, {});
      break;
    case ReferenceNativeFixtureCase::kParallel:
      populateActiveCycle(fixture.database());
      fixture.database().get_arc_list().back().set_is_loop_disable(true);
      addBoundBufferArc(fixture.database(), "u0");
      break;
    case ReferenceNativeFixtureCase::kLoopDisabled:
      populateActiveCycle(fixture.database());
      fixture.database().get_arc_list().back().set_is_loop_disable(true);
      break;
    case ReferenceNativeFixtureCase::kDisabled:
      populateActiveCycle(fixture.database());
      disableBufferCellArcs(fixture.database());
      break;
    case ReferenceNativeFixtureCase::kMalformedAdjacency:
      populateDiamond(fixture.database());
      fixture.database().get_incoming_arc_list_map().at("sink").pop_back();
      break;
  }
  return fixture;
}

CsrReferenceInputNativeFixture makeReferencePropertyFixture(std::uint64_t seed)
{
  CsrReferenceInputNativeFixture fixture;
  populateProperty(fixture.database(), seed);
  return fixture;
}

}  // namespace ista::csr::test
