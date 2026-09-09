// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrValidatorTestFixture.hpp"

namespace ista::csr::test {
namespace {

void requireStrictLiteral(const CsrReferenceInput& input, const std::string& name)
{
  const auto errors = validateReferenceInput(input);
  if (!errors.empty())
    throw std::runtime_error(name + " is not strict-valid at " + errors.front().field + ": " + errors.front().message);
}

const CsrTimingResult& computeLiteralTiming(const CsrReferenceInput& input, CsrComputationResult& computation, const std::string& name)
{
  computation = computeContractTiming(input);
  const auto* result = std::get_if<CsrTimingResult>(&computation.outcome);
  requireCpuReference(result != nullptr, name + " did not return timing");
  requireCpuReference(validateTimingResult(input, *result).empty(), name + " returned an invalid timing result");
  return *result;
}

CsrReferenceInput twoTagInput(bool different)
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  const auto make_node = [](NodeRecord node, std::uint64_t id, NodeKey key, std::string net) {
    node.id = NodeId{id};
    node.key = std::move(key);
    node.net_name = std::move(net);
    return node;
  };
  input.nodes
      = {make_node(input.nodes[0], 0, port("a"), "na"), make_node(input.nodes[0], 1, port("b"), "nb"), make_node(input.nodes[1], 2, input.nodes[1].key, "na"),
         make_node(input.nodes[1], 3, pin("u1:B", "B"), "nb"), make_node(input.nodes[2], 4, input.nodes[2].key, {})};
  input.node_ids_by_key.clear();
  for (const NodeRecord& node : input.nodes)
    input.node_ids_by_key.push_back({node.key, node.id});

  const ArcRecord net = input.arcs[0];
  const ArcRecord cell = input.arcs[1];
  const auto make_arc = [&](ArcRecord arc, std::uint64_t id, std::uint64_t source, std::uint64_t sink, std::string owner, std::string source_port) {
    arc.id = ArcId{id};
    arc.source = NodeId{source};
    arc.sink = NodeId{sink};
    arc.native_traversal_rank = id;
    arc.key.base.source = input.nodes[source].key;
    arc.key.base.sink = input.nodes[sink].key;
    arc.key.base.owner_name = std::move(owner);
    arc.key.base.library_source_port = std::move(source_port);
    arc.key.base.library_sink_port = arc.kind == ArcKind::kCell ? "Y" : "";
    return arc;
  };
  input.arcs = {make_arc(net, 0, 0, 2, "na", ""), make_arc(net, 1, 1, 3, "nb", ""), make_arc(cell, 2, 2, 4, "u1", "A"), make_arc(cell, 3, 3, 4, "u1", "B")};
  input.arc_ids_by_key.clear();
  for (const ArcRecord& arc : input.arcs)
    input.arc_ids_by_key.push_back({arc.key, arc.id});
  input.out_row_offsets = {0, 1, 2, 3, 4, 4};
  input.out_arc_ids = {ArcId{0}, ArcId{1}, ArcId{2}, ArcId{3}};
  input.in_row_offsets = {0, 0, 0, 1, 2, 4};
  input.in_arc_ids = input.out_arc_ids;
  input.ordered_root_ids = {NodeId{0}, NodeId{1}};
  input.ordered_start_ids = input.ordered_root_ids;
  input.ordered_end_ids = {NodeId{4}};
  input.clock_owner_by_node.assign(5, std::nullopt);
  input.instances.front().ordered_pins = {NodeId{2}, NodeId{3}, NodeId{4}};

  const auto make_constraint = [](PortConstraintModel value, std::uint64_t node, std::string name, std::string tag) {
    value.map_key = name;
    value.embedded_port_name = std::move(name);
    value.resolved_node = NodeId{node};
    value.clock_name = std::move(tag);
    return value;
  };
  const PortConstraintModel constraint = input.port_constraints.front();
  input.port_constraints = {make_constraint(constraint, 0, "a", "A"), make_constraint(constraint, 1, "b", different ? "B" : "A")};
  const auto make_seed = [](StartSeedModel seed, std::uint64_t node, std::string tag) {
    seed.seed_node = NodeId{node};
    seed.path_start_node = NodeId{node};
    seed.clock_tag = std::move(tag);
    return seed;
  };
  const StartSeedModel seed = input.start_seeds.front();
  input.start_seeds = {make_seed(seed, 0, "A"), make_seed(seed, 1, different ? "B" : "A")};
  input.clock_tags = different ? std::vector<std::string>{"A", "B"} : std::vector<std::string>{"A"};

  auto& library_cell = input.referenced_library_cells.front();
  LibraryPortModel port_b = library_cell.ports.front();
  port_b.name = "B";
  library_cell.ports.insert(library_cell.ports.begin() + 1, port_b);
  library_cell.cell_arcs = {{"A", "Y", input.arcs[2].transfer}, {"B", "Y", input.arcs[3].transfer}};
  input.cell_pin_capacitances = {{NodeId{2}, 0.1, {}}, {NodeId{3}, 0.1, {}}, {NodeId{4}, 0.1, {}}};
  refreshCpuReferenceIdentity(input);
  requireStrictLiteral(input, different ? "different-tag fixture" : "same-tag fixture");
  return input;
}

void testTagFlattening()
{
  CsrComputationResult computation;
  for (bool different : {false, true}) {
    const CsrTimingResult result = computeLiteralTiming(twoTagInput(different), computation, different ? "different tag" : "same tag");
    const std::size_t begin = result.paths.row_offsets[32];
    const std::size_t end = result.paths.row_offsets[33];
    requireCpuReference(end - begin == (different ? 2U : 1U), "same/different tag entry count differs");
    requireCpuReference(result.paths.clock_tags.values[begin] == ClockTagId{0} && (!different || result.paths.clock_tags.values[begin + 1] == ClockTagId{1}),
                        "flattened tags are not strictly ordered");
  }
}

CsrReferenceInput wholeLiteralInput()
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  input.nodes.push_back(NodeRecord{.id = NodeId{3},
                                   .key = NodeKey{NodeKind::kInstancePin, "u2:P", "u2", "P"},
                                   .direction = PinDirection::kInput,
                                   .instance_cell = "TAP_X1",
                                   .net_name = {}});
  input.node_ids_by_key.push_back({input.nodes.back().key, NodeId{3}});
  input.instances.push_back(InstanceRecord{"u2", "TAP_X1", {NodeId{3}}, false, false, false, false});
  ReferencedLibraryCellModel tap;
  tap.cell_name = "TAP_X1";
  tap.present = true;
  tap.library_name = "slow.lib";
  tap.ports = {LibraryPortModel{.name = "P", .direction = PinDirection::kInput, .default_capacitance = 0.1, .transition_capacitance = {}}};
  input.referenced_library_cells.push_back(tap);
  input.cell_pin_capacitances.push_back({NodeId{3}, 0.1, {}});
  input.out_row_offsets.push_back(input.out_row_offsets.back());
  input.in_row_offsets.push_back(input.in_row_offsets.back());
  input.port_constraints.front().clock_name = "clk";
  input.ordered_root_ids = {NodeId{0}, NodeId{3}};
  input.clocks = {ClockConstraintModel{"B", "E", {RawNodeReference{"u2:P", NodeId{3}}}, 10.0, {2.0, 7.0}, false}};
  input.clock_owner_by_node = {std::nullopt, std::nullopt, std::nullopt, std::optional<std::string>{"B"}};
  input.clock_tags = {"B", "E", "clk"};
  refreshCpuReferenceIdentity(input);
  requireStrictLiteral(input, "whole-result literal fixture");
  return input;
}

void testWholeResultLiteral()
{
  const CsrReferenceInput input = wholeLiteralInput();
  CsrTimingResult expected;
  expected.schema_version = input.schema_version;
  expected.source_identity = input.source_identity;
  expected.level_offsets = {0, 2, 3, 4};
  expected.level_nodes = {NodeId{0}, NodeId{3}, NodeId{1}, NodeId{2}};
  expected.node_to_level = {1, 2, 3, 1};
  expected.timing_order = expected.level_nodes;

  expected.nodes.is_clock_point = {{1, 1, 1, 1}, {0, 0, 0, 1}};
  expected.nodes.arrivals = {{1, 1, 1, 1}, {1.0, 1.0, 2.0, -std::numeric_limits<double>::infinity()}};
  expected.nodes.requireds = {{1, 1, 1, 1}, {1.0, 1.0, 2.0, std::numeric_limits<double>::infinity()}};
  expected.nodes.slacks = {{1, 1, 1, 1}, {0.0, 0.0, 0.0, 0.0}};
  expected.nodes.launches = {{1, 1, 1, 1}, {0.0, 0.0, 0.0, 0.0}};
  expected.nodes.clock_names = {{1, 1, 1, 1}, {"clk", "clk", "clk", "B"}};
  expected.nodes.predecessor_nodes = {{0, 1, 1, 0}, {NodeId{0}, NodeId{0}, NodeId{1}, NodeId{0}}};
  expected.nodes.predecessor_arcs = {{0, 1, 1, 0}, {ArcId{0}, ArcId{0}, ArcId{1}, ArcId{0}}};

  const OptionalSoa<double> clock_values{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
                                         {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
  const OptionalSoa<NodeId> absent_clock_nodes{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                                               {NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0},
                                                NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}}};
  const OptionalSoa<double> absent_clock_delays{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                                                {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
  const OptionalSoa<Transition> absent_clock_transitions{
      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone,
       Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone}};
  expected.effective_clock = {clock_values, clock_values, absent_clock_nodes, absent_clock_delays, absent_clock_transitions};
  expected.physical_clock = expected.effective_clock;
  expected.data_slews = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0}, {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.5, 0.5, 0.5, 0.5, 0.0, 0.0, 0.0, 0.0}};

  expected.paths.row_offsets = {0, 1, 2, 2, 2, 3, 4, 4, 4, 5, 6, 6, 6, 7, 8, 8, 8, 9, 10, 10, 10, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12};
  expected.paths.clock_tags = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
                               {ClockTagId{2}, ClockTagId{2}, ClockTagId{2}, ClockTagId{2}, ClockTagId{2}, ClockTagId{2}, ClockTagId{2}, ClockTagId{2},
                                ClockTagId{2}, ClockTagId{2}, ClockTagId{2}, ClockTagId{2}}};
  expected.paths.arrivals = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 3.0, 3.0, 3.0, 3.0}};
  expected.paths.slews = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.5, 0.5, 0.5, 0.5}};
  expected.paths.launches = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
  expected.paths.start_nodes
      = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
         {NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}}};
  expected.paths.predecessor_nodes
      = {{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
         {NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{1}, NodeId{1}, NodeId{1}, NodeId{1}}};
  expected.paths.predecessor_arcs = {{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
                                     {ArcId{0}, ArcId{0}, ArcId{0}, ArcId{0}, ArcId{0}, ArcId{0}, ArcId{0}, ArcId{0}, ArcId{1}, ArcId{1}, ArcId{1}, ArcId{1}}};
  expected.paths.predecessor_arc_delays = {{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 2.0, 2.0, 2.0}};
  expected.paths.predecessor_transitions = {{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
                                            {Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kRise, Transition::kFall,
                                             Transition::kRise, Transition::kFall, Transition::kRise, Transition::kFall, Transition::kRise, Transition::kFall}};
  expected.paths.crpr_nodes
      = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
         {NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}, NodeId{0}}};
  expected.paths.crpr_transitions = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                                     {Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone,
                                      Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone, Transition::kNone}};

  expected.arcs.delays = {{1, 1}, {0.0, 1.0}};
  expected.arcs.min_delays = expected.arcs.delays;
  expected.arcs.max_delays = expected.arcs.delays;
  expected.arcs.transition_delays = {{1, 1, 1, 1, 1, 1, 1, 1}, {0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0}};
  expected.arcs.input_output_delays
      = {{1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0}};
  expected.arcs.graph_delays
      = {{1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0, 2.0, 0.0, 0.0, 2.0}};
  expected.arcs.transition_types = {{1, 1, 1, 1}, {Transition::kRise, Transition::kFall, Transition::kRise, Transition::kFall}};
  expected.arcs.timing_arc_row_offsets = {0, 0, 1};
  expected.arcs.timing_arc_indices = {7};
  expected.arcs.timing_arc_delays = {{1, 0, 0, 1, 1, 0, 0, 1}, {2.0, 0.0, 0.0, 2.0, 2.0, 0.0, 0.0, 2.0}};
  expected.arcs.timing_arc_fallbacks = {{1, 0, 0, 1, 1, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 0, 0}};

  CsrComputationResult computation;
  const CsrTimingResult actual = computeLiteralTiming(input, computation, "whole-result literal");
  requireExactTimingResult(actual, expected, "whole-result literal");
}

}  // namespace

void runWholeResultLiteralContract()
{
  testTagFlattening();
  testWholeResultLiteral();
}

}  // namespace ista::csr::test
