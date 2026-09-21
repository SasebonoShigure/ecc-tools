// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include "CsrModelTestMutation.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrValidatorTestFixture.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::test {
namespace {

std::size_t nodeLane(std::size_t node, std::size_t analysis, std::size_t transition)
{
  return (node * kAnalysisCount + analysis) * kTransitionCount + transition;
}

void requireBits(double actual, double expected, const std::string& name)
{
  requireCpuReference(std::bit_cast<std::uint64_t>(actual) == std::bit_cast<std::uint64_t>(expected), name + " bit pattern differs");
}

void requireStrict(const CsrReferenceInput& input, const std::string& name)
{
  const auto errors = validateReferenceInput(input);
  if (!errors.empty())
    throw std::runtime_error(name + " is not strict-valid at " + errors.front().field + ": " + errors.front().message);
}

const CsrTimingResult& computeTiming(const CsrReferenceInput& input, CsrComputationResult& computation, const std::string& name)
{
  computation = computeContractTiming(input);
  const auto* result = std::get_if<CsrTimingResult>(&computation.outcome);
  requireCpuReference(result != nullptr, name + " did not return timing");
  requireCpuReference(validateTimingResult(input, *result).empty(), name + " returned an invalid timing result");
  return *result;
}

void configureOutputConstraint(CsrReferenceInput& input, std::optional<double> max_delay, std::optional<double> min_delay)
{
  ClockConstraintModel clock;
  clock.map_key = "clk";
  clock.embedded_clock_name = "clk";
  clock.period = 10.0;
  input.clocks = {clock};
  auto& port = input.port_constraints.back();
  port.clock_name = "clk";
  port.output_delay = {max_delay, min_delay};
  auto& endpoint = input.endpoint_required.front();
  endpoint.clock_name = "clk";
  endpoint.clock_period = 10.0;
  endpoint.output_delay = {max_delay, min_delay};
}

CsrReferenceInput isolatedClockSourceInput()
{
  CsrReferenceInput input = cellInput();
  input.nodes.resize(1);
  input.nodes.front().is_startpoint = false;
  input.nodes.front().is_endpoint = false;
  input.node_ids_by_key.resize(1);
  input.instances.front().ordered_pins = {NodeId{0}};
  input.arcs.clear();
  input.arc_ids_by_key.clear();
  input.out_row_offsets = {0, 0};
  input.out_arc_ids.clear();
  input.in_row_offsets = {0, 0};
  input.in_arc_ids.clear();
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids.clear();
  input.ordered_end_ids.clear();
  input.clocks = {ClockConstraintModel{"B", "E", {RawNodeReference{"u1:A", NodeId{0}}, RawNodeReference{"u1:A", NodeId{0}}}, 10.0, {2.0, 7.0}, false}};
  input.clock_owner_by_node = {std::optional<std::string>{"B"}};
  input.port_constraints.clear();
  input.start_seeds.clear();
  input.endpoint_required.clear();
  input.cell_pin_capacitances.resize(1);
  auto& cell = input.referenced_library_cells.front();
  cell.output_pin_name.clear();
  cell.ports.resize(1);
  cell.cell_arcs.clear();
  input.clock_tags = {"B", "E"};
  refreshCpuReferenceIdentity(input);
  requireStrict(input, "isolated internal clock-source fixture");
  return input;
}

CsrReferenceInput directStartClockInput()
{
  CsrReferenceInput input = referenceInput();
  input.nodes.resize(1);
  input.nodes.front().is_endpoint = false;
  input.node_ids_by_key.resize(1);
  input.arcs.clear();
  input.arc_ids_by_key.clear();
  input.out_row_offsets = {0, 0};
  input.out_arc_ids.clear();
  input.in_row_offsets = {0, 0};
  input.in_arc_ids.clear();
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids.clear();
  input.clocks = {ClockConstraintModel{"A", "X", {RawNodeReference{"missing", std::nullopt}}, 5.0, {1.0, 4.0}, false},
                  ClockConstraintModel{"B", "E", {RawNodeReference{"missing", std::nullopt}, RawNodeReference{"in", NodeId{0}}}, 10.0, {2.0, 7.0}, false}};
  input.clock_owner_by_node = {std::optional<std::string>{"B"}};
  input.port_constraints.resize(1);
  input.endpoint_required.clear();
  auto& seed = input.start_seeds.front();
  seed.clock_tag = "E";
  seed.arrival[0][0] = 2.0;
  seed.arrival[0][1] = 7.0;
  seed.arrival[1][0] = 2.0;
  seed.arrival[1][1] = 2.0;
  input.clock_tags = {"A", "B", "E", "X"};
  refreshCpuReferenceIdentity(input);
  requireStrict(input, "direct start clock-source fixture");
  return input;
}

CsrReferenceInput incomingClockSourceInput()
{
  CsrReferenceInput input = referenceInput();
  auto& sink = input.nodes[1];
  sink.key = NodeKey{NodeKind::kInstancePin, "u1:P", "u1", "P"};
  sink.direction = PinDirection::kInput;
  sink.instance_cell = "BUF_X1";
  sink.is_port = false;
  sink.is_endpoint = false;
  input.node_ids_by_key[1].key = sink.key;
  input.instances = {InstanceRecord{"u1", "BUF_X1", {NodeId{1}}, false, false, false, false}};
  auto& arc = input.arcs.front();
  arc.key.base.sink = sink.key;
  arc.output_load = outputLoad(0.1);
  input.arc_ids_by_key.front().key = arc.key;
  input.ordered_end_ids.clear();
  input.clocks = {ClockConstraintModel{"B", "E", {RawNodeReference{"u1:P", NodeId{1}}}, 10.0, {2.0, 7.0}, false}};
  input.clock_owner_by_node = {std::nullopt, std::optional<std::string>{"B"}};
  input.port_constraints.resize(1);
  input.port_constraints.front().clock_name = "A";
  input.start_seeds.front().clock_tag = "A";
  input.endpoint_required.clear();
  input.cell_pin_capacitances = {{NodeId{1}, 0.1, {}}};
  ReferencedLibraryCellModel cell;
  cell.cell_name = "BUF_X1";
  cell.present = true;
  cell.library_name = "slow.lib";
  cell.ports = {LibraryPortModel{.name = "P", .direction = PinDirection::kInput, .default_capacitance = 0.1, .transition_capacitance = {}}};
  input.referenced_library_cells = {cell};
  input.clock_tags = {"A", "B", "E"};
  refreshCpuReferenceIdentity(input);
  requireStrict(input, "incoming internal clock-source fixture");
  return input;
}

struct DagArc
{
  std::size_t source;
  std::size_t sink;
  bool disabled{false};
  bool loop_disabled{false};
};

CsrReferenceInput dagInput(std::size_t node_count, const std::vector<DagArc>& specs)
{
  CsrReferenceInput input;
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.out_row_offsets.assign(node_count + 1, 0);
  input.in_row_offsets.assign(node_count + 1, 0);
  input.clock_owner_by_node.assign(node_count, std::nullopt);
  if (node_count == 0) {
    refreshCpuReferenceIdentity(input);
    requireStrict(input, "empty DAG fixture");
    return input;
  }

  std::vector<std::string> names;
  for (std::size_t node = 0; node < node_count; ++node) {
    names.push_back("p" + std::to_string(node));
    const bool output = node + 1 == node_count;
    input.nodes.push_back(NodeRecord{.id = NodeId{node},
                                     .key = pin("u1:" + names.back(), names.back()),
                                     .direction = output ? PinDirection::kOutput : PinDirection::kInput,
                                     .instance_cell = "DAG_X1",
                                     .net_name = {},
                                     .is_output_pin = output});
    input.node_ids_by_key.push_back({input.nodes.back().key, NodeId{node}});
  }

  std::vector<std::uint8_t> incoming(node_count, 0);
  std::vector<std::uint8_t> outgoing(node_count, 0);
  std::vector<ArcKeyBase> bases;
  for (const DagArc& spec : specs) {
    ArcKeyBase base;
    base.source = input.nodes[spec.source].key;
    base.sink = input.nodes[spec.sink].key;
    base.kind = ArcKind::kCell;
    base.owner_name = "u1";
    base.library_source_port = names[spec.source];
    base.library_sink_port = names[spec.sink];
    bases.push_back(std::move(base));
    incoming[spec.sink] = 1;
    outgoing[spec.source] = 1;
  }
  const std::vector<ArcKey> keys = assignParallelOrdinals(bases);
  for (std::size_t rank = 0; rank < specs.size(); ++rank) {
    const DagArc& spec = specs[rank];
    ArcRecord arc;
    arc.key = keys[rank];
    arc.source = NodeId{spec.source};
    arc.sink = NodeId{spec.sink};
    arc.kind = ArcKind::kCell;
    arc.native_traversal_rank = rank;
    arc.is_disabled = spec.disabled;
    arc.is_loop_disabled = spec.loop_disabled;
    if (!spec.disabled && !spec.loop_disabled)
      arc.eligibility = {true, true, true, false};
    arc.transfer.delay = 1.0;
    arc.transfer.delay_min = 1.0;
    arc.transfer.delay_max = 1.0;
    arc.transfer.is_disabled = spec.disabled;
    arc.output_load = outputLoad(0.0);
    input.arcs.push_back(std::move(arc));
  }
  std::ranges::sort(input.arcs, [](const ArcRecord& lhs, const ArcRecord& rhs) { return arcKeyLess(lhs.key, rhs.key); });
  for (std::size_t arc = 0; arc < input.arcs.size(); ++arc) {
    input.arcs[arc].id = ArcId{arc};
    input.arc_ids_by_key.push_back({input.arcs[arc].key, ArcId{arc}});
  }
  for (std::size_t node = 0; node < node_count; ++node) {
    input.nodes[node].is_startpoint = incoming[node] == 0;
    input.nodes[node].is_endpoint = outgoing[node] == 0;
    if (input.nodes[node].is_startpoint)
      input.ordered_start_ids.push_back(NodeId{node});
    if (input.nodes[node].is_endpoint)
      input.ordered_end_ids.push_back(NodeId{node});
  }
  for (std::size_t rank = 0; rank < specs.size(); ++rank) {
    const auto found = std::ranges::find(input.arcs, rank, &ArcRecord::native_traversal_rank);
    input.out_arc_ids.push_back(found->id);
    input.in_arc_ids.push_back(found->id);
  }
  std::ranges::stable_sort(input.out_arc_ids, [&](ArcId lhs, ArcId rhs) {
    const auto& left = input.arcs[lhs.value()];
    const auto& right = input.arcs[rhs.value()];
    return left.source == right.source ? left.native_traversal_rank < right.native_traversal_rank : left.source.value() < right.source.value();
  });
  std::ranges::stable_sort(input.in_arc_ids, [&](ArcId lhs, ArcId rhs) {
    const auto& left = input.arcs[lhs.value()];
    const auto& right = input.arcs[rhs.value()];
    return left.sink == right.sink ? left.native_traversal_rank < right.native_traversal_rank : left.sink.value() < right.sink.value();
  });
  for (ArcId arc : input.out_arc_ids)
    ++input.out_row_offsets[input.arcs[arc.value()].source.value() + 1];
  for (ArcId arc : input.in_arc_ids)
    ++input.in_row_offsets[input.arcs[arc.value()].sink.value() + 1];
  std::partial_sum(input.out_row_offsets.begin(), input.out_row_offsets.end(), input.out_row_offsets.begin());
  std::partial_sum(input.in_row_offsets.begin(), input.in_row_offsets.end(), input.in_row_offsets.begin());

  std::vector<std::uint64_t> indegree(node_count, 0);
  for (const ArcRecord& arc : input.arcs)
    if (!arc.is_disabled && !arc.is_loop_disabled)
      ++indegree[arc.sink.value()];
  for (std::size_t node = 0; node < node_count; ++node) {
    if (indegree[node] == 0)
      input.ordered_root_ids.push_back(NodeId{node});
    if (incoming[node] == 0) {
      StartSeedModel seed;
      seed.seed_node = NodeId{node};
      seed.path_start_node = NodeId{node};
      seed.clock_tag = "clk";
      for (auto& analysis : seed.slew)
        for (auto& value : analysis)
          value = 0.0;
      input.start_seeds.push_back(std::move(seed));
    }
  }
  if (!input.start_seeds.empty())
    input.clock_tags = {"clk"};

  input.instances = {InstanceRecord{"u1", "DAG_X1", {}, false, false, false, false}};
  ReferencedLibraryCellModel cell;
  cell.cell_name = "DAG_X1";
  cell.present = true;
  cell.library_name = "slow.lib";
  cell.output_pin_name = names.back();
  for (std::size_t node = 0; node < node_count; ++node) {
    input.instances.front().ordered_pins.push_back(NodeId{node});
    const bool output = node + 1 == node_count;
    cell.ports.push_back(LibraryPortModel{.name = names[node],
                                          .direction = output ? PinDirection::kOutput : PinDirection::kInput,
                                          .is_output = output,
                                          .default_capacitance = 0.0,
                                          .transition_capacitance = {}});
    input.cell_pin_capacitances.push_back({NodeId{node}, 0.0, {}});
  }
  for (const ArcRecord& arc : input.arcs)
    cell.cell_arcs.push_back({arc.key.base.library_source_port, arc.key.base.library_sink_port, arc.transfer});
  input.referenced_library_cells = {std::move(cell)};
  refreshCpuReferenceIdentity(input);
  requireStrict(input, "cell-only DAG fixture");
  return input;
}

CsrReferenceInput parallelBoundaryInput(double second_delay, double second_slew)
{
  CsrReferenceInput input = makeCpuReferencePropertyFixture(4);
  input.port_constraints.front().input_delay = {0.0, 0.0};
  input.port_constraints.front().input_transition = 0.0;
  for (auto& analysis : input.start_seeds.front().arrival)
    for (auto& value : analysis)
      value = 0.0;
  for (auto& analysis : input.start_seeds.front().slew)
    for (auto& value : analysis)
      value = 0.0;
  for (std::size_t arc = 1; arc < input.arcs.size(); ++arc) {
    auto children = input.arcs[arc].transfer.children();
  auto& child = children.front();
    const double delay = arc == 1 ? 0.0 : second_delay;
    const double slew = arc == 1 ? 1.0 : second_slew;
    for (std::size_t output = 0; output < kTransitionCount; ++output) {
      std::ranges::fill(child.delay_tables[output].values, delay);
      std::ranges::fill(child.slew_tables[output].values, slew);
    }
    input.arcs[arc].transfer.setChildren(children);
    input.referenced_library_cells.front().cell_arcs[arc - 1].transfer = input.arcs[arc].transfer;
  }
  refreshCpuReferenceIdentity(input);
  requireStrict(input, "parallel boundary fixture");
  return input;
}

void testChainLiteral()
{
  const CsrReferenceInput input = makeCpuReferenceNetFixture();
  CsrComputationResult computation;
  const CsrTimingResult result = computeTiming(input, computation, "net chain");
  requireCpuReference(result.level_offsets == std::vector<std::uint64_t>({0, 1, 2}), "chain level offsets differ");
  requireCpuReference(result.level_nodes == std::vector<NodeId>({NodeId{0}, NodeId{1}}), "chain level nodes differ");
  requireCpuReference(result.node_to_level == std::vector<std::uint64_t>({1, 2}), "chain node-to-level differs");
  requireBits(result.nodes.arrivals.values[0], 1.0, "chain source arrival");
  requireBits(result.nodes.arrivals.values[1], 1.0, "chain sink arrival");
  requireBits(result.nodes.requireds.values[0], 1.0, "chain source required");
  requireBits(result.nodes.requireds.values[1], 1.0, "chain sink required");
  requireCpuReference(result.nodes.predecessor_nodes.presence == std::vector<std::uint8_t>({0, 1}) && result.nodes.predecessor_nodes.values[1] == NodeId{0},
                      "chain predecessor node differs");
  requireCpuReference(result.nodes.predecessor_arcs.presence == std::vector<std::uint8_t>({0, 1}) && result.nodes.predecessor_arcs.values[1] == ArcId{0},
                      "chain predecessor arc differs");
  const std::vector<std::uint64_t> expected_offsets{0, 1, 2, 2, 2, 3, 4, 4, 4, 5, 6, 6, 6, 7, 8, 8, 8};
  requireCpuReference(result.paths.row_offsets == expected_offsets, "chain ragged path offsets differ");
  requireCpuReference(result.paths.clock_tags.values.size() == 8, "chain path entry count differs");
  for (std::size_t entry = 0; entry < result.paths.clock_tags.values.size(); ++entry) {
    requireCpuReference(result.paths.clock_tags.presence[entry] == 1, "chain path tag is absent");
    requireCpuReference(result.paths.clock_tags.values[entry] == ClockTagId{0}, "chain path tag differs");
    requireBits(result.paths.arrivals.values[entry], 1.0, "chain path arrival");
  }
}

void testRequiredLiterals()
{
  CsrReferenceInput constrained = makeCpuReferenceNetFixture();
  configureOutputConstraint(constrained, 2.0, std::nullopt);
  refreshCpuReferenceIdentity(constrained);
  CsrComputationResult computation;
  const CsrTimingResult result = computeTiming(constrained, computation, "max output delay");
  requireBits(result.nodes.requireds.values[0], 8.0, "reverse constrained required");
  requireBits(result.nodes.requireds.values[1], 8.0, "endpoint constrained required");
  requireBits(result.nodes.slacks.values[0], 7.0, "source constrained slack");
  requireBits(result.nodes.slacks.values[1], 7.0, "sink constrained slack");

  CsrReferenceInput min_only = makeCpuReferenceNetFixture();
  configureOutputConstraint(min_only, std::nullopt, 2.0);
  refreshCpuReferenceIdentity(min_only);
  const CsrTimingResult min_result = computeTiming(min_only, computation, "min-only output delay");
  requireBits(min_result.nodes.requireds.values[1], 1.0, "min-only changed scalar required");

  CsrReferenceInput negative = makeCpuReferenceNetFixture();
  negative.port_constraints.front().input_delay = {-2.0, -2.0};
  for (auto& analysis : negative.start_seeds.front().arrival)
    for (auto& transition : analysis)
      transition = -2.0;
  refreshCpuReferenceIdentity(negative);
  const CsrTimingResult negative_result = computeTiming(negative, computation, "negative endpoint default");
  requireBits(negative_result.nodes.requireds.values[1], 0.0, "negative endpoint default required");
  requireBits(negative_result.nodes.slacks.values[1], 2.0, "negative endpoint slack");

  CsrReferenceInput first_period = makeCpuReferenceNetFixture();
  first_period.clocks = {ClockConstraintModel{"A", "A", {}, -3.0, {0.0, 0.0}, false}, ClockConstraintModel{"Z", "Z", {}, 20.0, {0.0, 0.0}, false}};
  first_period.port_constraints.back().clock_name = "missing";
  first_period.port_constraints.back().output_delay = {2.0, std::nullopt};
  first_period.start_seeds.front().clock_tag = "A";
  first_period.endpoint_required.front() = {NodeId{1}, "missing", -3.0, {2.0, std::nullopt}};
  first_period.clock_tags = {"A", "Z", "missing"};
  refreshCpuReferenceIdentity(first_period);
  const CsrTimingResult first_period_result = computeTiming(first_period, computation, "first frozen period");
  requireBits(first_period_result.nodes.requireds.values[1], -5.0, "first frozen period required");

  for (const auto& [period, delay, expected] : {std::array{-3.0, 0.0, -3.0}, std::array{0.0, -2.0, 2.0}}) {
    CsrReferenceInput finite = makeCpuReferenceNetFixture();
    configureOutputConstraint(finite, delay, std::nullopt);
    finite.clocks.front().period = period;
    finite.endpoint_required.front().clock_period = period;
    refreshCpuReferenceIdentity(finite);
    const CsrTimingResult finite_result = computeTiming(finite, computation, "negative/zero required arithmetic");
    requireBits(finite_result.nodes.requireds.values[1], expected, "negative/zero period-delay required");
  }

  const CsrReferenceInput scalar_graph = makeCpuReferenceCellFixture();
  const CsrTimingResult scalar_graph_result = computeTiming(scalar_graph, computation, "reverse scalar versus graph");
  requireBits(scalar_graph_result.arcs.delays.values[1], 1.0, "reverse scalar fixture scalar delay");
  requireBits(scalar_graph_result.arcs.graph_delays.values[8], 2.0, "reverse scalar fixture graph delay");
  requireBits(scalar_graph_result.nodes.requireds.values[1], 1.0, "reverse required did not use immutable scalar");
}

void testClockNameOverwriteBranches()
{
  CsrComputationResult computation;
  const CsrReferenceInput isolated = isolatedClockSourceInput();
  const CsrTimingResult isolated_result = computeTiming(isolated, computation, "isolated clock source");
  requireCpuReference(isolated_result.nodes.is_clock_point.values[0] == 1, "isolated source lost is-clock");
  requireCpuReference(isolated_result.nodes.clock_names.presence[0] == 1 && isolated_result.nodes.clock_names.values[0] == "B",
                      "isolated source did not retain first map key");
  requireBits(isolated_result.nodes.arrivals.values[0], -std::numeric_limits<double>::infinity(), "isolated source arrival");
  requireCpuReference(isolated_result.nodes.predecessor_nodes.presence[0] == 0, "isolated source gained predecessor");
  for (std::size_t lane = 0; lane < kAnalysisCount * kTransitionCount; ++lane) {
    requireCpuReference(isolated_result.effective_clock.arrivals.presence[lane] == 1 && isolated_result.physical_clock.arrivals.presence[lane] == 1,
                        "isolated source clock arrival lane is absent");
    requireBits(isolated_result.effective_clock.arrivals.values[lane], 0.0, "isolated effective clock arrival");
    requireBits(isolated_result.physical_clock.arrivals.values[lane], 0.0, "isolated physical clock arrival");
  }

  const CsrReferenceInput direct = directStartClockInput();
  const CsrTimingResult direct_result = computeTiming(direct, computation, "direct start clock source");
  requireCpuReference(direct_result.nodes.clock_names.presence[0] == 1 && direct_result.nodes.clock_names.values[0] == "E",
                      "start seed did not overwrite map key");
  const std::size_t fall_entry = direct_result.paths.row_offsets[1];
  requireBits(direct_result.paths.launches.values[fall_entry], 7.0, "second raw-source clock fall edge");

  const CsrReferenceInput incoming = incomingClockSourceInput();
  const CsrTimingResult incoming_result = computeTiming(incoming, computation, "incoming clock source");
  requireCpuReference(incoming_result.nodes.is_clock_point.values[1] == 1, "incoming source lost is-clock");
  requireCpuReference(incoming_result.nodes.predecessor_nodes.presence[1] == 1 && incoming_result.nodes.predecessor_nodes.values[1] == NodeId{0},
                      "incoming source scalar predecessor differs");
  requireCpuReference(incoming_result.nodes.clock_names.presence[1] == 1 && incoming_result.nodes.clock_names.values[1] == "A",
                      "incoming scalar winner did not overwrite map key");
  requireBits(incoming_result.nodes.arrivals.values[1], 1.0, "incoming clock-source scalar arrival");
  for (std::size_t lane = 0; lane < kAnalysisCount * kTransitionCount; ++lane)
    requireCpuReference(incoming_result.effective_clock.arrivals.presence[nodeLane(1, lane / 2, lane % 2)] == 1, "incoming source lost clock SoA");
}

void testTopologyMatrix()
{
  CsrComputationResult computation;
  const CsrTimingResult empty = computeTiming(dagInput(0, {}), computation, "empty DAG");
  requireCpuReference(empty.level_offsets == std::vector<std::uint64_t>{0} && empty.paths.row_offsets == std::vector<std::uint64_t>{0}
                          && empty.arcs.timing_arc_row_offsets == std::vector<std::uint64_t>{0},
                      "empty DAG exact shape differs");

  const CsrTimingResult singleton = computeTiming(dagInput(1, {}), computation, "singleton DAG");
  requireCpuReference(singleton.level_offsets == std::vector<std::uint64_t>({0, 1}) && singleton.node_to_level == std::vector<std::uint64_t>({1})
                          && singleton.nodes.arrivals.values.size() == 1,
                      "singleton required shape differs");
  requireBits(singleton.nodes.arrivals.values[0], 0.0, "singleton seed arrival");

  const CsrTimingResult diamond = computeTiming(dagInput(4, {{0, 1}, {0, 2}, {1, 3}, {2, 3}}), computation, "diamond DAG");
  requireCpuReference(diamond.level_offsets == std::vector<std::uint64_t>({0, 1, 3, 4})
                          && diamond.level_nodes == std::vector<NodeId>({NodeId{0}, NodeId{1}, NodeId{2}, NodeId{3}}),
                      "diamond FIFO levels differ");
  requireCpuReference(diamond.nodes.predecessor_nodes.values[3] == NodeId{1} && diamond.nodes.predecessor_arcs.values[3] == ArcId{2},
                      "diamond exact-tie first winner differs");

  const CsrTimingResult multi_root = computeTiming(dagInput(4, {{0, 2}, {1, 3}}), computation, "multi-root DAG");
  requireCpuReference(multi_root.level_offsets == std::vector<std::uint64_t>({0, 2, 4})
                          && multi_root.level_nodes == std::vector<NodeId>({NodeId{0}, NodeId{1}, NodeId{2}, NodeId{3}}),
                      "multi-root FIFO order differs");

  const CsrTimingResult parallel = computeTiming(dagInput(2, {{0, 1}, {0, 1}}), computation, "parallel DAG");
  requireCpuReference(parallel.nodes.predecessor_arcs.presence[1] == 1 && parallel.nodes.predecessor_arcs.values[1] == ArcId{0},
                      "parallel row did not preserve first winner");

  for (const DagArc inactive : {DagArc{0, 1, true, false}, DagArc{0, 1, false, true}}) {
    const CsrTimingResult result = computeTiming(dagInput(2, {inactive}), computation, "inactive DAG");
    requireCpuReference(result.level_offsets == std::vector<std::uint64_t>({0, 2}) && result.nodes.predecessor_nodes.presence[1] == 0,
                        "disabled/loop-disabled arc affected propagation");
    requireBits(result.nodes.arrivals.values[1], -std::numeric_limits<double>::infinity(), "inactive sink arrival");
  }

  const CsrTimingResult unreachable = computeTiming(dagInput(3, {{0, 1}, {1, 2, true, false}}), computation, "unreachable DAG");
  requireBits(unreachable.nodes.arrivals.values[2], -std::numeric_limits<double>::infinity(), "unreachable arrival");
  requireBits(unreachable.nodes.requireds.values[0], std::numeric_limits<double>::infinity(), "unreachable component required");
  requireBits(unreachable.nodes.requireds.values[1], std::numeric_limits<double>::infinity(), "unreachable sink required");
  requireBits(unreachable.nodes.slacks.values[1], 0.0, "unreachable slack");

  const CsrTimingResult no_seed = computeTiming(dagInput(2, {{0, 1}, {1, 0, false, true}}), computation, "no-seed complete DAG");
  requireCpuReference(no_seed.level_nodes == std::vector<NodeId>({NodeId{0}, NodeId{1}}), "no-seed DAG was not complete");
  for (std::size_t node = 0; node < 2; ++node) {
    requireBits(no_seed.nodes.arrivals.values[node], -std::numeric_limits<double>::infinity(), "no-seed arrival");
    requireBits(no_seed.nodes.requireds.values[node], std::numeric_limits<double>::infinity(), "no-seed required");
    requireBits(no_seed.nodes.slacks.values[node], 0.0, "no-seed slack");
  }
}

void testWinnerEpsilonBoundaries()
{
  struct Boundary
  {
    double candidate;
    bool second_wins;
  };
  CsrComputationResult computation;
  const double scalar_equal = 1.0e-9;
  for (const Boundary test : {Boundary{std::nextafter(scalar_equal, 0.0), false}, Boundary{scalar_equal, false},
                              Boundary{std::nextafter(scalar_equal, std::numeric_limits<double>::infinity()), true}}) {
    const CsrTimingResult result = computeTiming(parallelBoundaryInput(test.candidate, 1.0), computation, "scalar epsilon boundary");
    requireCpuReference(result.nodes.predecessor_arcs.values[2] == ArcId{test.second_wins ? 2U : 1U}, "scalar epsilon winner differs");
  }

  for (const Boundary test : {Boundary{1.0e-6 - 1.0e-15, false}, Boundary{1.0e-6, false}, Boundary{1.0e-6 + 1.0e-15, true}}) {
    const CsrTimingResult result = computeTiming(parallelBoundaryInput(test.candidate, 1.0), computation, "path epsilon boundary");
    const std::size_t entry = result.paths.row_offsets[16];
    requireCpuReference(result.paths.predecessor_arcs.values[entry] == ArcId{test.second_wins ? 2U : 1U}, "path epsilon winner differs");
  }

  const double slew_equal = 1.0 + 1.0e-6;
  for (const Boundary test : {Boundary{std::nextafter(slew_equal, 1.0), false}, Boundary{slew_equal, false},
                              Boundary{std::nextafter(slew_equal, std::numeric_limits<double>::infinity()), true}}) {
    const CsrTimingResult result = computeTiming(parallelBoundaryInput(0.0, test.candidate), computation, "slew epsilon boundary");
    const double expected = test.second_wins ? test.candidate : 1.0;
    requireBits(result.data_slews.values[nodeLane(2, 0, 0)], expected, "slew epsilon winner");
    const std::size_t entry = result.paths.row_offsets[16];
    requireCpuReference(result.paths.predecessor_arcs.values[entry] == ArcId{1}, "path winner did not remain first arc");
    requireBits(result.paths.slews.values[entry], expected, "path did not copy global sink-slew winner");
  }
}

void testCandidateOrderIndependentOfPhysicalSlots()
{
  for (auto input : {parallelBoundaryInput(0.5e-9, 1.0), parallelBoundaryInput(0.5e-6, 1.0),
                     parallelBoundaryInput(0.0, 1.0 + 0.5e-6), dagInput(4, {{0, 1}, {0, 2}, {1, 3}, {2, 3}})}) {
    CsrComputationResult computation;
    auto expected = computeTiming(input, computation, "ordered adjacency baseline");
    std::vector<std::uint64_t> reversed;
    for (const auto& arc : input.arcs)
      reversed.push_back(input.arcs.size() - 1 - arc.native_traversal_rank);
    for (auto& arc : input.arcs) {
      std::vector<std::uint64_t> parallel_slots;
      for (std::size_t i = 0; i < input.arcs.size(); ++i)
        if (input.arcs[i].key.base == arc.key.base)
          parallel_slots.push_back(reversed[i]);
      std::ranges::sort(parallel_slots);
      arc.native_traversal_rank = parallel_slots.at(arc.key.parallel_ordinal);
    }
    refreshCpuReferenceIdentity(input);
    requireStrict(input, "nonmonotonic physical slots");
    const auto actual = computeTiming(input, computation, "nonmonotonic physical slots");
    expected.source_identity = actual.source_identity;
    requireExactTimingResult(actual, expected, "physical slot permutation changed FIFO, winner or path");
  }
}

void testExplicitParallelCandidateOrder()
{
  auto input = parallelBoundaryInput(0.5e-9, 1.0);
  std::swap(input.out_arc_ids[1], input.out_arc_ids[2]);
  std::swap(input.in_arc_ids[1], input.in_arc_ids[2]);
  refreshCpuReferenceIdentity(input);
  requireStrict(input, "explicit parallel candidate order");
  CsrComputationResult computation;
  const auto result = computeTiming(input, computation, "explicit parallel candidate order");
  requireCpuReference(result.nodes.predecessor_arcs.values[2] == ArcId{2}
                          && result.paths.predecessor_arcs.values[result.paths.row_offsets[16]] == ArcId{2},
                      "epsilon winner followed physical rank instead of explicit candidate order");
}

void testFrozenPathDetails()
{
  CsrComputationResult computation;
  for (const auto values : {std::array{0.5e-15, 1.0e-15}, std::array{-0.5e-15, -1.0e-15}}) {
    CsrReferenceInput input = makeCpuReferenceNetFixture();
    input.port_constraints.front().input_delay = {values[0], values[0]};
    for (auto& analysis : input.start_seeds.front().arrival)
      for (auto& value : analysis)
        value = values[0];
    refreshCpuReferenceIdentity(input);
    const CsrTimingResult result = computeTiming(input, computation, "halfway path rounding");
    requireBits(result.paths.arrivals.values[result.paths.row_offsets[8]], values[1], "halfway did not round away from zero");
  }
  CsrReferenceInput scalar = makeCpuReferenceCellFixture();
  auto& scalar_arc = scalar.arcs[1];
  auto children = scalar_arc.transfer.children();
  auto& missing = children.front();
  for (std::size_t output = 0; output < kTransitionCount; ++output) {
    missing.signature.delay_tables[output] = {};
    missing.signature.slew_tables[output] = {};
    missing.delay_tables[output] = {};
    missing.slew_tables[output] = {};
  }
  scalar_arc.key.base.library_arcs = {missing.signature};
  scalar.arc_ids_by_key[1].key = scalar_arc.key;
  scalar_arc.transfer.setChildren(children);
  scalar.referenced_library_cells.front().cell_arcs.front().transfer = scalar_arc.transfer;
  refreshCpuReferenceIdentity(scalar);
  requireStrict(scalar, "scalar-without-IO fixture");
  const CsrTimingResult scalar_result = computeTiming(scalar, computation, "scalar without IO");
  requireCpuReference(scalar_result.nodes.predecessor_arcs.values[2] == ArcId{1} && scalar_result.paths.row_offsets[16] == scalar_result.paths.row_offsets[24],
                      "scalar/path relation independence differs");
  const double winning_slew = std::nextafter(1.0 + 1.0e-6, std::numeric_limits<double>::infinity());
  CsrReferenceInput downstream = parallelBoundaryInput(0.0, winning_slew);
  NodeRecord output = downstream.nodes[2];
  downstream.nodes[2].is_endpoint = false;
  output.id = NodeId{3};
  output.key = pin("u1:Z", "Z");
  output.is_endpoint = true;
  output.is_output_pin = false;
  downstream.nodes.push_back(output);
  downstream.node_ids_by_key.push_back({output.key, NodeId{3}});
  downstream.instances.front().ordered_pins.push_back(NodeId{3});
  downstream.clock_owner_by_node.push_back(std::nullopt);
  downstream.cell_pin_capacitances.push_back({NodeId{3}, 0.1, {}});
  auto& library = downstream.referenced_library_cells.front();
  LibraryPortModel port_z = library.ports.back();
  port_z.name = "Z";
  library.ports.push_back(port_z);
  ArcRecord arc = downstream.arcs[1];
  arc.id = ArcId{3};
  arc.source = NodeId{2};
  arc.sink = NodeId{3};
  arc.native_traversal_rank = 3;
  arc.key.base.source = downstream.nodes[2].key;
  arc.key.base.sink = output.key;
  arc.key.base.library_source_port = "Y";
  arc.key.base.library_sink_port = "Z";
  ista::csr::test::mutateChildren(arc.transfer, [&](auto& children) { for (auto& table : children.front().delay_tables)
    table.values = {0.0, 1.0, 2.0}; });
  downstream.arcs.push_back(arc);
  downstream.arc_ids_by_key.push_back({arc.key, ArcId{3}});
  downstream.out_row_offsets = {0, 1, 3, 4, 4};
  downstream.out_arc_ids = {ArcId{0}, ArcId{1}, ArcId{2}, ArcId{3}};
  downstream.in_row_offsets = {0, 0, 1, 3, 4};
  downstream.in_arc_ids = downstream.out_arc_ids;
  downstream.ordered_end_ids = {NodeId{3}};
  library.cell_arcs.push_back({"Y", "Z", arc.transfer});
  refreshCpuReferenceIdentity(downstream);
  requireStrict(downstream, "downstream full-slew fixture");
  const CsrTimingResult downstream_result = computeTiming(downstream, computation, "downstream full-slew LUT");
  requireBits(downstream_result.arcs.graph_delays.values[24], winning_slew, "downstream LUT did not read completed sink slew");
}

}  // namespace

void runCpuReferencePropagationContracts()
{
  testChainLiteral();
  testRequiredLiterals();
  testClockNameOverwriteBranches();
  testTopologyMatrix();
  testWinnerEpsilonBoundaries();
  testCandidateOrderIndependentOfPhysicalSlots();
  testExplicitParallelCandidateOrder();
  testFrozenPathDetails();
}

}  // namespace ista::csr::test
