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
#include "CsrModelTestMutation.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrFingerprint.hpp"

namespace {

using namespace ista::csr;

template <typename T>
concept HasPersistedStorage = requires(T value) { value.storage; };

static_assert(!HasPersistedStorage<CsrReferenceInput>);

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

NodeKey port(std::string name)
{
  return NodeKey{NodeKind::kPort, std::move(name), {}, {}};
}

NodeKey pin(std::string full_name, std::string pin_name)
{
  return NodeKey{NodeKind::kInstancePin, std::move(full_name), "u1", std::move(pin_name)};
}

LibraryArcSignature librarySignature()
{
  LibraryArcSignature signature;
  signature.library_name = "slow.lib";
  signature.arc_index = -1;
  signature.sense = TimingSense::kPositiveUnate;
  signature.trigger_transition = Transition::kRise;
  signature.check_transition = Transition::kNone;
  signature.delay_tables[0] = {true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance};
  return signature;
}

CsrSnapshotIdentity identity()
{
  FingerprintPayloads payloads;
  payloads.topology = {1};
  payloads.clock_constraint = {2};
  payloads.library = {3};
  payloads.corner = {4};
  payloads.parasitic = {5};
  payloads.timing_input = {6};
  return makeSnapshotIdentity(9, 0, payloads);
}

CsrReferenceInput inputFixture()
{
  CsrReferenceInput input;
  input.source_identity = identity();
  input.analysis_context = {"slow", "25", "rc0"};
  input.nodes = {NodeRecord{.id = NodeId{0}, .key = port("a"), .direction = PinDirection::kInput, .instance_cell = {}, .net_name = {}, .is_port = true, .is_startpoint = true},
                 NodeRecord{.id = NodeId{1}, .key = port("bb"), .direction = PinDirection::kOutput, .instance_cell = {}, .net_name = {}, .is_port = true, .is_endpoint = true},
                 NodeRecord{.id = NodeId{2},
                            .key = pin("u1:A", "A"),
                            .direction = PinDirection::kInput,
                            .instance_cell = "BUF_X1",
                            .net_name = "n_in",
                            .is_startpoint = true,
                            .is_data_pin = true},
                 NodeRecord{.id = NodeId{3},
                            .key = pin("u1:Y", "Y"),
                            .direction = PinDirection::kOutput,
                            .instance_cell = "BUF_X1",
                            .net_name = "n_out",
                            .is_endpoint = true,
                            .is_output_pin = true}};
  for (const auto& node : input.nodes)
    input.node_ids_by_key.push_back({node.key, node.id});
  input.instances = {{"u1", "BUF_X1", {NodeId{2}, NodeId{3}}, false, false, false, false}, {"u2", "INV_X1", {NodeId{1}}, false, false, true, true}};

  ChildTimingArcModel child;
  child.signature = librarySignature();
  child.time_unit_scale = 2.0;
  child.capacitance_unit_scale = 3.0;
  child.slew_derate = 0.8;
  child.delay_tables[0] = {true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.1}, {0.2}}, {0.3}};
  ArcKeyBase base{.source = input.nodes[2].key,
                  .sink = input.nodes[3].key,
                  .kind = ArcKind::kCell,
                  .owner_name = "u1",
                  .library_source_port = "A",
                  .library_sink_port = "Y",
                  .library_arcs = {child.signature}};
  ArcRecord arc{.id = ArcId{0},
                .key = ArcKey{base, 0},
                .source = NodeId{2},
                .sink = NodeId{3},
                .kind = ArcKind::kCell,
                .native_traversal_rank = 0,
                .eligibility = {true, true, true, false},
                .transfer = {},
                .output_load = {}};
  arc.transfer.delay = 0.1;
  arc.transfer.delay_min = 0.05;
  arc.transfer.delay_max = 0.2;
  arc.transfer.setChildren({child});
  arc.output_load.reset();
  ArcRecord parallel_arc = arc;
  parallel_arc.id = ArcId{1};
  parallel_arc.key.parallel_ordinal = 1;
  parallel_arc.native_traversal_rank = 1;
  input.arcs = {arc, parallel_arc};
  input.arc_ids_by_key = {{arc.key, arc.id}, {parallel_arc.key, parallel_arc.id}};
  input.out_row_offsets = {0, 0, 0, 2, 2};
  input.out_arc_ids = {ArcId{0}, ArcId{1}};
  input.in_row_offsets = {0, 0, 0, 0, 2};
  input.in_arc_ids = {ArcId{0}, ArcId{1}};
  input.ordered_root_ids = {NodeId{0}, NodeId{2}};
  input.ordered_start_ids = {NodeId{0}, NodeId{2}};
  input.ordered_end_ids = {NodeId{1}, NodeId{3}};
  input.clock_tags = {"clk", "clk2", "embedded", "embedded2", "tag2"};
  input.clocks
      = {ClockConstraintModel{"clk", "embedded", {RawNodeReference{"a", NodeId{0}}, RawNodeReference{"missing", std::nullopt}}, 10.0, {0.0, 5.0}, false},
         ClockConstraintModel{"clk2", "embedded2", {RawNodeReference{"bb", NodeId{1}}}, 12.0, {1.0, 6.0}, true}};
  input.clock_owner_by_node = {std::optional<std::string>{"clk"}, std::optional<std::string>{"clk2"}, std::nullopt, std::nullopt};

  PortConstraintModel constraint;
  constraint.map_key = "a";
  constraint.embedded_port_name = "embedded_a";
  constraint.resolved_node = NodeId{0};
  constraint.input_delay = {1.0, 2.0};
  constraint.input_transition = 3.0;
  constraint.output_delay = {4.0, 5.0};
  constraint.output_load = 6.0;
  constraint.clock_name = "clk";
  PortConstraintModel second_constraint = constraint;
  second_constraint.map_key = "bb";
  second_constraint.embedded_port_name = "embedded_bb";
  second_constraint.resolved_node = NodeId{1};
  second_constraint.clock_name = "clk2";
  input.port_constraints = {constraint, second_constraint};
  input.case_analysis_constraints = {{"u1:A", NodeId{2}, true}, {"u1:Y", NodeId{3}, false}};

  StartSeedModel seed;
  seed.seed_node = NodeId{0};
  seed.path_start_node = NodeId{0};
  seed.path_source = PathSource::kInput;
  seed.clock_tag = "clk";
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      seed.arrival[analysis][transition] = 0.1 + analysis * 2 + transition;
      seed.slew[analysis][transition] = 0.2 + analysis * 2 + transition;
    }
  seed.output_load = ArcOutputLoadMatrix{};
  StartSeedModel second_seed = seed;
  second_seed.seed_node = NodeId{2};
  second_seed.path_start_node = NodeId{2};
  second_seed.clock_tag = "tag2";
  second_seed.path_source.reset();
  second_seed.output_load.reset();
  for (auto& analysis : second_seed.arrival)
    for (auto& value : analysis)
      value.reset();
  for (auto& analysis : second_seed.slew)
    for (auto& value : analysis)
      value.reset();
  second_seed.slew[1][1] = 0.2;
  input.start_seeds = {seed, second_seed};
  input.endpoint_required = {{NodeId{3}, "clk", 10.0, {1.0, 2.0}}, {NodeId{1}, "clk2", 12.0, {2.0, 3.0}}};

  CellPinCapacitanceModel capacitance;
  capacitance.node = NodeId{2};
  capacitance.default_capacitance = 0.4;
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition)
      capacitance.transition_capacitance[analysis][transition] = 0.5 + analysis * 2 + transition;
  CellPinCapacitanceModel second_capacitance = capacitance;
  second_capacitance.node = NodeId{3};
  second_capacitance.default_capacitance = 0.9;
  input.cell_pin_capacitances = {capacitance, second_capacitance};
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kParasiticNet, CsrReferenceUnsupportedReason::kCaseAnalysis};
  return input;
}

struct MutationCase
{
  std::string name;
  std::function<void(CsrReferenceInput&)> mutate;
};

void addTopLevelCases(std::vector<MutationCase>& cases)
{
  cases.emplace_back("schema", [](auto& value) { value.schema_version = "unsupported-version"; });
  cases.emplace_back("context liberty", [](auto& value) { value.analysis_context.liberty_corner = "fast"; });
  cases.emplace_back("context temperature", [](auto& value) { value.analysis_context.temperature = "125"; });
  cases.emplace_back("context rc", [](auto& value) { value.analysis_context.rc_corner = "rc1"; });
  cases.emplace_back("analysis order", [](auto& value) { std::ranges::reverse(value.analysis_order); });
  cases.emplace_back("path-source order", [](auto& value) { std::ranges::reverse(value.path_source_order); });
  cases.emplace_back("transition order", [](auto& value) { std::ranges::reverse(value.transition_order); });
  cases.emplace_back("nodes container length", [](auto& value) { value.nodes.pop_back(); });
  cases.emplace_back("instances container length", [](auto& value) { value.instances.clear(); });
  cases.emplace_back("arcs container length", [](auto& value) { value.arcs.pop_back(); });
  cases.emplace_back("clocks container length", [](auto& value) { value.clocks.clear(); });
  cases.emplace_back("port constraints container length", [](auto& value) { value.port_constraints.clear(); });
  cases.emplace_back("case constraints container length", [](auto& value) { value.case_analysis_constraints.clear(); });
  cases.emplace_back("start seeds container length", [](auto& value) { value.start_seeds.clear(); });
  cases.emplace_back("endpoint constraints container length", [](auto& value) { value.endpoint_required.clear(); });
  cases.emplace_back("capacitance container length", [](auto& value) { value.cell_pin_capacitances.clear(); });
  cases.emplace_back("nodes order", [](auto& value) { std::ranges::reverse(value.nodes); });
  cases.emplace_back("node lookup order", [](auto& value) { std::ranges::reverse(value.node_ids_by_key); });
  cases.emplace_back("instances order", [](auto& value) { std::ranges::reverse(value.instances); });
  cases.emplace_back("arcs order", [](auto& value) { std::ranges::reverse(value.arcs); });
  cases.emplace_back("arc lookup order", [](auto& value) { std::ranges::reverse(value.arc_ids_by_key); });
  cases.emplace_back("clocks order", [](auto& value) { std::ranges::reverse(value.clocks); });
  cases.emplace_back("port constraints order", [](auto& value) { std::ranges::reverse(value.port_constraints); });
  cases.emplace_back("case constraints order", [](auto& value) { std::ranges::reverse(value.case_analysis_constraints); });
  cases.emplace_back("start seeds order", [](auto& value) { std::ranges::reverse(value.start_seeds); });
  cases.emplace_back("endpoint constraints order", [](auto& value) { std::ranges::reverse(value.endpoint_required); });
  cases.emplace_back("capacitance order", [](auto& value) { std::ranges::reverse(value.cell_pin_capacitances); });
}

void addNodeAndInstanceCases(std::vector<MutationCase>& cases)
{
  cases.emplace_back("node id", [](auto& value) { value.nodes[2].id = NodeId{9}; });
  cases.emplace_back("node key kind", [](auto& value) { value.nodes[2].key.kind = NodeKind::kPort; });
  cases.emplace_back("node key full", [](auto& value) { value.nodes[2].key.full_name = "u1:B"; });
  cases.emplace_back("node key instance", [](auto& value) { value.nodes[2].key.instance_name = "u2"; });
  cases.emplace_back("node key pin", [](auto& value) { value.nodes[2].key.pin_name = "B"; });
  cases.emplace_back("node direction", [](auto& value) { value.nodes[2].direction = PinDirection::kInout; });
  cases.emplace_back("node cell", [](auto& value) { value.nodes[2].instance_cell = "BUF_X2"; });
  cases.emplace_back("node net", [](auto& value) { value.nodes[2].net_name = "n2"; });
  using NodeFlag = bool NodeRecord::*;
  const std::array<NodeFlag, 7> flags = {&NodeRecord::is_port,      &NodeRecord::is_startpoint, &NodeRecord::is_endpoint,  &NodeRecord::is_sequential,
                                         &NodeRecord::is_clock_pin, &NodeRecord::is_data_pin,   &NodeRecord::is_output_pin};
  for (std::size_t index = 0; index < flags.size(); ++index)
    cases.emplace_back("node flag " + std::to_string(index), [member = flags[index]](auto& value) { value.nodes[2].*member = !(value.nodes[2].*member); });
  cases.emplace_back("node lookup key", [](auto& value) { value.node_ids_by_key[2].key.full_name = "lookup"; });
  cases.emplace_back("node lookup kind", [](auto& value) { value.node_ids_by_key[2].key.kind = NodeKind::kPort; });
  cases.emplace_back("node lookup instance", [](auto& value) { value.node_ids_by_key[2].key.instance_name = "u2"; });
  cases.emplace_back("node lookup pin", [](auto& value) { value.node_ids_by_key[2].key.pin_name = "B"; });
  cases.emplace_back("node lookup id", [](auto& value) { value.node_ids_by_key[2].id = NodeId{9}; });
  cases.emplace_back("node lookup length", [](auto& value) { value.node_ids_by_key.pop_back(); });

  cases.emplace_back("instance name", [](auto& value) { value.instances[0].instance_name = "u2"; });
  cases.emplace_back("instance cell", [](auto& value) { value.instances[0].cell_name = "BUF_X2"; });
  cases.emplace_back("instance pins length", [](auto& value) { value.instances[0].ordered_pins.pop_back(); });
  cases.emplace_back("instance pins order", [](auto& value) { std::ranges::reverse(value.instances[0].ordered_pins); });
  cases.emplace_back("instance sequential", [](auto& value) { value.instances[0].is_sequential = true; });
  cases.emplace_back("instance timing checks", [](auto& value) { value.instances[0].has_timing_checks = true; });
  cases.emplace_back("instance clock gating", [](auto& value) { value.instances[0].is_clock_gating = true; });
  cases.emplace_back("instance macro", [](auto& value) { value.instances[0].is_macro = true; });
}

void addArcCases(std::vector<MutationCase>& cases)
{
  cases.emplace_back("arc id", [](auto& value) { value.arcs[0].id = ArcId{9}; });
  cases.emplace_back("arc key owner", [](auto& value) { value.arcs[0].key.base.owner_name = "u2"; });
  cases.emplace_back("arc key ordinal", [](auto& value) { value.arcs[0].key.parallel_ordinal = 1; });
  cases.emplace_back("arc source", [](auto& value) { value.arcs[0].source = NodeId{0}; });
  cases.emplace_back("arc sink", [](auto& value) { value.arcs[0].sink = NodeId{1}; });
  cases.emplace_back("arc kind", [](auto& value) { value.arcs[0].kind = ArcKind::kNet; });
  cases.emplace_back("arc rank", [](auto& value) { value.arcs[0].native_traversal_rank = 1; });
  cases.emplace_back("arc clock", [](auto& value) { value.arcs[0].is_clock_arc = true; });
  cases.emplace_back("arc disabled", [](auto& value) { value.arcs[0].is_disabled = true; });
  cases.emplace_back("arc loop disabled", [](auto& value) { value.arcs[0].is_loop_disabled = true; });
  using Eligibility = bool ArcEligibility::*;
  for (auto member :
       std::array<Eligibility, 4>{&ArcEligibility::levelization, &ArcEligibility::data_arrival, &ArcEligibility::data_slew, &ArcEligibility::clock})
    cases.emplace_back("arc eligibility", [member](auto& value) { value.arcs[0].eligibility.*member = !(value.arcs[0].eligibility.*member); });
  cases.emplace_back("arc output load presence", [](auto& value) { value.arcs[0].output_load = ArcOutputLoadMatrix{}; });

  for (auto member : std::array{&CellTransferModel::delay, &CellTransferModel::delay_min, &CellTransferModel::delay_max})
    cases.emplace_back("transfer scalar", [member](auto& value) { value.arcs[0].transfer.*member += 1.0; });
  for (auto member : std::array{&CellTransferModel::is_clock_arc, &CellTransferModel::is_disabled, &CellTransferModel::is_clear_or_preset})
    cases.emplace_back("transfer flag", [member](auto& value) { value.arcs[0].transfer.*member = !(value.arcs[0].transfer.*member); });
  for (auto member : std::array{&ChildTimingArcModel::time_unit_scale, &ChildTimingArcModel::capacitance_unit_scale, &ChildTimingArcModel::slew_derate})
    cases.emplace_back("child scale", [member](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].*member += 1.0; }); });
  using Threshold = std::array<double, kTransitionCount> ChildTimingArcModel::*;
  for (auto member : std::array<Threshold, 4>{&ChildTimingArcModel::slew_lower_threshold_pct, &ChildTimingArcModel::slew_upper_threshold_pct,
                                              &ChildTimingArcModel::input_threshold_pct, &ChildTimingArcModel::output_threshold_pct})
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition)
      cases.emplace_back("child threshold", [=](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { (children[0].*member)[transition] += 0.01; }); });
  cases.emplace_back("child signature", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].signature.library_name = "fast.lib"; }); });
  cases.emplace_back("delay table presence", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].delay_tables[0].present = false; }); });
  cases.emplace_back("delay table variable", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].delay_tables[0].variable2 = TableVariable::kNone; }); });
  cases.emplace_back("delay table axes", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].delay_tables[0].axes[0][0] += 1.0; }); });
  cases.emplace_back("delay table values", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].delay_tables[0].values[0] += 1.0; }); });
  cases.emplace_back("slew table presence", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].slew_tables[0].present = true; }); });

  cases.emplace_back("arc lookup key", [](auto& value) { value.arc_ids_by_key[0].key.base.owner_name = "lookup"; });
  cases.emplace_back("arc lookup source", [](auto& value) { value.arc_ids_by_key[0].key.base.source.full_name = "source"; });
  cases.emplace_back("arc lookup sink", [](auto& value) { value.arc_ids_by_key[0].key.base.sink.full_name = "sink"; });
  cases.emplace_back("arc lookup kind", [](auto& value) { value.arc_ids_by_key[0].key.base.kind = ArcKind::kNet; });
  cases.emplace_back("arc lookup source port", [](auto& value) { value.arc_ids_by_key[0].key.base.library_source_port = "B"; });
  cases.emplace_back("arc lookup sink port", [](auto& value) { value.arc_ids_by_key[0].key.base.library_sink_port = "Z"; });
  cases.emplace_back("arc lookup library", [](auto& value) { value.arc_ids_by_key[0].key.base.library_arcs[0].library_name = "fast.lib"; });
  cases.emplace_back("arc lookup ordinal", [](auto& value) { value.arc_ids_by_key[0].key.parallel_ordinal = 1; });
  cases.emplace_back("arc lookup id", [](auto& value) { value.arc_ids_by_key[0].id = ArcId{9}; });
  cases.emplace_back("arc lookup length", [](auto& value) { value.arc_ids_by_key.clear(); });
}

void addProjectionAndConstraintCases(std::vector<MutationCase>& cases)
{
  cases.emplace_back("out offsets", [](auto& value) { ++value.out_row_offsets.back(); });
  cases.emplace_back("out offsets length", [](auto& value) { value.out_row_offsets.pop_back(); });
  cases.emplace_back("out ids", [](auto& value) { value.out_arc_ids[0] = ArcId{9}; });
  cases.emplace_back("out ids length", [](auto& value) { value.out_arc_ids.pop_back(); });
  cases.emplace_back("out row arc order", [](auto& value) { std::ranges::reverse(value.out_arc_ids); });
  cases.emplace_back("in offsets", [](auto& value) { ++value.in_row_offsets.back(); });
  cases.emplace_back("in offsets length", [](auto& value) { value.in_row_offsets.pop_back(); });
  cases.emplace_back("in ids", [](auto& value) { value.in_arc_ids[0] = ArcId{9}; });
  cases.emplace_back("in ids length", [](auto& value) { value.in_arc_ids.pop_back(); });
  cases.emplace_back("in row arc order", [](auto& value) { std::ranges::reverse(value.in_arc_ids); });
  cases.emplace_back("root projection", [](auto& value) { value.ordered_root_ids.pop_back(); });
  cases.emplace_back("root projection order", [](auto& value) { std::ranges::reverse(value.ordered_root_ids); });
  cases.emplace_back("start projection", [](auto& value) { value.ordered_start_ids.pop_back(); });
  cases.emplace_back("start projection order", [](auto& value) { std::ranges::reverse(value.ordered_start_ids); });
  cases.emplace_back("end projection", [](auto& value) { value.ordered_end_ids.pop_back(); });
  cases.emplace_back("end projection order", [](auto& value) { std::ranges::reverse(value.ordered_end_ids); });
  cases.emplace_back("clock tag value", [](auto& value) { value.clock_tags[0] = "clk2"; });
  cases.emplace_back("clock tag order", [](auto& value) { std::ranges::reverse(value.clock_tags); });
  cases.emplace_back("clock tag length", [](auto& value) { value.clock_tags.pop_back(); });

  cases.emplace_back("clock map key", [](auto& value) { value.clocks[0].map_key = "clk0"; });
  cases.emplace_back("clock embedded name", [](auto& value) { value.clocks[0].embedded_clock_name = "embedded0"; });
  cases.emplace_back("clock source raw name", [](auto& value) { value.clocks[0].ordered_sources[0].full_name = "bb"; });
  cases.emplace_back("clock source resolved value", [](auto& value) { value.clocks[0].ordered_sources[0].resolved_node = NodeId{1}; });
  cases.emplace_back("clock source resolved presence", [](auto& value) { value.clocks[0].ordered_sources[0].resolved_node.reset(); });
  cases.emplace_back("clock source order", [](auto& value) { std::ranges::reverse(value.clocks[0].ordered_sources); });
  cases.emplace_back("clock source length", [](auto& value) { value.clocks[0].ordered_sources.pop_back(); });
  cases.emplace_back("clock period", [](auto& value) { value.clocks[0].period += 1.0; });
  cases.emplace_back("clock rise", [](auto& value) { value.clocks[0].edges[0] += 1.0; });
  cases.emplace_back("clock fall", [](auto& value) { value.clocks[0].edges[1] += 1.0; });
  cases.emplace_back("clock propagated", [](auto& value) { value.clocks[0].propagated = true; });

  cases.emplace_back("constraint map key", [](auto& value) { value.port_constraints[0].map_key = "aa"; });
  cases.emplace_back("constraint embedded name", [](auto& value) { value.port_constraints[0].embedded_port_name = "embedded_aa"; });
  cases.emplace_back("constraint node value", [](auto& value) { value.port_constraints[0].resolved_node = NodeId{1}; });
  cases.emplace_back("constraint node presence", [](auto& value) { value.port_constraints[0].resolved_node.reset(); });
  cases.emplace_back("constraint clock", [](auto& value) { value.port_constraints[0].clock_name = "clk2"; });
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    cases.emplace_back("input delay value", [=](auto& value) { *value.port_constraints[0].input_delay[analysis] += 1.0; });
    cases.emplace_back("input delay presence", [=](auto& value) { value.port_constraints[0].input_delay[analysis].reset(); });
    cases.emplace_back("output delay value", [=](auto& value) { *value.port_constraints[0].output_delay[analysis] += 1.0; });
    cases.emplace_back("output delay presence", [=](auto& value) { value.port_constraints[0].output_delay[analysis].reset(); });
  }
  cases.emplace_back("input transition value", [](auto& value) { *value.port_constraints[0].input_transition += 1.0; });
  cases.emplace_back("input transition presence", [](auto& value) { value.port_constraints[0].input_transition.reset(); });
  cases.emplace_back("output load value", [](auto& value) { *value.port_constraints[0].output_load += 1.0; });
  cases.emplace_back("output load presence", [](auto& value) { value.port_constraints[0].output_load.reset(); });
}

void addSeedEndpointAndSupportCases(std::vector<MutationCase>& cases)
{
  cases.emplace_back("seed storage node", [](auto& value) { value.start_seeds[0].seed_node = NodeId{2}; });
  cases.emplace_back("seed path start", [](auto& value) { value.start_seeds[0].path_start_node = NodeId{2}; });
  cases.emplace_back("seed source value", [](auto& value) { *value.start_seeds[0].path_source = PathSource::kRegister; });
  cases.emplace_back("seed source present to absent", [](auto& value) { value.start_seeds[0].path_source.reset(); });
  cases.emplace_back("seed source absent to present", [](auto& value) { value.start_seeds[1].path_source = PathSource::kInput; });
  cases.emplace_back("seed load presence", [](auto& value) { value.start_seeds[0].output_load.reset(); });
  cases.emplace_back("seed load absent to present", [](auto& value) { value.start_seeds[1].output_load = ArcOutputLoadMatrix{}; });
  for (std::size_t a = 0; a < kAnalysisCount; ++a)
    for (std::size_t t = 0; t < kTransitionCount; ++t)
      cases.emplace_back("seed load lane", [=](auto& value) { (*value.start_seeds[0].output_load)[a][t] = 1.0; });
  cases.emplace_back("seed tag", [](auto& value) { value.start_seeds[0].clock_tag = "tag2"; });
  cases.emplace_back("seed CRPR node", [](auto& value) { value.start_seeds[0].crpr_node = NodeId{2}; });
  cases.emplace_back("seed CRPR transition", [](auto& value) { value.start_seeds[0].crpr_transition = Transition::kRise; });
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      cases.emplace_back("seed arrival presence", [=](auto& value) {
        auto& field = value.start_seeds[0].arrival[analysis][transition];
        field = field ? std::optional<double>{} : std::optional<double>{0.7};
      });
      cases.emplace_back("seed slew presence", [=](auto& value) {
        auto& field = value.start_seeds[0].slew[analysis][transition];
        field = field ? std::optional<double>{} : std::optional<double>{0.8};
      });
    }
  }
  cases.emplace_back("seed arrival value", [](auto& value) { *value.start_seeds[0].arrival[0][0] += 1.0; });
  cases.emplace_back("seed slew value", [](auto& value) { *value.start_seeds[0].slew[1][1] += 1.0; });

  cases.emplace_back("endpoint node", [](auto& value) { value.endpoint_required[0].node = NodeId{1}; });
  cases.emplace_back("endpoint clock", [](auto& value) { value.endpoint_required[0].clock_name = "clk2"; });
  cases.emplace_back("endpoint period", [](auto& value) { value.endpoint_required[0].clock_period += 1.0; });
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    cases.emplace_back("endpoint delay value", [=](auto& value) { *value.endpoint_required[0].output_delay[analysis] += 1.0; });
    cases.emplace_back("endpoint delay presence", [=](auto& value) { value.endpoint_required[0].output_delay[analysis].reset(); });
  }

  cases.emplace_back("cap node", [](auto& value) { value.cell_pin_capacitances[0].node = NodeId{3}; });
  cases.emplace_back("cap default", [](auto& value) { value.cell_pin_capacitances[0].default_capacitance += 1.0; });
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      cases.emplace_back("cap value", [=](auto& value) { *value.cell_pin_capacitances[0].transition_capacitance[analysis][transition] += 1.0; });
      cases.emplace_back("cap presence", [=](auto& value) { value.cell_pin_capacitances[0].transition_capacitance[analysis][transition].reset(); });
    }

  cases.emplace_back("support reason value", [](auto& value) { value.unsupported_reasons[0] = CsrReferenceUnsupportedReason::kCrpr; });
  cases.emplace_back("support reason order", [](auto& value) { std::ranges::reverse(value.unsupported_reasons); });
  cases.emplace_back("support reason length", [](auto& value) { value.unsupported_reasons.pop_back(); });
  cases.emplace_back("parasitic unit", [](auto& value) { value.parasitics.capacitance_unit = "pf"; });
  cases.emplace_back("case constraint raw name", [](auto& value) { value.case_analysis_constraints[0].raw_name = "u1:Y"; });
  cases.emplace_back("case constraint node value", [](auto& value) { value.case_analysis_constraints[0].resolved_node = NodeId{3}; });
  cases.emplace_back("case constraint node presence", [](auto& value) { value.case_analysis_constraints[0].resolved_node.reset(); });
  cases.emplace_back("case constraint value", [](auto& value) { value.case_analysis_constraints[0].value = false; });
  cases.emplace_back("case constraint length", [](auto& value) { value.case_analysis_constraints.clear(); });
}

void testReferenceInputFieldMutations()
{
  std::vector<MutationCase> cases;
  addTopLevelCases(cases);
  addNodeAndInstanceCases(cases);
  addArcCases(cases);
  addProjectionAndConstraintCases(cases);
  addSeedEndpointAndSupportCases(cases);
  const CsrReferenceInput baseline = inputFixture();
  const auto baseline_bytes = canonicalReferenceInputBytes(baseline);
  for (const auto& test : cases) {
    CsrReferenceInput changed = baseline;
    test.mutate(changed);
    const auto changed_bytes = canonicalReferenceInputBytes(changed);
    require(changed_bytes != baseline_bytes, test.name + " did not change canonical bytes");
  }
}

void testExcludedProcessAndCapacityState()
{
  const CsrReferenceInput baseline = inputFixture();
  const auto baseline_bytes = canonicalReferenceInputBytes(baseline);
  const auto requireExcluded = [&](auto mutate, const std::string& field) {
    CsrReferenceInput changed = baseline;
    mutate(changed);
    const auto bytes = canonicalReferenceInputBytes(changed);
    require(bytes == baseline_bytes, field + " changed canonical bytes");
  };
  requireExcluded([](auto& value) { value.source_identity.fingerprint_schema_version = "identity-only"; }, "identity schema");
  requireExcluded([](auto& value) { value.source_identity.session_token = 99; }, "session token");
  requireExcluded([](auto& value) { value.source_identity.graph_generation = 99; }, "generation");
  requireExcluded([](auto& value) { value.source_identity.topology_fingerprint = "identity-only"; }, "topology identity");
  requireExcluded([](auto& value) { value.source_identity.clock_constraint_fingerprint = "identity-only"; }, "clock identity");
  requireExcluded([](auto& value) { value.source_identity.library_fingerprint = "identity-only"; }, "library identity");
  requireExcluded([](auto& value) { value.source_identity.corner_fingerprint = "identity-only"; }, "corner identity");
  requireExcluded([](auto& value) { value.source_identity.parasitic_fingerprint = "identity-only"; }, "parasitic identity");
  requireExcluded([](auto& value) { value.source_identity.timing_input_fingerprint = "identity-only"; }, "timing identity");
  requireExcluded(
      [](auto& value) {
        value.nodes.reserve(64);
        value.node_ids_by_key.reserve(64);
        value.instances.reserve(64);
        value.instances[0].ordered_pins.reserve(64);
        value.arcs.reserve(64);
        value.arcs[0].key.base.library_arcs.reserve(64);
        ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children.reserve(64);
        children[0].delay_tables[0].axes.reserve(64);
        children[0].delay_tables[0].values.reserve(64); });
        value.arc_ids_by_key.reserve(64);
        value.out_row_offsets.reserve(64);
        value.out_arc_ids.reserve(64);
        value.in_row_offsets.reserve(64);
        value.in_arc_ids.reserve(64);
        value.ordered_root_ids.reserve(64);
        value.ordered_start_ids.reserve(64);
        value.ordered_end_ids.reserve(64);
        value.clock_tags.reserve(64);
        value.clocks.reserve(64);
        value.clocks[0].ordered_sources.reserve(64);
        value.port_constraints.reserve(64);
        value.case_analysis_constraints.reserve(64);
        value.start_seeds.reserve(64);
        value.endpoint_required.reserve(64);
        value.cell_pin_capacitances.reserve(64);
        value.unsupported_reasons.reserve(64);
        value.nodes[0].key.full_name.reserve(64);
      },
      "reserved capacity");
}

}  // namespace

int main()
{
  testReferenceInputFieldMutations();
  testExcludedProcessAndCapacityState();
  return 0;
}
