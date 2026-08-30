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
#include <cstdint>
#include <string>

#include "CsrValidatorTestFixture.hpp"
#include "TimingSemanticKey.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

void makeUnsupportedSequentialNull(StartSeedModel& seed)
{
  seed.availability = StartSeedAvailability::kUnsupportedSequential;
  seed.path_start_node = seed.seed_node;
  seed.path_source.reset();
  seed.crpr_node.reset();
  seed.crpr_transition.reset();
  for (auto& analysis : seed.arrival)
    for (auto& value : analysis)
      value.reset();
  for (auto& analysis : seed.slew)
    for (auto& value : analysis)
      value.reset();
}

CsrReferenceInput parasiticInput()
{
  CsrReferenceInput input = referenceInput();
  input.analysis_context.rc_corner = "rc";
  for (auto& arc : input.arcs)
    arc.output_load.reset();
  input.parasitics = {"PF", "OHM", {ParasiticNetModel{"n1", "n1", 0.2, {}, {}}}};
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kParasiticNet};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput sequentialInput()
{
  CsrReferenceInput input = cellInput();
  input.instances[0].is_sequential = true;
  for (auto& node : input.nodes)
    node.is_sequential = true;
  input.referenced_library_cells[0].is_sequential = true;
  makeUnsupportedSequentialNull(input.start_seeds[0]);
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kSequentialInstance};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput registerInput()
{
  CsrReferenceInput input = sequentialInput();
  input.nodes[0].is_clock_pin = true;
  input.nodes[1].is_data_pin = true;
  input.instances[0].has_timing_checks = true;
  auto& cell = input.referenced_library_cells[0];
  cell.clock_pin_name = "A";
  cell.data_pin_name = "Y";
  cell.timing_checks = {LibraryTimingCheckArcModel{"A", "Y", TimingCheckKind::kSetup, 0.0, Transition::kRise, {}}};
  input.timing_check_endpoints = {TimingCheckEndpointModel{"u1", 0, TimingCheckKind::kSetup, "A", "Y", "u1:A", "u1:Y", NodeId{0}, NodeId{1}}};
  input.clocks = {ClockConstraintModel{"clk", "clk", {RawNodeReference{"u1:A", NodeId{0}}}, 10.0, {0.0, 5.0}, false}};
  input.clock_owner_by_node = {std::optional<std::string>{"clk"}, std::nullopt};
  auto& seed = input.start_seeds[0];
  seed.availability = StartSeedAvailability::kUnsupportedSequential;
  seed.path_start_node = NodeId{1};
  seed.path_source = PathSource::kRegister;
  seed.crpr_node = NodeId{0};
  seed.crpr_transition = Transition::kRise;
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kSequentialInstance, CsrReferenceUnsupportedReason::kSequentialLaunch,
                               CsrReferenceUnsupportedReason::kTimingCheck, CsrReferenceUnsupportedReason::kCrpr};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput timingCheckInput()
{
  CsrReferenceInput input = registerInput();
  input.clocks.clear();
  input.clock_owner_by_node = {std::nullopt, std::nullopt};
  makeUnsupportedSequentialNull(input.start_seeds[0]);
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kSequentialInstance, CsrReferenceUnsupportedReason::kTimingCheck};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput gatingInput()
{
  CsrReferenceInput input = cellInput();
  input.instances[0].is_clock_gating = true;
  input.referenced_library_cells[0].is_clock_gating = true;
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kClockGatingOrDivider};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput dividerInput()
{
  CsrReferenceInput input;
  input.source_identity = identity();
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.nodes = {NodeRecord{.id = NodeId{0},
                            .key = NodeKey{NodeKind::kInstancePin, "u1:CK", "u1", "CK"},
                            .direction = PinDirection::kInput,
                            .instance_cell = "DFF_X1",
                            .net_name = {},
                            .is_startpoint = true,
                            .is_sequential = true,
                            .is_clock_pin = true},
                 NodeRecord{.id = NodeId{1},
                            .key = NodeKey{NodeKind::kInstancePin, "u1:Q", "u1", "Q"},
                            .direction = PinDirection::kOutput,
                            .instance_cell = "DFF_X1",
                            .net_name = "n_div",
                            .is_startpoint = true,
                            .is_sequential = true,
                            .is_output_pin = true},
                 NodeRecord{.id = NodeId{2},
                            .key = NodeKey{NodeKind::kInstancePin, "u2:CK", "u2", "CK"},
                            .direction = PinDirection::kInput,
                            .instance_cell = "DFF_X1",
                            .net_name = "n_div",
                            .is_startpoint = true,
                            .is_sequential = true,
                            .is_clock_pin = true},
                 NodeRecord{.id = NodeId{3},
                            .key = NodeKey{NodeKind::kInstancePin, "u2:Q", "u2", "Q"},
                            .direction = PinDirection::kOutput,
                            .instance_cell = "DFF_X1",
                            .net_name = {},
                            .is_startpoint = true,
                            .is_endpoint = true,
                            .is_sequential = true,
                            .is_output_pin = true}};
  std::ranges::sort(input.nodes, [](const NodeRecord& lhs, const NodeRecord& rhs) { return nodeKeyLess(lhs.key, rhs.key); });
  for (std::size_t index = 0; index < input.nodes.size(); ++index) {
    input.nodes[index].id = NodeId{index};
    const auto& node = input.nodes[index];
    input.node_ids_by_key.push_back({node.key, node.id});
  }
  const auto node_id
      = [&](const std::string& full_name) { return std::ranges::find(input.nodes, full_name, [](const NodeRecord& node) { return node.key.full_name; })->id; };
  const NodeId u1_ck = node_id("u1:CK");
  const NodeId u1_q = node_id("u1:Q");
  const NodeId u2_ck = node_id("u2:CK");
  const NodeId u2_q = node_id("u2:Q");
  input.instances = {{"u1", "DFF_X1", {u1_ck, u1_q}, true, true, false, false}, {"u2", "DFF_X1", {u2_ck, u2_q}, true, true, false, false}};

  ArcKeyBase base;
  base.source = input.nodes[u1_q.value()].key;
  base.sink = input.nodes[u2_ck.value()].key;
  base.kind = ArcKind::kNet;
  base.owner_name = "n_div";
  ArcRecord arc;
  arc.id = ArcId{0};
  arc.key = {base, 0};
  arc.source = u1_q;
  arc.sink = u2_ck;
  arc.kind = ArcKind::kNet;
  arc.is_disabled = true;
  arc.transfer.delay = 0.0;
  arc.transfer.delay_min = 0.0;
  arc.transfer.delay_max = 0.0;
  arc.output_load = outputLoad(0.1);
  input.arcs = {arc};
  input.arc_ids_by_key = {{arc.key, ArcId{0}}};
  input.out_row_offsets = {0};
  for (std::size_t node = 0; node < input.nodes.size(); ++node)
    input.out_row_offsets.push_back(input.out_row_offsets.back() + (node == u1_q.value()));
  input.out_arc_ids = {ArcId{0}};
  input.in_row_offsets = {0};
  for (std::size_t node = 0; node < input.nodes.size(); ++node)
    input.in_row_offsets.push_back(input.in_row_offsets.back() + (node == u2_ck.value()));
  input.in_arc_ids = {ArcId{0}};
  const auto full_name_less = [&](NodeId lhs, NodeId rhs) { return input.nodes[lhs.value()].key.full_name < input.nodes[rhs.value()].key.full_name; };
  input.ordered_root_ids = {u1_ck, u1_q, u2_ck, u2_q};
  std::ranges::sort(input.ordered_root_ids, full_name_less);
  input.ordered_start_ids = input.ordered_root_ids;
  input.ordered_end_ids = {u2_q};

  ChildTimingArcModel selected_child;
  selected_child.signature.library_name = "slow.lib";
  CellTransferModel selected_transfer;
  selected_transfer.is_clock_arc = true;
  selected_transfer.children = {selected_child};
  ReferencedLibraryCellModel cell;
  cell.cell_name = "DFF_X1";
  cell.present = true;
  cell.library_name = "slow.lib";
  cell.is_sequential = true;
  cell.clock_pin_name = "CK";
  cell.data_pin_name = "D";
  cell.output_pin_name = "Q";
  cell.ports = {LibraryPortModel{.name = "CK", .direction = PinDirection::kInput, .is_clock = true, .default_capacitance = 0.1},
                LibraryPortModel{.name = "D", .direction = PinDirection::kInput, .default_capacitance = 0.1},
                LibraryPortModel{.name = "Q", .direction = PinDirection::kOutput, .is_output = true, .default_capacitance = 0.1}};
  cell.cell_arcs = {LibraryCellArcModel{"CK", "Q", selected_transfer}};
  cell.timing_checks = {LibraryTimingCheckArcModel{"CK", "D", TimingCheckKind::kSetup, 0.0, Transition::kRise, {}}};
  input.referenced_library_cells = {cell};
  input.timing_check_endpoints = {{"u1", 0, TimingCheckKind::kSetup, "CK", "D", "u1:CK", "u1:D", u1_ck, std::nullopt},
                                  {"u2", 0, TimingCheckKind::kSetup, "CK", "D", "u2:CK", "u2:D", u2_ck, std::nullopt}};
  input.clocks = {ClockConstraintModel{"clk", "clk", {RawNodeReference{"u1:CK", u1_ck}}, 10.0, {0.0, 5.0}, false}};
  input.clock_owner_by_node.assign(input.nodes.size(), std::nullopt);
  input.clock_owner_by_node[u1_ck.value()] = "clk";

  const auto make_register_seed = [](NodeId seed_node, NodeId path_start_node, NodeId clock_node) {
    StartSeedModel seed;
    seed.seed_node = seed_node;
    seed.path_start_node = path_start_node;
    seed.availability = StartSeedAvailability::kUnsupportedSequential;
    seed.path_source = PathSource::kRegister;
    seed.clock_tag = "clk";
    seed.crpr_node = clock_node;
    seed.crpr_transition = Transition::kRise;
    return seed;
  };
  for (NodeId start : input.ordered_start_ids) {
    if (start == u1_ck || start == u1_q) {
      input.start_seeds.push_back(make_register_seed(start, start == u1_ck ? u1_q : u1_q, u1_ck));
    } else {
      StartSeedModel seed;
      seed.seed_node = start;
      seed.clock_tag = "clk";
      makeUnsupportedSequentialNull(seed);
      input.start_seeds.push_back(seed);
    }
  }
  input.clock_tags = {"clk"};
  for (const NodeId node : {u1_ck, u1_q, u2_ck, u2_q})
    input.cell_pin_capacitances.push_back({node, 0.1, {}});
  input.unsupported_reasons
      = {CsrReferenceUnsupportedReason::kSequentialInstance, CsrReferenceUnsupportedReason::kSequentialLaunch, CsrReferenceUnsupportedReason::kTimingCheck,
         CsrReferenceUnsupportedReason::kCrpr, CsrReferenceUnsupportedReason::kClockGatingOrDivider};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput conditionalInput()
{
  CsrReferenceInput input = cellInput();
  input.arcs[0].key.base.library_arcs[0].sdf_condition = "A";
  input.arcs[0].transfer.children[0].signature.sdf_condition = "A";
  input.referenced_library_cells[0].cell_arcs[0].transfer = input.arcs[0].transfer;
  input.arc_ids_by_key[0].key = input.arcs[0].key;
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kConditionalTimingArc};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput caseInput()
{
  CsrReferenceInput input = referenceInput();
  input.case_analysis_constraints = {{"in", NodeId{0}, true}};
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kCaseAnalysis};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput propagatedInput()
{
  CsrReferenceInput input = clockInput();
  input.clocks[0].propagated = true;
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kPropagatedClock, CsrReferenceUnsupportedReason::kClockTreeFanout};
  refreshIdentity(input);
  return input;
}

void verifyExactReason(const CsrReferenceInput& input, CsrReferenceUnsupportedReason reason, const std::string&, const std::string& name)
{
  requireValid(validateReferenceInput(input), name + " baseline");
  CsrReferenceInput missing = input;
  const auto found = std::ranges::find(missing.unsupported_reasons, reason);
  require(found != missing.unsupported_reasons.end(), name + " fixture does not contain its reason");
  missing.unsupported_reasons.erase(found);
  requireViolation(validateReferenceInput(missing), "unsupported_reasons", "exactly equal", name + " missing reason");

  CsrReferenceInput spurious = input;
  CsrReferenceUnsupportedReason extra = CsrReferenceUnsupportedReason::kCaseAnalysis;
  if (std::ranges::find(spurious.unsupported_reasons, extra) != spurious.unsupported_reasons.end())
    extra = CsrReferenceUnsupportedReason::kParasiticNet;
  spurious.unsupported_reasons.push_back(extra);
  std::ranges::sort(spurious.unsupported_reasons, {}, [](auto value) { return static_cast<std::uint8_t>(value); });
  requireInvalid(validateReferenceInput(spurious), name + " spurious reason");
}

void testAllDetectors()
{
  verifyExactReason(parasiticInput(), CsrReferenceUnsupportedReason::kParasiticNet, "parasitics.nets", "parasitic");
  verifyExactReason(sequentialInput(), CsrReferenceUnsupportedReason::kSequentialInstance, "instances.is_sequential", "sequential instance");
  const CsrReferenceInput register_input = registerInput();
  verifyExactReason(register_input, CsrReferenceUnsupportedReason::kSequentialLaunch, "start_seeds.path_source", "sequential launch");
  verifyExactReason(register_input, CsrReferenceUnsupportedReason::kCrpr, "start_seeds.crpr", "CRPR");
  verifyExactReason(timingCheckInput(), CsrReferenceUnsupportedReason::kTimingCheck, "timing_check_endpoints", "timing check");
  verifyExactReason(gatingInput(), CsrReferenceUnsupportedReason::kClockGatingOrDivider, "instances.is_clock_gating", "clock gating");
  verifyExactReason(dividerInput(), CsrReferenceUnsupportedReason::kClockGatingOrDivider, "instances.is_clock_gating", "clock divider");
  verifyExactReason(conditionalInput(), CsrReferenceUnsupportedReason::kConditionalTimingArc, "arcs.key.sdf_condition", "conditional timing arc");
  verifyExactReason(caseInput(), CsrReferenceUnsupportedReason::kCaseAnalysis, "case_analysis_constraints", "case analysis");
  verifyExactReason(clockInput(), CsrReferenceUnsupportedReason::kClockTreeFanout, "arcs.eligibility.clock", "clock-tree fanout");
  verifyExactReason(propagatedInput(), CsrReferenceUnsupportedReason::kPropagatedClock, "clocks.propagated", "propagated clock");
}

void testMultiReasonOverlap()
{
  CsrReferenceInput input = parasiticInput();
  input.case_analysis_constraints = {{"in", NodeId{0}, true}};
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kParasiticNet, CsrReferenceUnsupportedReason::kCaseAnalysis};
  refreshIdentity(input);
  requireValid(validateReferenceInput(input), "parasitic plus case-analysis overlap");
  CsrReferenceInput missing = input;
  missing.unsupported_reasons.pop_back();
  requireViolation(validateReferenceInput(missing), "unsupported_reasons", "exactly equal", "overlap missing case-analysis reason");
}

}  // namespace

int main()
{
  testAllDetectors();
  testMultiReasonOverlap();
  return 0;
}
