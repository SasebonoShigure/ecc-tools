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
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "CsrRealDifferentialWire.hpp"

namespace ista::csr::real_diff {
namespace {

void require(bool condition, std::string_view message)
{
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename Work>
void expectReject(std::string_view name, Work work)
{
  bool rejected = false;
  try {
    work();
  } catch (const std::exception&) {
    rejected = true;
  }
  require(rejected, std::string(name) + " was accepted");
}

NodeKey port(std::string name)
{
  return NodeKey{NodeKind::kPort, std::move(name), {}, {}};
}

ArcKey netArc(const NodeKey& source, const NodeKey& sink, std::uint64_t ordinal = 0)
{
  ArcKey key;
  key.base.source = source;
  key.base.sink = sink;
  key.base.kind = ArcKind::kNet;
  key.base.owner_name = "wire_net";
  key.parallel_ordinal = ordinal;
  return key;
}

NeutralNode baseNode(const NodeKey& key, std::int32_t level)
{
  NeutralNode node;
  node.key = key;
  node.level = level;
  node.arrival = level == 0 ? -0.0 : 4.0;
  node.required = level == 0 ? std::numeric_limits<double>::infinity() : 8.0;
  node.slack = level == 0 ? std::numeric_limits<double>::infinity() : 4.0;
  node.launch = 0.0;
  return node;
}

NeutralWire richWire()
{
  const NodeKey source = port("a");
  const NodeKey sink = port("z");
  const ArcKey arc_key = netArc(source, sink);

  NeutralNode first = baseNode(source, 0);
  first.is_clock_point = true;
  first.clock_name = "vclk";
  first.effective_clock.arrivals[0][0] = 0.0;
  first.effective_clock.slews[0][0] = 0.02;
  first.physical_clock.arrivals[0][0] = 0.0;
  first.physical_clock.slews[0][0] = 0.02;
  first.data_slews[0][0] = 0.03;

  NeutralNode second = baseNode(sink, 1);
  second.clock_name = "vclk";
  second.predecessor_node = source;
  second.predecessor_arc = arc_key;
  second.data_slews[0][0] = 0.04;
  second.effective_clock.arrivals[0][0] = 0.1;
  second.effective_clock.slews[0][0] = 0.02;
  second.effective_clock.predecessor_nodes[0][0] = source;
  second.effective_clock.predecessor_arc_delays[0][0] = 0.1;
  second.effective_clock.predecessor_transitions[0][0] = Transition::kRise;
  NeutralPath path;
  path.analysis = AnalysisMode::kMax;
  path.source = PathSource::kInput;
  path.lane_transition = Transition::kRise;
  path.clock_tag = "vclk";
  path.arrival = 4.0;
  path.slew = 0.04;
  path.start_node = source;
  path.predecessor_node = source;
  path.predecessor_arc = arc_key;
  path.predecessor_arc_delay = 4.0;
  path.clock_name = "vclk";
  path.state_transition = Transition::kRise;
  path.predecessor_transition = Transition::kRise;
  second.paths.push_back(path);

  NeutralArc arc;
  arc.key = arc_key;
  arc.delay = 4.0;
  arc.min_delay = 3.0;
  arc.max_delay = 5.0;
  arc.transition_delays[0][0] = 0.04;
  arc.input_output_delays[0][0][0] = 4.0;
  arc.graph_delays[0][0][0] = 4.0;
  arc.transition_types[0] = Transition::kRise;
  NeutralArcEntry child;
  child.timing_arc_index = -7;
  child.lanes[0][0][0] = NeutralArcDelay{4.0, false};
  arc.timing_arcs.push_back(child);

  NeutralWire wire;
  wire.timing_order = {source, sink};
  wire.out_rows = {{source, {arc_key}}, {sink, {}}};
  wire.in_rows = {{source, {}}, {sink, {arc_key}}};
  wire.nodes = {std::move(first), std::move(second)};
  wire.arcs = {std::move(arc)};
  return wire;
}

NeutralWire nearTieWire()
{
  NeutralWire wire = richWire();
  NeutralPath second = wire.nodes[1].paths.front();
  second.lane_transition = Transition::kFall;
  second.state_transition = Transition::kFall;
  second.predecessor_transition = Transition::kFall;
  second.arrival += 5.0e-8;
  wire.nodes[1].paths.insert(wire.nodes[1].paths.begin(), std::move(second));
  return wire;
}

NeutralWire parallelWire()
{
  NeutralWire wire = richWire();
  NeutralArc copy = wire.arcs.front();
  copy.key.parallel_ordinal = 1;
  copy.delay = 4.25;
  wire.arcs.push_back(copy);
  wire.out_rows[0].arcs.push_back(copy.key);
  wire.in_rows[1].arcs.push_back(copy.key);
  return wire;
}

NeutralWire disabledWire()
{
  NeutralWire wire = richWire();
  wire.arcs[0].graph_delays = {};
  wire.arcs[0].timing_arcs[0].lanes[0][0][0] = NeutralArcDelay{4.0, true};
  wire.nodes[1].arrival = -std::numeric_limits<double>::infinity();
  wire.nodes[1].required = std::numeric_limits<double>::infinity();
  wire.nodes[1].slack = std::numeric_limits<double>::infinity();
  wire.nodes[1].paths.clear();
  wire.nodes[1].data_slews = {};
  return wire;
}

NeutralWire canaryWire()
{
  NeutralWire wire = richWire();
  wire.nodes[0].is_clock_point = false;
  wire.nodes[1].arrival = -987654.25;
  wire.nodes[1].required = 987654.25;
  wire.arcs[0].graph_delays[0][0][0] = NeutralArcDelay{}.delay - 12345.0;
  wire.arcs[0].timing_arcs[0].lanes[0][0][0] = NeutralArcDelay{-54321.0, true};
  return wire;
}

void knownAnswer(std::string_view name, const NeutralWire& expected)
{
  const std::string first = encodeNeutralWire(expected);
  require(!first.empty(), std::string(name) + " encoded empty");
  const NeutralWire decoded = decodeNeutralWire(first);
  require(decoded == expected, std::string(name) + " roundtrip differs");
  require(encodeNeutralWire(decoded) == first, std::string(name) + " canonical bytes changed");
}

void validateDisabledWitness(const NeutralWire& wire)
{
  const NeutralArc& arc = wire.arcs.front();
  require(std::isfinite(arc.delay) && std::isfinite(arc.min_delay) && std::isfinite(arc.max_delay), "disabled initialized scalar delays are absent");
  require(arc.transition_delays[0][0].has_value() && arc.input_output_delays[0][0][0].has_value(), "disabled initialized delay lanes are absent");
  require(arc.transition_types[0] == Transition::kRise, "disabled transition type is absent");
  for (const auto& analysis : arc.graph_delays) {
    for (const auto& input : analysis) {
      for (const auto& lane : input) {
        require(!lane.has_value(), "disabled graph-delay lane is not typed-null");
      }
    }
  }
  const auto& fallback = arc.timing_arcs.front().lanes[0][0][0];
  require(fallback.has_value() && fallback->is_fallback, "disabled timing-arc fallback is absent");
}

void testDisabledWitness()
{
  const NeutralWire wire = disabledWire();
  validateDisabledWitness(wire);
  NeutralWire missing_io = wire;
  missing_io.arcs[0].input_output_delays[0][0][0].reset();
  expectReject("disabled missing initialized IO", [&] { validateDisabledWitness(missing_io); });
  NeutralWire graph_poison = wire;
  graph_poison.arcs[0].graph_delays[0][0][0] = 4.0;
  expectReject("disabled nonempty graph delay", [&] { validateDisabledWitness(graph_poison); });
  NeutralWire fallback_poison = wire;
  fallback_poison.arcs[0].timing_arcs[0].lanes[0][0][0]->is_fallback = false;
  expectReject("disabled missing fallback provenance", [&] { validateDisabledWitness(fallback_poison); });
}
void testKnownAnswers()
{
  const NeutralWire empty;
  require(encodeNeutralWire(empty) == R"({"arcs":[],"in_rows":[],"nodes":[],"out_rows":[],"timing_order":[]})", "empty KAT bytes differ");
  knownAnswer("empty", empty);
  knownAnswer("rich", richWire());
  knownAnswer("near_tie", nearTieWire());
  knownAnswer("parallel", parallelWire());
  knownAnswer("disabled", disabledWire());
  knownAnswer("canary", canaryWire());
  testDisabledWitness();

  const double negative_zero = numberFromJson(numberToJson(-0.0), "signed-zero KAT");
  require(negative_zero == 0.0 && std::signbit(negative_zero), "signed-zero KAT lost sign");
  expectReject("signed-zero inverse KAT",
               [] { static_cast<void>(numberFromJson({{"kind", "finite"}, {"value", -0.0}, {"negative_zero", false}}, "signed-zero inverse KAT")); });
  expectReject("signed-zero flagged inverse KAT",
               [] { static_cast<void>(numberFromJson({{"kind", "finite"}, {"value", -0.0}, {"negative_zero", true}}, "signed-zero flagged inverse KAT")); });
  require(std::isinf(numberFromJson(numberToJson(std::numeric_limits<double>::infinity()), "positive infinity KAT")), "positive infinity KAT differs");
}

void testNegativeMatrix()
{
  expectReject("duplicate JSON key", [] { static_cast<void>(parseStrictJson(R"({"arcs":[],"arcs":[]})", "duplicate")); });
  expectReject("trailing JSON", [] { static_cast<void>(parseStrictJson("{} trailing", "trailing")); });
  expectReject("extra wire key", [] {
    Json value = neutralWireToJson(NeutralWire{});
    value["extra"] = true;
    static_cast<void>(neutralWireFromJson(value));
  });
  expectReject("NaN number", [] { static_cast<void>(numberToJson(std::numeric_limits<double>::quiet_NaN())); });
  expectReject("invalid negative zero",
               [] { static_cast<void>(numberFromJson({{"kind", "finite"}, {"value", 1.0}, {"negative_zero", true}}, "negative zero")); });
  expectReject("duplicate timing order", [] {
    NeutralWire wire = richWire();
    wire.timing_order[1] = wire.timing_order[0];
    static_cast<void>(encodeNeutralWire(wire));
  });
  expectReject("duplicate node", [] {
    NeutralWire wire = richWire();
    wire.nodes[1].key = wire.nodes[0].key;
    static_cast<void>(encodeNeutralWire(wire));
  });
  expectReject("wrong row rank", [] {
    NeutralWire wire = parallelWire();
    std::swap(wire.out_rows[0].arcs[0], wire.out_rows[0].arcs[1]);
    wire.out_rows[0].arcs.push_back(wire.out_rows[0].arcs.front());
    static_cast<void>(encodeNeutralWire(wire));
  });
}

}  // namespace
}  // namespace ista::csr::real_diff

int main()
{
  using namespace ista::csr::real_diff;
  try {
    testKnownAnswers();
    testNegativeMatrix();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CsrRealDifferentialWireContractTest: " << error.what() << '\n';
    return 1;
  }
}
