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
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "AnalysisType.hpp"
#include "Arc.hpp"
#include "CsrNativeGraphTestSupport.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "Pin.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingPathState.hpp"
#include "TimingPoint.hpp"

namespace {

using ista::AnalysisType;
using ista::Arc;
using ista::ArcType;
using ista::Database;
using ista::Instance;
using ista::PathSourceType;
using ista::Pin;
using ista::TimingArc;
using ista::TimingArcDelayState;
using ista::TimingCell;
using ista::TimingCellArc;
using ista::TimingPathState;
using ista::TimingPoint;
using ista::TransType;

static_assert(std::is_same_v<decltype(std::declval<const TimingPathState&>().get_start_point()), const std::string&>);
static_assert(noexcept(std::declval<const TimingPathState&>().get_start_point()));
static_assert(std::is_same_v<decltype(std::declval<const TimingPathState&>().get_predecessor()), const std::string&>);
static_assert(noexcept(std::declval<const TimingPathState&>().get_predecessor()));
static_assert(std::is_same_v<decltype(std::declval<const TimingPathState&>().get_clock_name()), const std::string&>);
static_assert(noexcept(std::declval<const TimingPathState&>().get_clock_name()));
static_assert(std::is_same_v<decltype(std::declval<const TimingPathState&>().get_crpr_clock_pin()), const std::string&>);
static_assert(noexcept(std::declval<const TimingPathState&>().get_crpr_clock_pin()));
using ista::csr::ArcKind;
using ista::csr::canonicalArcKeyBytes;
using ista::csr::canonicalNodeKeyBytes;
using ista::csr::CsrNativeTimingArc;
using ista::csr::CsrNativeTimingNode;
using ista::csr::CsrNativeTimingOracle;
using ista::csr::NativeGraphProjection;
using ista::csr::NodeKind;
using ista::csr::test::canonicalIndependentNativeTimingOracleBytes;
using ista::csr::test::captureDirectNativeGraphProjection;
using ista::csr::test::readNativeTimingOracle;

void require(bool condition, std::string_view message)
{
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename Function>
void requireThrows(Function function, std::string_view message)
{
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> fromHex(std::string_view hex)
{
  require(hex.size() % 2 == 0, "hex literal has odd length");
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  const auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    throw std::runtime_error("hex literal has a non-lowercase-hex character");
  };
  for (std::size_t offset = 0; offset < hex.size(); offset += 2) {
    bytes.push_back(static_cast<std::uint8_t>((nibble(hex[offset]) << 4) | nibble(hex[offset + 1])));
  }
  return bytes;
}

Pin makePin(std::string full_name, std::string local_name, std::string instance_name, bool is_port)
{
  Pin pin;
  pin.set_full_name(full_name);
  pin.set_pin_name(local_name);
  pin.set_instance_name(instance_name);
  pin.set_is_port(is_port);
  return pin;
}

void addNode(Database& database, Pin pin)
{
  const std::string full_name = pin.get_full_name();
  database.get_pin_map().emplace(full_name, std::move(pin));
  database.get_timing_point_map().emplace(full_name, TimingPoint{});
}

TimingArc makeTimingArc(std::int32_t index, TransType trigger)
{
  TimingArc arc;
  arc.set_arc_idx(index);
  arc.set_library_name("slow.lib");
  arc.set_trigger_trans_type(trigger);
  return arc;
}

TimingCellArc* installCell(Database& database)
{
  TimingCellArc cell_arc;
  cell_arc.set_source_port("A");
  cell_arc.set_sink_port("Y");
  cell_arc.get_timing_arc_list().push_back(makeTimingArc(-7, TransType::kRise));
  cell_arc.get_timing_arc_list().push_back(makeTimingArc(3, TransType::kFall));

  TimingCell cell;
  cell.set_cell_name("BUF_X1");
  cell.set_library_name("slow.lib");
  cell.get_cell_arc_list().push_back(std::move(cell_arc));
  database.get_timing_library().get_cell_map().emplace("BUF_X1", std::move(cell));
  return &database.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list().at(0);
}

std::size_t addArc(Database& database, std::string source, std::string sink, ArcType type, std::string owner, TimingCellArc* timing_cell_arc = nullptr)
{
  Arc arc;
  arc.set_source_pin(source);
  arc.set_sink_pin(sink);
  arc.set_owner_name(owner);
  arc.set_type(type);
  if (timing_cell_arc != nullptr) {
    arc.set_library_source_port("A");
    arc.set_library_sink_port("Y");
    arc.set_timing_cell_arc(timing_cell_arc);
  }
  database.get_arc_list().push_back(std::move(arc));
  return database.get_arc_list().size() - 1;
}

TimingPathState makePath(std::string start, std::string predecessor, std::size_t predecessor_arc, std::string clock_name, std::string crpr_pin, double arrival)
{
  TimingPathState path;
  path.set_arrival(arrival);
  path.set_slew(0.125);
  path.set_launch_time(0.5);
  path.set_start_point(start);
  path.set_predecessor(predecessor);
  path.set_predecessor_arc_idx(predecessor_arc);
  path.set_predecessor_arc_delay(predecessor.empty() ? 0.0 : 1.25);
  path.set_clock_name(clock_name);
  path.set_crpr_clock_pin(crpr_pin);
  path.set_trans_type(TransType::kRise);
  path.set_predecessor_trans_type(predecessor.empty() ? TransType::kNone : TransType::kFall);
  path.set_crpr_clock_trans_type(crpr_pin.empty() ? TransType::kNone : TransType::kFall);
  return path;
}

void populatePointState(Database& database)
{
  TimingPoint& input = database.get_timing_point_map().at("in");
  input.set_level(1);
  input.set_is_clock_point(true);
  input.set_arrival(0.0);
  input.set_required(8.0);
  input.set_slack(8.0);
  input.set_launch_time(0.5);
  input.set_clock_name("clk");
  input.get_data_slew_map()[AnalysisType::kMax][TransType::kRise] = -0.0;
  input.get_path_state_map()[AnalysisType::kMax][PathSourceType::kInput][TransType::kRise]["z"]
      = makePath("in", "", std::numeric_limits<std::size_t>::max(), "z", "in", 0.0);
  input.get_path_state_map()[AnalysisType::kMax][PathSourceType::kInput][TransType::kRise]["a"]
      = makePath("in", "", std::numeric_limits<std::size_t>::max(), "a", "", 0.25);

  TimingPoint& middle = database.get_timing_point_map().at("u:A");
  middle.set_level(2);
  middle.set_arrival(1.25);
  middle.set_required(7.5);
  middle.set_slack(6.25);
  middle.set_launch_time(0.5);
  middle.set_clock_name("clk");
  middle.set_predecessor("in");
  middle.set_predecessor_arc_idx(0);
  middle.get_clock_arrival_map()[AnalysisType::kMax][TransType::kRise] = 1.0;
  middle.get_clock_slew_map()[AnalysisType::kMax][TransType::kRise] = 0.1;
  middle.get_clock_predecessor_map()[AnalysisType::kMax][TransType::kRise] = "in";
  middle.get_clock_predecessor_arc_delay_map()[AnalysisType::kMax][TransType::kRise] = 1.0;
  middle.get_clock_predecessor_trans_type_map()[AnalysisType::kMax][TransType::kRise] = TransType::kFall;
  middle.get_physical_clock_arrival_map()[AnalysisType::kMin][TransType::kFall] = 1.5;
  middle.get_physical_clock_slew_map()[AnalysisType::kMin][TransType::kFall] = 0.2;
  middle.get_physical_clock_predecessor_map()[AnalysisType::kMin][TransType::kFall] = "in";
  middle.get_physical_clock_predecessor_arc_delay_map()[AnalysisType::kMin][TransType::kFall] = 1.5;
  middle.get_physical_clock_predecessor_trans_type_map()[AnalysisType::kMin][TransType::kFall] = TransType::kRise;
  middle.get_data_slew_map()[AnalysisType::kMin][TransType::kFall] = 0.3;
  middle.get_path_state_map()[AnalysisType::kMin][PathSourceType::kRegister][TransType::kFall]["clk"] = makePath("in", "in", 0, "clk", "in", 1.25);

  TimingPoint& output = database.get_timing_point_map().at("u:Y");
  output.set_level(3);
  output.set_arrival(2.5);
  output.set_required(7.0);
  output.set_slack(4.5);
  output.set_launch_time(0.5);
  output.set_clock_name("clk");
  output.set_predecessor("u:A");
  output.set_predecessor_arc_idx(1);
}

void populateArcState(Arc& arc, double base, bool has_timing_arcs)
{
  arc.set_delay(base);
  arc.set_delay_min(base - 0.25);
  arc.set_delay_max(base + 0.25);
  arc.get_trans_delay_map()[AnalysisType::kMax][TransType::kRise] = base + 0.1;
  arc.get_input_output_delay_map()[AnalysisType::kMax][TransType::kRise][TransType::kFall] = base + 0.2;
  arc.get_graph_delay_map()[AnalysisType::kMin][TransType::kFall][TransType::kRise] = base + 0.3;
  arc.get_trans_type_map()[TransType::kRise] = TransType::kFall;
  if (has_timing_arcs) {
    arc.update_timing_arc_delay(-7, AnalysisType::kMax, TransType::kRise, TransType::kFall, base + 0.4, true);
    arc.update_timing_arc_delay(3, AnalysisType::kMin, TransType::kFall, TransType::kRise, base + 0.5, false);
  }
}

std::unique_ptr<Database> makeFixture()
{
  auto database = std::make_unique<Database>();
  addNode(*database, makePin("in", "in", "", true));
  addNode(*database, makePin("u:A", "A", "u", false));
  addNode(*database, makePin("u:Y", "Y", "u", false));
  Instance instance;
  instance.set_instance_name("u");
  instance.set_cell_name("BUF_X1");
  instance.set_pin_name_list({"u:A", "u:Y"});
  instance.set_output_pin_name("u:Y");
  database->get_instance_map().emplace("u", std::move(instance));

  TimingCellArc* cell_arc = installCell(*database);
  addArc(*database, "in", "u:A", ArcType::kNet, "n0");
  addArc(*database, "u:A", "u:Y", ArcType::kCell, "u", cell_arc);
  addArc(*database, "u:A", "u:Y", ArcType::kCell, "u", cell_arc);
  database->get_timing_order_list() = {"in", "u:A", "u:Y"};
  populatePointState(*database);
  populateArcState(database->get_arc_list()[0], 1.0, false);
  populateArcState(database->get_arc_list()[1], 2.0, true);
  populateArcState(database->get_arc_list()[2], 3.0, true);
  return database;
}

bool bytesLess(const std::vector<std::uint8_t>& lhs, const std::vector<std::uint8_t>& rhs)
{
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

const CsrNativeTimingNode& findNode(const CsrNativeTimingOracle& oracle, std::string_view full_name)
{
  const auto found = std::find_if(oracle.nodes.begin(), oracle.nodes.end(), [&](const CsrNativeTimingNode& node) { return node.key.full_name == full_name; });
  if (found == oracle.nodes.end()) {
    throw std::runtime_error("expected timing node is absent");
  }
  return *found;
}

const CsrNativeTimingArc& findCellArc(const CsrNativeTimingOracle& oracle, std::uint64_t parallel_ordinal)
{
  const auto found = std::find_if(oracle.arcs.begin(), oracle.arcs.end(), [&](const CsrNativeTimingArc& arc) {
    return arc.key.base.kind == ArcKind::kCell && arc.key.parallel_ordinal == parallel_ordinal;
  });
  if (found == oracle.arcs.end()) {
    throw std::runtime_error("expected timing Arc is absent");
  }
  return *found;
}

std::vector<std::uint8_t> forbiddenNativeEncoding(const Database& database)
{
  ista::csr::CanonicalEncoder encoder;
  encoder.appendDomain("native-timing-oracle-forbidden");
  encoder.appendU64(database.get_pin_map().size());
  for (const auto& [map_name, pin] : database.get_pin_map()) {
    encoder.appendString(map_name);
    encoder.appendString(pin.get_full_name());
    encoder.appendString(pin.get_instance_name());
    encoder.appendString(pin.get_pin_name());
    encoder.appendBool(pin.get_is_port());
  }
  encoder.appendU64(database.get_arc_list().size());
  for (const Arc& arc : database.get_arc_list()) {
    encoder.appendU64(reinterpret_cast<std::uintptr_t>(&arc));
    encoder.appendString(arc.get_source_pin());
    encoder.appendString(arc.get_sink_pin());
    encoder.appendString(arc.get_owner_name());
    encoder.appendU64(static_cast<std::uint64_t>(arc.get_type()));
    encoder.appendBool(arc.get_is_clock_arc());
    encoder.appendBool(arc.get_is_disable_arc());
    encoder.appendBool(arc.get_is_loop_disable());
  }
  return encoder.bytes();
}

void verifyEmptyKnownAnswer()
{
  constexpr std::string_view kEmptyOracleHex
      = "0000000000000008697374612d63737200000000000000146e61746976652d74696d696e672d6f7261636c6500000000"
        "00000005312e302e300000000000000023697374612d6373722d6e61746976652d74696d696e672d6f7261636c652f31"
        "2e302e30000000000000000000000000000000000000000000000000";
  const auto expected = fromHex(kEmptyOracleHex);
  require(expected.size() == 124, "empty timing oracle KAT size changed");
  require(canonicalIndependentNativeTimingOracleBytes(CsrNativeTimingOracle{}) == expected, "empty timing oracle independent bytes changed");
}
void verifyRawTimingSemanticKeys(const Database& database, const CsrNativeTimingOracle& oracle)
{
  const NativeGraphProjection raw = captureDirectNativeGraphProjection(database);
  require(raw.nodes.size() == oracle.nodes.size(), "raw/timing NodeKey cardinality differs");
  for (std::size_t index = 0; index < raw.nodes.size(); ++index) {
    require(raw.nodes[index].key == oracle.nodes[index].key, "raw/timing canonical NodeKey differs");
  }
  require(raw.arcs.size() == oracle.arcs.size(), "raw/timing ArcKey cardinality differs");
  for (std::size_t index = 0; index < raw.arcs.size(); ++index) {
    require(raw.arcs[index].key == oracle.arcs[index].key, "raw/timing canonical ArcKey differs");
  }
  require(std::ranges::any_of(raw.nodes, [](const auto& node) { return node.key.kind == NodeKind::kPort; }), "raw/timing KAT lacks a port NodeKey");
  require(std::ranges::any_of(raw.nodes, [](const auto& node) { return node.key.kind == NodeKind::kInstancePin; }),
          "raw/timing KAT lacks an instance-pin NodeKey");
  require(std::ranges::any_of(raw.arcs, [](const auto& arc) { return arc.key.parallel_ordinal == 1; }), "raw/timing KAT lacks a parallel ArcKey");
}

void verifyRichRead()
{
  auto database = makeFixture();
  const auto forbidden_before = forbiddenNativeEncoding(*database);
  const CsrNativeTimingOracle oracle = readNativeTimingOracle(*database);
  verifyRawTimingSemanticKeys(*database, oracle);
  require(forbiddenNativeEncoding(*database) == forbidden_before, "timing reader changed forbidden native state");
  require(readNativeTimingOracle(*database) == oracle, "timing reader is not repeatable");
  require(oracle.schema == "ista-csr-native-timing-oracle/1.0.0", "timing oracle schema changed");
  require(oracle.native_timing_order.size() == 3, "native timing order cardinality changed");
  require(oracle.native_timing_order[0].kind == NodeKind::kPort, "port timing-order key changed");
  require(oracle.native_timing_order[1].kind == NodeKind::kInstancePin, "instance-pin timing-order key changed");

  for (std::size_t index = 1; index < oracle.nodes.size(); ++index) {
    require(bytesLess(canonicalNodeKeyBytes(oracle.nodes[index - 1].key), canonicalNodeKeyBytes(oracle.nodes[index].key)),
            "timing nodes are not canonical-key sorted");
  }
  for (std::size_t index = 1; index < oracle.arcs.size(); ++index) {
    require(bytesLess(canonicalArcKeyBytes(oracle.arcs[index - 1].key), canonicalArcKeyBytes(oracle.arcs[index].key)),
            "timing Arcs are not canonical-key sorted");
  }

  const CsrNativeTimingNode& input = findNode(oracle, "in");
  require(input.level == 1 && input.is_clock_point, "node scalar/clock fields changed");
  require(input.clock_name == std::optional<std::string>{"clk"}, "optional node clock name changed");
  require(std::signbit(*input.data_slews[0][0]), "signed-zero data slew was not observed");
  require(input.paths.size() == 2 && input.paths[0].clock_tag == "a" && input.paths[1].clock_tag == "z", "path clock-tag byte sorting changed");
  require(input.paths[1].start_node.full_name == "in" && input.paths[1].crpr_node && input.paths[1].crpr_node->full_name == "in",
          "path start/CRPR semantic conversion changed");

  const CsrNativeTimingNode& middle = findNode(oracle, "u:A");
  require(middle.predecessor_node && middle.predecessor_node->full_name == "in", "scalar predecessor NodeKey changed");
  require(middle.predecessor_arc && middle.predecessor_arc->base.kind == ArcKind::kNet, "scalar predecessor ArcKey changed");
  require(middle.effective_clock.predecessor_nodes[0][0] && middle.effective_clock.predecessor_nodes[0][0]->full_name == "in",
          "effective clock predecessor conversion changed");
  require(middle.paths.size() == 1 && middle.paths[0].predecessor_arc && middle.paths[0].predecessor_arc->base.kind == ArcKind::kNet,
          "path predecessor conversion changed");

  const CsrNativeTimingArc& first = findCellArc(oracle, 0);
  const CsrNativeTimingArc& parallel = findCellArc(oracle, 1);
  require(first.key.base.source.full_name == "u:A" && first.key.base.sink.full_name == "u:Y", "cell Arc endpoint keys changed");
  require(parallel.key.parallel_ordinal == 1, "parallel Arc ordinal changed");
  require(first.timing_arcs.size() == 2 && first.timing_arcs[0].timing_arc_index == -7 && first.timing_arcs[1].timing_arc_index == 3,
          "signed timing-arc index sorting changed");
  require(first.timing_arcs[0].lanes[0][0][1] && first.timing_arcs[0].lanes[0][0][1]->is_fallback, "timing-arc fallback provenance changed");
  require(first.timing_arcs[1].lanes[1][1][0] && !first.timing_arcs[1].lanes[1][1][0]->is_fallback, "dynamic timing-arc provenance changed");
  require(!canonicalIndependentNativeTimingOracleBytes(oracle).empty(), "rich independent timing bytes are empty");

  CsrNativeTimingOracle positive_zero = oracle;
  const auto positive_input
      = std::find_if(positive_zero.nodes.begin(), positive_zero.nodes.end(), [](const CsrNativeTimingNode& node) { return node.key.full_name == "in"; });
  require(positive_input != positive_zero.nodes.end() && positive_input->data_slews[0][0], "signed-zero byte canary lane is absent");
  positive_input->data_slews[0][0] = 0.0;
  require(!std::signbit(*positive_input->data_slews[0][0]), "signed-zero byte canary did not become positive zero");
  require(canonicalIndependentNativeTimingOracleBytes(positive_zero) != canonicalIndependentNativeTimingOracleBytes(oracle),
          "independent timing bytes collapsed signed zero");
}

template <typename Mutate>
void requireReaderMutation(Mutate mutate, std::string_view label)
{
  auto database = makeFixture();
  const auto before = canonicalIndependentNativeTimingOracleBytes(readNativeTimingOracle(*database));
  mutate(*database);
  const auto after = canonicalIndependentNativeTimingOracleBytes(readNativeTimingOracle(*database));
  require(after != before, label);
}

void verifyFamilyMutationCanaries()
{
  requireReaderMutation([](Database& database) { std::reverse(database.get_timing_order_list().begin(), database.get_timing_order_list().end()); },
                        "timing-order mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_level(20); }, "node level mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_is_clock_point(true); },
                        "node is-clock mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_arrival(9.0); }, "node arrival mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_required(9.0); }, "node required mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_slack(9.0); }, "node slack mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_launch_time(9.0); }, "node launch mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_clock_name("other"); },
                        "node clock-name mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_predecessor("u:Y"); },
                        "node predecessor-name mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").set_predecessor_arc_idx(2); },
                        "node predecessor-Arc mutation was not observed");

  requireReaderMutation(
      [](Database& database) { database.get_timing_point_map().at("u:A").get_clock_arrival_map()[AnalysisType::kMax][TransType::kRise] = 9.0; },
      "effective arrival mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").get_clock_slew_map()[AnalysisType::kMax][TransType::kRise] = 9.0; },
                        "effective slew mutation was not observed");
  requireReaderMutation(
      [](Database& database) { database.get_timing_point_map().at("u:A").get_clock_predecessor_map()[AnalysisType::kMax][TransType::kRise] = "u:Y"; },
      "effective predecessor mutation was not observed");
  requireReaderMutation(
      [](Database& database) { database.get_timing_point_map().at("u:A").get_clock_predecessor_arc_delay_map()[AnalysisType::kMax][TransType::kRise] = 9.0; },
      "effective predecessor-delay mutation was not observed");
  requireReaderMutation(
      [](Database& database) {
        database.get_timing_point_map().at("u:A").get_clock_predecessor_trans_type_map()[AnalysisType::kMax][TransType::kRise] = TransType::kRise;
      },
      "effective predecessor-transition mutation was not observed");

  requireReaderMutation(
      [](Database& database) { database.get_timing_point_map().at("u:A").get_physical_clock_arrival_map()[AnalysisType::kMin][TransType::kFall] = 9.0; },
      "physical arrival mutation was not observed");
  requireReaderMutation(
      [](Database& database) { database.get_timing_point_map().at("u:A").get_physical_clock_slew_map()[AnalysisType::kMin][TransType::kFall] = 9.0; },
      "physical slew mutation was not observed");
  requireReaderMutation(
      [](Database& database) { database.get_timing_point_map().at("u:A").get_physical_clock_predecessor_map()[AnalysisType::kMin][TransType::kFall] = "u:Y"; },
      "physical predecessor mutation was not observed");
  requireReaderMutation(
      [](Database& database) {
        database.get_timing_point_map().at("u:A").get_physical_clock_predecessor_arc_delay_map()[AnalysisType::kMin][TransType::kFall] = 9.0;
      },
      "physical predecessor-delay mutation was not observed");
  requireReaderMutation(
      [](Database& database) {
        database.get_timing_point_map().at("u:A").get_physical_clock_predecessor_trans_type_map()[AnalysisType::kMin][TransType::kFall] = TransType::kFall;
      },
      "physical predecessor-transition mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_timing_point_map().at("u:A").get_data_slew_map()[AnalysisType::kMin][TransType::kFall] = 9.0; },
                        "data-slew mutation was not observed");
  requireReaderMutation(
      [](Database& database) {
        database.get_timing_point_map().at("u:A").get_path_state_map()[AnalysisType::kMin][PathSourceType::kRegister][TransType::kFall].at("clk").set_arrival(
            9.0);
      },
      "path-state mutation was not observed");

  requireReaderMutation([](Database& database) { database.get_arc_list()[1].set_delay(9.0); }, "Arc delay mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_arc_list()[1].set_delay_min(9.0); }, "Arc min-delay mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_arc_list()[1].set_delay_max(9.0); }, "Arc max-delay mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_arc_list()[1].get_trans_delay_map()[AnalysisType::kMax][TransType::kRise] = 9.0; },
                        "Arc transition-delay mutation was not observed");
  requireReaderMutation(
      [](Database& database) { database.get_arc_list()[1].get_input_output_delay_map()[AnalysisType::kMax][TransType::kRise][TransType::kFall] = 9.0; },
      "Arc input-output mutation was not observed");
  requireReaderMutation(
      [](Database& database) { database.get_arc_list()[1].get_graph_delay_map()[AnalysisType::kMin][TransType::kFall][TransType::kRise] = 9.0; },
      "Arc graph-delay mutation was not observed");
  requireReaderMutation([](Database& database) { database.get_arc_list()[1].get_trans_type_map()[TransType::kRise] = TransType::kRise; },
                        "Arc transition-type mutation was not observed");
  requireReaderMutation(
      [](Database& database) { database.get_arc_list()[1].update_timing_arc_delay(-7, AnalysisType::kMax, TransType::kRise, TransType::kFall, 99.0, false); },
      "timing-arc state mutation was not observed");
}

void verifyNegativeContracts()
{
  {
    auto database = makeFixture();
    database->get_timing_point_map().at("u:A").set_predecessor("missing");
    requireThrows([&] { static_cast<void>(readNativeTimingOracle(*database)); }, "stale scalar predecessor was accepted");
  }
  {
    auto database = makeFixture();
    database->get_timing_point_map().at("u:A").set_predecessor_arc_idx(99);
    requireThrows([&] { static_cast<void>(readNativeTimingOracle(*database)); }, "out-of-range predecessor Arc rank was accepted");
  }
  {
    auto database = makeFixture();
    database->get_timing_point_map().at("u:A").get_data_slew_map()[static_cast<AnalysisType>(99)][TransType::kRise] = 1.0;
    requireThrows([&] { static_cast<void>(readNativeTimingOracle(*database)); }, "unknown native enum was accepted");
  }
  {
    auto database = makeFixture();
    database->get_arc_list()[1].get_timing_cell_arc()->get_timing_arc_list()[1].set_arc_idx(-7);
    requireThrows([&] { static_cast<void>(readNativeTimingOracle(*database)); }, "duplicate signed timing-arc index was accepted");
  }
  {
    auto database = makeFixture();
    Arc& arc = database->get_arc_list()[1];
    auto& escaped_delay_map = arc.get_timing_arc_delay_map();
    arc.update_timing_arc_delay(-7, AnalysisType::kMin, TransType::kRise, TransType::kRise, 99.0, true);
    escaped_delay_map.at(-7).at(AnalysisType::kMin).at(TransType::kRise).erase(TransType::kRise);
    requireThrows([&] { static_cast<void>(readNativeTimingOracle(*database)); }, "fallback key outside delay-map subset was accepted");
  }
  {
    auto database = makeFixture();
    database->get_timing_point_map().at("u:A").set_arrival(std::numeric_limits<double>::quiet_NaN());
    requireThrows([&] { static_cast<void>(readNativeTimingOracle(*database)); }, "NaN node value was accepted");
  }
  {
    auto database = makeFixture();
    database->get_timing_point_map().at("u:A").get_path_state_map()[AnalysisType::kMin][PathSourceType::kRegister][TransType::kFall].at("clk").set_start_point(
        "missing");
    requireThrows([&] { static_cast<void>(readNativeTimingOracle(*database)); }, "stale path startpoint was accepted");
  }
}

}  // namespace

int main()
{
  try {
    verifyEmptyKnownAnswer();
    verifyRichRead();
    verifyFamilyMutationCanaries();
    verifyNegativeContracts();
  } catch (const std::exception& error) {
    std::cerr << "CSR native timing oracle contract failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
