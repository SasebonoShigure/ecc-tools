#include "CsrModelTestMutation.hpp"
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

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceValidator.hpp"

namespace ista::csr::test {

inline void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

inline void requireValid(const std::vector<CsrContractViolation>& errors, const std::string& name)
{
  if (!errors.empty())
    throw std::runtime_error(name + " unexpectedly rejected at " + errors.front().field + ": " + errors.front().message);
}

inline void requireInvalid(const std::vector<CsrContractViolation>& errors, const std::string& name)
{
  require(!errors.empty(), name + " unexpectedly accepted");
}

inline void requireViolation(const std::vector<CsrContractViolation>& errors, const std::string& field, const std::string& message, const std::string& name)
{
  const bool found = std::ranges::any_of(
      errors, [&](const CsrContractViolation& error) { return error.field == field && error.message.find(message) != std::string::npos; });
  require(found, name + " did not report the targeted violation");
}

inline NodeKey port(std::string name)
{
  return NodeKey{NodeKind::kPort, std::move(name), {}, {}};
}

inline NodeKey pin(std::string full_name, std::string pin_name)
{
  return NodeKey{NodeKind::kInstancePin, std::move(full_name), "u1", std::move(pin_name)};
}

inline CsrSnapshotIdentity identity()
{
  CsrSnapshotIdentity value;
  value.session_token = 7;
  value.topology_fingerprint = std::string(64, 'a');
  value.clock_constraint_fingerprint = std::string(64, 'b');
  value.library_fingerprint = std::string(64, 'c');
  value.corner_fingerprint = std::string(64, 'd');
  value.parasitic_fingerprint = std::string(64, 'e');
  value.timing_input_fingerprint = std::string(64, 'f');
  return value;
}

inline ArcOutputLoadMatrix outputLoad(double value)
{
  ArcOutputLoadMatrix result{};
  for (auto& analysis : result)
    analysis.fill(value);
  return result;
}

inline void refreshIdentity(CsrReferenceInput& input, std::uint64_t session_token = 7)
{
  input.source_identity = makeReferenceInputIdentity(input, session_token, 0);
}

template <typename T>
inline OptionalSoa<T> soa(std::size_t size, std::uint8_t presence = 0, const T& value = T{})
{
  return OptionalSoa<T>{std::vector<std::uint8_t>(size, presence), std::vector<T>(size, value)};
}

inline OptionalSoa<Transition> transitionSoa(std::size_t size, std::uint8_t presence = 0, Transition value = Transition::kNone)
{
  return OptionalSoa<Transition>{std::vector<std::uint8_t>(size, presence), std::vector<Transition>(size, value)};
}

inline CsrReferenceInput referenceInput()
{
  CsrReferenceInput input;
  input.source_identity = identity();
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.nodes = {
      NodeRecord{
          .id = NodeId{0}, .key = port("in"), .direction = PinDirection::kInput, .instance_cell = {}, .net_name = "n1", .is_port = true, .is_startpoint = true},
      NodeRecord{.id = NodeId{1},
                 .key = port("out"),
                 .direction = PinDirection::kOutput,
                 .instance_cell = {},
                 .net_name = "n1",
                 .is_port = true,
                 .is_endpoint = true}};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}, {input.nodes[1].key, NodeId{1}}};

  ArcKeyBase base;
  base.source = input.nodes[0].key;
  base.sink = input.nodes[1].key;
  base.kind = ArcKind::kNet;
  base.owner_name = "n1";
  ArcRecord arc;
  arc.id = ArcId{0};
  arc.key = {base, 0};
  arc.source = NodeId{0};
  arc.sink = NodeId{1};
  arc.kind = ArcKind::kNet;
  arc.eligibility = {true, true, true, false};
  arc.transfer.delay = 0.0;
  arc.transfer.delay_min = 0.0;
  arc.transfer.delay_max = 0.0;
  arc.output_load = outputLoad(0.5);
  input.arcs = {arc};
  input.arc_ids_by_key = {{arc.key, arc.id}};
  input.out_row_offsets = {0, 1, 1};
  input.out_arc_ids = {ArcId{0}};
  input.in_row_offsets = {0, 0, 1};
  input.in_arc_ids = {ArcId{0}};
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids = {NodeId{1}};

  PortConstraintModel input_constraint;
  input_constraint.map_key = "in";
  input_constraint.embedded_port_name = "in";
  input_constraint.resolved_node = NodeId{0};
  input_constraint.input_delay = {1.0, 1.0};
  input_constraint.input_transition = 0.2;
  PortConstraintModel output_constraint;
  output_constraint.map_key = "out";
  output_constraint.embedded_port_name = "out";
  output_constraint.resolved_node = NodeId{1};
  output_constraint.output_load = 0.5;
  input.port_constraints = {input_constraint, output_constraint};
  input.clock_owner_by_node = {std::nullopt, std::nullopt};

  StartSeedModel seed;
  seed.seed_node = NodeId{0};
  seed.path_start_node = NodeId{0};
  seed.path_source = PathSource::kInput;
  seed.clock_tag = "clk";
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      seed.arrival[analysis][transition] = 1.0;
      seed.slew[analysis][transition] = 0.2;
    }
  input.start_seeds = {seed};
  EndpointRequiredModel endpoint;
  endpoint.node = NodeId{1};
  endpoint.clock_name = "clk";
  endpoint.clock_period = 0.0;
  input.endpoint_required = {endpoint};
  input.clock_tags = {"clk"};
  refreshIdentity(input);
  return input;
}

inline CsrReferenceInput cellInput()
{
  CsrReferenceInput input;
  input.source_identity = identity();
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.nodes = {
      NodeRecord{.id = NodeId{0}, .key = pin("u1:A", "A"), .direction = PinDirection::kInput, .instance_cell = "BUF_X1", .net_name = {}, .is_startpoint = true},
      NodeRecord{.id = NodeId{1},
                 .key = pin("u1:Y", "Y"),
                 .direction = PinDirection::kOutput,
                 .instance_cell = "BUF_X1",
                 .net_name = {},
                 .is_endpoint = true,
                 .is_output_pin = true}};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}, {input.nodes[1].key, NodeId{1}}};
  input.instances = {{"u1", "BUF_X1", {NodeId{0}, NodeId{1}}, false, false, false, false}};

  LibraryArcSignature signature;
  signature.library_name = "slow.lib";
  ChildTimingArcModel child;
  child.signature = signature;
  ArcKeyBase base;
  base.source = input.nodes[0].key;
  base.sink = input.nodes[1].key;
  base.kind = ArcKind::kCell;
  base.owner_name = "u1";
  base.library_source_port = "A";
  base.library_sink_port = "Y";
  base.library_arcs = {signature};
  ArcRecord arc;
  arc.id = ArcId{0};
  arc.key = {base, 0};
  arc.source = NodeId{0};
  arc.sink = NodeId{1};
  arc.kind = ArcKind::kCell;
  arc.eligibility = {true, true, true, false};
  arc.transfer.setChildren({child});
  arc.output_load = outputLoad(0.0);
  input.arcs = {arc};
  input.arc_ids_by_key = {{arc.key, arc.id}};
  input.out_row_offsets = {0, 1, 1};
  input.out_arc_ids = {ArcId{0}};
  input.in_row_offsets = {0, 0, 1};
  input.in_arc_ids = {ArcId{0}};
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids = {NodeId{1}};
  input.clock_tags = {"clk"};
  input.clock_owner_by_node = {std::nullopt, std::nullopt};

  StartSeedModel seed;
  seed.seed_node = NodeId{0};
  seed.path_start_node = NodeId{0};
  seed.clock_tag = "clk";
  for (auto& analysis : seed.slew)
    for (auto& transition : analysis)
      transition = 0.0;
  input.start_seeds = {seed};
  input.cell_pin_capacitances = {{NodeId{0}, 0.1, {}}, {NodeId{1}, 0.1, {}}};
  ReferencedLibraryCellModel cell;
  cell.cell_name = "BUF_X1";
  cell.present = true;
  cell.library_name = "slow.lib";
  cell.output_pin_name = "Y";
  cell.ports = {LibraryPortModel{.name = "A", .direction = PinDirection::kInput, .default_capacitance = 0.1, .transition_capacitance = {}},
                LibraryPortModel{.name = "Y", .direction = PinDirection::kOutput, .is_output = true, .default_capacitance = 0.1, .transition_capacitance = {}}};
  cell.cell_arcs = {LibraryCellArcModel{"A", "Y", arc.transfer}};
  input.referenced_library_cells = {std::move(cell)};
  refreshIdentity(input);
  return input;
}

inline CsrReferenceInput cycleInput()
{
  CsrReferenceInput input = cellInput();
  input.nodes[0].is_startpoint = false;
  input.nodes[1].is_endpoint = false;
  ArcRecord reverse = input.arcs[0];
  reverse.id = ArcId{1};
  reverse.key.base.source = input.nodes[1].key;
  reverse.key.base.sink = input.nodes[0].key;
  reverse.key.base.library_source_port = "Y";
  reverse.key.base.library_sink_port = "A";
  reverse.source = NodeId{1};
  reverse.sink = NodeId{0};
  reverse.native_traversal_rank = 1;
  input.arcs.push_back(reverse);
  input.arc_ids_by_key = {{input.arcs[0].key, ArcId{0}}, {input.arcs[1].key, ArcId{1}}};
  input.out_row_offsets = {0, 1, 2};
  input.out_arc_ids = {ArcId{0}, ArcId{1}};
  input.in_row_offsets = {0, 1, 2};
  input.in_arc_ids = {ArcId{1}, ArcId{0}};
  input.ordered_root_ids.clear();
  input.ordered_start_ids.clear();
  input.ordered_end_ids.clear();
  input.start_seeds.clear();
  input.clock_tags.clear();
  input.referenced_library_cells[0].cell_arcs.push_back({"Y", "A", reverse.transfer});
  refreshIdentity(input);
  return input;
}

inline CsrReferenceInput tableInput()
{
  CsrReferenceInput input = cellInput();
  auto signature = input.arcs[0].transfer.children()[0].signature;
  signature.delay_tables[0] = {true, TableVariable::kInputTransition, TableVariable::kNone};
  input.arcs[0].key.base.library_arcs[0] = signature;
  ista::csr::test::mutateChildren(input.arcs[0].transfer, [&](auto& children) {
    auto& child = children[0];
  child.signature = signature;
  child.delay_tables[0] = {true, TableVariable::kInputTransition, TableVariable::kNone, {{0.1, 0.2}}, {0.3, 0.4}};
  });
  input.referenced_library_cells[0].cell_arcs[0].transfer = input.arcs[0].transfer;
  input.arc_ids_by_key[0].key = input.arcs[0].key;
  refreshIdentity(input);
  return input;
}

inline CsrReferenceInput clockInput()
{
  CsrReferenceInput input = referenceInput();
  input.clocks = {ClockConstraintModel{"clk", "clk", {RawNodeReference{"in", NodeId{0}}}, 10.0, {0.0, 5.0}, false}};
  input.clock_owner_by_node = {std::optional<std::string>{"clk"}, std::optional<std::string>{"clk"}};
  input.arcs[0].eligibility.clock = true;
  input.endpoint_required[0].clock_period = 10.0;
  input.unsupported_reasons = {};
  auto& seed = input.start_seeds[0];
  seed.arrival[0][0] = 0.0;
  seed.arrival[0][1] = 5.0;
  seed.arrival[1][0] = 0.0;
  seed.arrival[1][1] = 0.0;
  refreshIdentity(input);
  return input;
}

inline ClockResultSoa emptyClock(std::size_t lanes)
{
  ClockResultSoa clock;
  clock.arrivals = soa<double>(lanes);
  clock.slews = soa<double>(lanes);
  clock.predecessor_nodes = soa<NodeId>(lanes);
  clock.predecessor_arc_delays = soa<double>(lanes);
  clock.predecessor_transitions = transitionSoa(lanes);
  return clock;
}

inline CsrTimingResult timingResult(const CsrReferenceInput& input)
{
  CsrTimingResult result;
  result.source_identity = input.source_identity;
  result.level_offsets = {0, 1, 2};
  result.level_nodes = {NodeId{0}, NodeId{1}};
  result.node_to_level = {1, 2};
  result.nodes.is_clock_point = soa<std::uint8_t>(2, 1, 0);
  result.nodes.arrivals = OptionalSoa<double>{{1, 1}, {1.0, 1.0}};
  result.nodes.requireds = OptionalSoa<double>{{1, 1}, {1.0, 1.0}};
  result.nodes.slacks = OptionalSoa<double>{{1, 1}, {0.0, 0.0}};
  result.nodes.launches = OptionalSoa<double>{{1, 1}, {0.0, 0.0}};
  result.nodes.clock_names = OptionalSoa<std::string>{{1, 1}, {"clk", "clk"}};
  result.nodes.predecessor_nodes = soa<NodeId>(2);
  result.nodes.predecessor_arcs = soa<ArcId>(2);
  result.nodes.predecessor_nodes.presence[1] = 1;
  result.nodes.predecessor_arcs.presence[1] = 1;

  constexpr std::size_t kClockLanes = 2 * kAnalysisCount * kTransitionCount;
  result.effective_clock = emptyClock(kClockLanes);
  result.physical_clock = emptyClock(kClockLanes);
  result.data_slews = soa<double>(kClockLanes);
  constexpr std::size_t kPathLanes = 2 * kAnalysisCount * kPathSourceCount * kTransitionCount;
  result.paths.row_offsets.assign(kPathLanes + 1, 0);

  if (input.start_seeds[0].path_source) {
    result.paths.row_offsets = {0, 1, 2, 2, 2, 3, 4, 4, 4, 5, 6, 6, 6, 7, 8, 8, 8};
    result.paths.clock_tags = soa<ClockTagId>(8, 1, ClockTagId{0});
    result.paths.arrivals = soa<double>(8, 1, 1.0);
    result.paths.slews = soa<double>(8, 1, 0.2);
    result.paths.launches = soa<double>(8, 1, 0.0);
    result.paths.start_nodes = soa<NodeId>(8, 1, NodeId{0});
    result.paths.predecessor_nodes = soa<NodeId>(8);
    result.paths.predecessor_arcs = soa<ArcId>(8);
    result.paths.predecessor_arc_delays = soa<double>(8);
    result.paths.predecessor_transitions = transitionSoa(8);
    result.paths.crpr_nodes = soa<NodeId>(8);
    result.paths.crpr_transitions = transitionSoa(8);
    for (std::size_t entry = 4; entry < 8; ++entry) {
      result.paths.predecessor_nodes.presence[entry] = 1;
      result.paths.predecessor_arcs.presence[entry] = 1;
      result.paths.predecessor_arc_delays.presence[entry] = 1;
      result.paths.predecessor_transitions.presence[entry] = 1;
      result.paths.predecessor_transitions.values[entry] = entry % kTransitionCount == 0 ? Transition::kRise : Transition::kFall;
    }
    result.data_slews = soa<double>(kClockLanes, 1, 0.2);
  }

  result.arcs.delays = soa<double>(1, 1, 0.0);
  result.arcs.min_delays = soa<double>(1, 1, 0.0);
  result.arcs.max_delays = soa<double>(1, 1, 0.0);
  result.arcs.transition_delays = soa<double>(4, 1, 0.0);
  result.arcs.input_output_delays = soa<double>(8);
  result.arcs.graph_delays = soa<double>(8);
  for (std::size_t lane : {std::size_t{0}, std::size_t{3}, std::size_t{4}, std::size_t{7}}) {
    result.arcs.input_output_delays.presence[lane] = 1;
    result.arcs.graph_delays.presence[lane] = 1;
  }
  result.arcs.transition_types = transitionSoa(2, 1, Transition::kRise);
  result.arcs.transition_types.values[1] = Transition::kFall;
  const std::size_t children = input.arcs[0].transfer.children().size();
  result.arcs.timing_arc_row_offsets = {0, children};
  for (const auto& child : input.arcs[0].transfer.children())
    result.arcs.timing_arc_indices.push_back(child.signature.arc_index);
  result.arcs.timing_arc_delays = soa<double>(children * 8);
  result.arcs.timing_arc_fallbacks = soa<std::uint8_t>(children * 8);
  return result;
}

inline CsrTimingResult cellTimingResult(const CsrReferenceInput& input)
{
  CsrTimingResult result;
  result.source_identity = input.source_identity;
  result.level_offsets = {0, 1, 2};
  result.level_nodes = {NodeId{0}, NodeId{1}};
  result.node_to_level = {1, 2};
  result.nodes.is_clock_point = soa<std::uint8_t>(2, 1, 0);
  result.nodes.arrivals = soa<double>(2, 1, 0.0);
  result.nodes.requireds = soa<double>(2, 1, 0.0);
  result.nodes.slacks = soa<double>(2, 1, 0.0);
  result.nodes.launches = soa<double>(2, 1, 0.0);
  result.nodes.clock_names = soa<std::string>(2, 1, "clk");
  result.nodes.predecessor_nodes = soa<NodeId>(2);
  result.nodes.predecessor_arcs = soa<ArcId>(2);
  result.nodes.predecessor_nodes.presence[1] = 1;
  result.nodes.predecessor_arcs.presence[1] = 1;

  constexpr std::size_t kClockLanes = 2 * kAnalysisCount * kTransitionCount;
  result.effective_clock = emptyClock(kClockLanes);
  result.physical_clock = emptyClock(kClockLanes);
  result.data_slews = soa<double>(kClockLanes, 1, 0.0);
  result.paths.row_offsets.assign(2 * kAnalysisCount * kPathSourceCount * kTransitionCount + 1, 0);

  result.arcs.delays = soa<double>(1, 1, 0.0);
  result.arcs.min_delays = soa<double>(1, 1, 0.0);
  result.arcs.max_delays = soa<double>(1, 1, 0.0);
  result.arcs.transition_delays = soa<double>(4, 1, 0.0);
  result.arcs.input_output_delays = soa<double>(8);
  result.arcs.graph_delays = soa<double>(8);
  result.arcs.transition_types = transitionSoa(2, 1, Transition::kRise);
  result.arcs.transition_types.values[1] = Transition::kFall;
  result.arcs.timing_arc_row_offsets = {0, 1};
  result.arcs.timing_arc_indices = {input.arcs[0].transfer.children()[0].signature.arc_index};
  result.arcs.timing_arc_delays = soa<double>(8);
  result.arcs.timing_arc_fallbacks = soa<std::uint8_t>(8);
  result.arcs.timing_arc_delays.presence[0] = 1;
  result.arcs.timing_arc_fallbacks.presence[0] = 1;
  return result;
}

}  // namespace ista::csr::test
