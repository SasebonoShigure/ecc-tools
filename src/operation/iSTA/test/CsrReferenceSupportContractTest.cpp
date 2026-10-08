// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "Net.hpp"
#include "ParasiticNet.hpp"
#include "Pin.hpp"
#include "TimingCell.hpp"
#include "TimingCellPort.hpp"
#include "TimingCheckArc.hpp"
#include "TimingClock.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingPoint.hpp"

namespace {

using namespace ista;
using namespace ista::csr;
using namespace ista::csr::test;

void must(bool condition, const std::string& message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

NodeKey portKey(std::string name)
{
  return NodeKey{NodeKind::kPort, std::move(name), {}, {}};
}

NodeKey pinKey(std::string full_name, std::string pin_name)
{
  return NodeKey{NodeKind::kInstancePin, std::move(full_name), "u1", std::move(pin_name)};
}

ArcOutputLoadMatrix outputLoad(double value)
{
  ArcOutputLoadMatrix load{};
  for (auto& analysis : load)
    analysis.fill(value);
  return load;
}

void refreshIdentity(CsrReferenceInput& input)
{
  input.source_identity = makeReferenceInputIdentity(input, 7, 0);
}

CsrReferenceInput referenceInput()
{
  CsrReferenceInput input;
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  NodeRecord source;
  source.id = NodeId{0};
  source.key = portKey("in");
  source.direction = ista::csr::PinDirection::kInput;
  source.net_name = "n1";
  source.is_port = true;
  source.is_startpoint = true;
  NodeRecord sink;
  sink.id = NodeId{1};
  sink.key = portKey("out");
  sink.direction = ista::csr::PinDirection::kOutput;
  sink.net_name = "n1";
  sink.is_port = true;
  sink.is_endpoint = true;
  input.nodes = {source, sink};
  input.node_ids_by_key = {{source.key, source.id}, {sink.key, sink.id}};

  ArcKeyBase base;
  base.source = source.key;
  base.sink = sink.key;
  base.kind = ArcKind::kNet;
  base.owner_name = "n1";
  ArcRecord arc;
  arc.id = ArcId{0};
  arc.key = {base, 0};
  arc.source = source.id;
  arc.sink = sink.id;
  arc.kind = ArcKind::kNet;
  arc.eligibility = {true, true, true, false};
  arc.transfer.delay = 0.0;
  arc.transfer.delay_min = 0.0;
  arc.transfer.delay_max = 0.0;
  arc.output_load = outputLoad(0.5);
  input.arcs = {arc};
  input.arc_ids_by_key = {{arc.key, arc.id}};
  input.out_row_offsets = {0, 1, 1};
  input.out_arc_ids = {arc.id};
  input.in_row_offsets = {0, 0, 1};
  input.in_arc_ids = {arc.id};
  input.ordered_root_ids = {source.id};
  input.ordered_start_ids = {source.id};
  input.ordered_end_ids = {sink.id};

  PortConstraintModel source_constraint;
  source_constraint.map_key = "in";
  source_constraint.embedded_port_name = "in";
  source_constraint.resolved_node = source.id;
  source_constraint.input_delay = {1.0, 1.0};
  source_constraint.input_transition = 0.2;
  PortConstraintModel sink_constraint;
  sink_constraint.map_key = "out";
  sink_constraint.embedded_port_name = "out";
  sink_constraint.resolved_node = sink.id;
  sink_constraint.output_load = 0.5;
  input.port_constraints = {source_constraint, sink_constraint};
  input.clock_owner_by_node = {std::nullopt, std::nullopt};

  StartSeedModel seed;
  seed.seed_node = source.id;
  seed.path_start_node = source.id;
  seed.path_source = PathSource::kInput;
  seed.clock_tag = "clk";
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      seed.arrival[analysis][transition] = 1.0;
      seed.slew[analysis][transition] = 0.2;
    }
  input.start_seeds = {seed};
  EndpointRequiredModel endpoint;
  endpoint.node = sink.id;
  endpoint.clock_name = "clk";
  input.endpoint_required = {endpoint};
  input.clock_tags = {"clk"};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput cellInput()
{
  CsrReferenceInput input;
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  NodeRecord source;
  source.id = NodeId{0};
  source.key = pinKey("u1:A", "A");
  source.direction = ista::csr::PinDirection::kInput;
  source.instance_cell = "BUF_X1";
  source.is_startpoint = true;
  NodeRecord sink;
  sink.id = NodeId{1};
  sink.key = pinKey("u1:Y", "Y");
  sink.direction = ista::csr::PinDirection::kOutput;
  sink.instance_cell = "BUF_X1";
  sink.is_endpoint = true;
  sink.is_output_pin = true;
  input.nodes = {source, sink};
  input.node_ids_by_key = {{source.key, source.id}, {sink.key, sink.id}};
  InstanceRecord instance;
  instance.instance_name = "u1";
  instance.cell_name = "BUF_X1";
  instance.ordered_pins = {source.id, sink.id};
  input.instances = {instance};

  LibraryArcSignature signature;
  signature.library_name = "slow.lib";
  ChildTimingArcModel child;
  child.signature = signature;
  ArcKeyBase base;
  base.source = source.key;
  base.sink = sink.key;
  base.kind = ArcKind::kCell;
  base.owner_name = "u1";
  base.library_source_port = "A";
  base.library_sink_port = "Y";
  base.library_arcs = {signature};
  ArcRecord arc;
  arc.id = ArcId{0};
  arc.key = {base, 0};
  arc.source = source.id;
  arc.sink = sink.id;
  arc.kind = ArcKind::kCell;
  arc.eligibility = {true, true, true, false};
  arc.transfer.setChildren({child});
  arc.native_cell_arc_ordinal = 0;
  arc.output_load = outputLoad(0.0);
  input.arcs = {arc};
  input.arc_ids_by_key = {{arc.key, arc.id}};
  input.out_row_offsets = {0, 1, 1};
  input.out_arc_ids = {arc.id};
  input.in_row_offsets = {0, 0, 1};
  input.in_arc_ids = {arc.id};
  input.ordered_root_ids = {source.id};
  input.ordered_start_ids = {source.id};
  input.ordered_end_ids = {sink.id};
  input.clock_tags = {"clk"};
  input.clock_owner_by_node = {std::nullopt, std::nullopt};

  StartSeedModel seed;
  seed.seed_node = source.id;
  seed.path_start_node = source.id;
  seed.clock_tag = "clk";
  for (auto& analysis : seed.slew)
    for (auto& transition : analysis)
      transition = 0.0;
  input.start_seeds = {seed};
  CellPinCapacitanceModel source_cap;
  source_cap.node = source.id;
  source_cap.default_capacitance = 0.1;
  CellPinCapacitanceModel sink_cap = source_cap;
  sink_cap.node = sink.id;
  input.cell_pin_capacitances = {source_cap, sink_cap};
  ReferencedLibraryCellModel cell;
  cell.cell_name = "BUF_X1";
  cell.present = true;
  cell.library_name = "slow.lib";
  cell.output_pin_name = "Y";
  LibraryPortModel source_port;
  source_port.name = "A";
  source_port.direction = ista::csr::PinDirection::kInput;
  source_port.default_capacitance = 0.1;
  LibraryPortModel sink_port;
  sink_port.name = "Y";
  sink_port.direction = ista::csr::PinDirection::kOutput;
  sink_port.is_output = true;
  sink_port.default_capacitance = 0.1;
  cell.ports = {source_port, sink_port};
  LibraryCellArcModel cell_arc;
  cell_arc.source_port = "A";
  cell_arc.sink_port = "Y";
  cell_arc.transfer = arc.transfer;
  cell.cell_arcs = {cell_arc};
  input.referenced_library_cells = {std::move(cell)};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput clockInput()
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

bool hasReason(const CsrReferenceInput& input, CsrReferenceUnsupportedReason reason)
{
  return std::ranges::find(input.unsupported_reasons, reason) != input.unsupported_reasons.end();
}


CsrReferenceInput parasiticInput()
{
  CsrReferenceInput input = referenceInput();
  input.analysis_context.rc_corner = "rc";
  input.parasitics = {"PF", "OHM", {ParasiticNetModel{"n1", "n1", 0.0, {}, {}}}};
  for (auto& arc : input.arcs)
    arc.output_load.reset();
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kParasiticNet};
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
  input.unsupported_reasons.clear();
  refreshIdentity(input);
  return input;
}

using InputBuilder = CsrReferenceInput (*)();

void verifyReason(CsrReferenceUnsupportedReason reason, InputBuilder baseline_builder, InputBuilder trigger_builder, const char* name)
{
  CsrReferenceInput baseline = baseline_builder();
  must(validateReferenceInput(baseline).empty(), std::string{name} + " baseline is not strict-valid");
  must(!hasReason(baseline, reason), std::string{name} + " baseline already contains target reason");

  CsrReferenceInput trigger = trigger_builder();
  must(validateReferenceInput(trigger).empty(), std::string{name} + " trigger is not strict-valid");
  must(hasReason(trigger, reason), std::string{name} + " trigger omitted target reason");

  CsrReferenceInput removed = trigger;
  removed.unsupported_reasons.erase(std::ranges::find(removed.unsupported_reasons, reason));
  refreshIdentity(removed);
  must(!validateReferenceInput(removed).empty(), std::string{name} + " accepted target-reason removal");

  CsrReferenceInput spurious = baseline;
  spurious.unsupported_reasons.push_back(reason);
  std::ranges::sort(spurious.unsupported_reasons, {}, [](auto value) { return static_cast<std::uint8_t>(value); });
  refreshIdentity(spurious);
  must(!validateReferenceInput(spurious).empty(), std::string{name} + " accepted spurious target reason");
}

enum class SourceMode
{
  kSequential,
  kGating,
  kMacro,
  kNonSequential,
  kMissing,
  kPort
};

Pin nativePin(const std::string& full_name, const std::string& local_name, const std::string& instance, ista::PinDirection direction, bool port,
              bool connected = true)
{
  Pin pin;
  pin.set_full_name(full_name);
  pin.set_pin_name(local_name);
  pin.set_instance_name(instance);
  pin.set_net_name(connected ? "n" : "");
  pin.set_direction(direction);
  pin.set_is_port(port);
  return pin;
}

TimingCellPort nativePort(const std::string& name, bool input, bool output, bool clock)
{
  TimingCellPort port;
  port.set_port_name(name);
  port.set_is_input(input);
  port.set_is_output(output);
  port.set_is_clock(clock);
  port.set_capacitance(0.1);
  return port;
}

TimingCheckArc libraryCheck()
{
  TimingCheckArc check;
  check.set_clock_port("CK");
  check.set_data_port("D");
  check.set_check_type(TimingCheckType::kSetup);
  check.set_clock_trans_type(TransType::kRise);
  return check;
}

CsrReferenceInputNativeFixture makeDividerFixture(SourceMode mode, bool disabled, bool loop_disabled, bool source_reachable, bool sink_is_port = false)
{
  CsrReferenceInputNativeFixture fixture;
  Database& database = fixture.database();
  const bool source_is_port = mode == SourceMode::kPort;
  const std::string source_name = source_is_port ? "src" : "src:X";
  const std::string sink_name = sink_is_port ? "sink" : "sink:CK";
  database.get_pin_map().emplace(
      source_name, nativePin(source_name, source_is_port ? "src" : "X", source_is_port ? "" : "src", ista::PinDirection::kOutput, source_is_port));
  database.get_pin_map().emplace(sink_name, nativePin(sink_name, sink_is_port ? "sink" : "CK", sink_is_port ? "" : "sink",
                                                      sink_is_port ? ista::PinDirection::kOutput : ista::PinDirection::kInput, sink_is_port));
  database.get_timing_point_map().emplace(source_name, TimingPoint{});
  database.get_timing_point_map().emplace(sink_name, TimingPoint{});
  if (!source_is_port) {
    database.get_pin_map().emplace("src:Q", nativePin("src:Q", "Q", "src", ista::PinDirection::kOutput, false, false));
    database.get_timing_point_map().emplace("src:Q", TimingPoint{});
  }
  if (!sink_is_port) {
    database.get_pin_map().emplace("sink:D", nativePin("sink:D", "D", "sink", ista::PinDirection::kInput, false, false));
    database.get_timing_point_map().emplace("sink:D", TimingPoint{});
  }

  if (!source_is_port) {
    Instance source;
    source.set_instance_name("src");
    source.set_cell_name(mode == SourceMode::kMissing ? "MISSING" : "SRC");
    source.set_pin_name_list({"src:Q", source_name});
    source.set_output_pin_name(mode == SourceMode::kMissing ? "" : "src:Q");
    source.set_is_sequential(mode != SourceMode::kNonSequential && mode != SourceMode::kMissing);
    source.set_is_clock_gating(mode == SourceMode::kGating);
    database.get_instance_map().emplace("src", std::move(source));
  }

  if (!sink_is_port) {
    TimingCheckArc instance_check = libraryCheck();
    instance_check.set_clock_port("sink:CK");
    instance_check.set_data_port("sink:D");
    Instance sink;
    sink.set_instance_name("sink");
    sink.set_cell_name("SINK");
    sink.set_pin_name_list({sink_name, "sink:D"});
    sink.set_clock_pin_name(sink_name);
    sink.set_data_pin_name("sink:D");
    sink.set_is_sequential(true);
    sink.get_check_arc_list().push_back(std::move(instance_check));
    database.get_instance_map().emplace("sink", std::move(sink));
  }

  if (!source_is_port && mode != SourceMode::kMissing) {
    TimingCell source;
    source.set_cell_name("SRC");
    source.set_library_name("slow.lib");
    source.set_is_sequential(mode != SourceMode::kNonSequential);
    source.set_is_clock_gating(mode == SourceMode::kGating);
    source.set_is_macro(mode == SourceMode::kMacro);
    source.get_port_map().emplace("Q", nativePort("Q", false, true, false));
    source.get_port_map().emplace("X", nativePort("X", false, true, false));
    database.get_timing_library().get_cell_map().emplace("SRC", std::move(source));
  }

  if (!sink_is_port) {
    TimingCell sink;
    sink.set_cell_name("SINK");
    sink.set_library_name("slow.lib");
    sink.set_is_sequential(true);
    sink.get_port_map().emplace("CK", nativePort("CK", true, false, true));
    sink.get_port_map().emplace("D", nativePort("D", true, false, false));
    sink.get_check_arc_list().push_back(libraryCheck());
    database.get_timing_library().get_cell_map().emplace("SINK", std::move(sink));
  }

  Net net;
  net.set_net_name("n");
  net.set_pin_name_list({source_name, sink_name});
  net.set_driver_pin_list({source_name});
  net.set_driver_pin(source_name);
  net.set_load_pin_list({sink_name});
  database.get_net_map().emplace("n", std::move(net));
  Arc arc;
  arc.set_arc_name("n:" + source_name + "->" + sink_name);
  arc.set_source_pin(source_name);
  arc.set_sink_pin(sink_name);
  arc.set_owner_name("n");
  arc.set_type(ArcType::kNet);
  arc.set_is_disable_arc(disabled);
  arc.set_is_loop_disable(loop_disabled);
  database.get_arc_list().push_back(std::move(arc));
  database.get_outgoing_arc_list_map()[source_name] = {0};
  database.get_incoming_arc_list_map()[sink_name] = {0};

  if (source_reachable) {
    TimingClock clock;
    clock.set_clock_name("embedded");
    clock.set_source_list({source_name});
    clock.set_period(2.0);
    clock.set_fall_edge(1.0);
    database.get_timing_constraint().get_clock_map().emplace("clock_key", std::move(clock));
  }
  return fixture;
}

CsrReferenceInput exportFixture(CsrReferenceInputNativeFixture& fixture, std::uint64_t token, const char* name)
{
  auto result = exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), token);
  if (const auto* failure = std::get_if<CsrReferenceInputExportFailure>(&result)) {
    std::cerr << name << " export failed with code " << static_cast<int>(failure->code) << '\n';
    std::exit(1);
  }
  CsrReferenceInput input = std::get<CsrReferenceInput>(std::move(result));
  must(validateReferenceInputForSession(input, token).empty(), std::string{name} + " export is not strict-valid");
  return input;
}

void testExactReasonMatrix()
{
  verifyReason(CsrReferenceUnsupportedReason::kParasiticNet, referenceInput, parasiticInput, "parasitic");
  const auto propagated = propagatedInput();
  must(propagated.unsupported_reasons.empty() && validateReferenceInput(propagated).empty(),
       "ordinary propagated clock must be supported");
  verifyReason(CsrReferenceUnsupportedReason::kClockGatingOrDivider, cellInput, gatingInput, "clock gating");
  verifyReason(CsrReferenceUnsupportedReason::kCaseAnalysis, referenceInput, caseInput, "case analysis");
}

void testSupportFirstAndOverlap()
{
  const CsrReferenceInput unsupported = parasiticInput();
  const auto errors = validateTimingResult(unsupported, CsrTimingResult{});
  must(errors.size() == 1 && errors[0].field == "input.unsupported_reasons", "support-first gate accessed malformed result state");

  CsrReferenceInput overlap = clockInput();
  overlap.clocks.push_back(ClockConstraintModel{"z_clock", "z_embedded", {RawNodeReference{"in", NodeId{0}}}, 3.0, {0.0, 1.0}, false});
  overlap.clock_tags = {"clk", "z_clock", "z_embedded"};
  refreshIdentity(overlap);
  const auto overlap_errors = validateReferenceInputIntrinsic(overlap);
  must(std::ranges::any_of(overlap_errors,
                           [](const CsrContractViolation& error) {
                             return error.field == "clock_owner_by_node" && error.message.find("different clock map keys") != std::string::npos;
                           }),
       "artificial different-map-key overlap was not rejected by the pure validator");
}

void testDividerAndGatingOrthogonality()
{
  auto divider_fixture = makeDividerFixture(SourceMode::kSequential, true, false, false);
  const CsrReferenceInput divider = exportFixture(divider_fixture, 101, "unreachable nonselected divider");
  must(hasReason(divider, CsrReferenceUnsupportedReason::kClockGatingOrDivider), "true divider reason missing");
  const auto divider_raw = projectCsrReferenceInputRaw(divider_fixture.database());
  const auto source = std::ranges::find(divider_raw.instances, "src", &RawInstance::map_key);
  must(source != divider_raw.instances.end() && source->output_pin_name == "src:Q" && !source->selected_clock_arc,
       "divider source was accidentally constrained to selected output/C2Q");

  auto reachable_fixture = makeDividerFixture(SourceMode::kSequential, true, false, true);
  const CsrReferenceInput reachable = exportFixture(reachable_fixture, 102, "reachable divider source");
  const auto source_node = std::ranges::find(reachable.nodes, "src:X", [](const NodeRecord& node) { return node.key.full_name; });
  must(source_node != reachable.nodes.end() && reachable.clock_owner_by_node[source_node->id.value()] == "clock_key",
       "reachable divider source lost its replay owner");
  must(hasReason(reachable, CsrReferenceUnsupportedReason::kClockGatingOrDivider), "reachable divider reason missing");

  for (const auto& [mode, disabled, expect_reason, name] :
       std::array{std::tuple{SourceMode::kGating, false, true, "gating"}, std::tuple{SourceMode::kMacro, false, false, "macro"},
                  std::tuple{SourceMode::kNonSequential, false, false, "nonsequential"}, std::tuple{SourceMode::kMissing, false, false, "missing source cell"},
                  std::tuple{SourceMode::kPort, false, false, "port source"}}) {
    auto fixture = makeDividerFixture(mode, disabled, false, false);
    const CsrReferenceInput input = exportFixture(fixture, 103, name);
    must(hasReason(input, CsrReferenceUnsupportedReason::kClockGatingOrDivider) == expect_reason,
         std::string{name} + " changed exact gating/divider predicate");
  }

  auto sink_port_fixture = makeDividerFixture(SourceMode::kSequential, false, false, false, true);
  const CsrReferenceInput sink_port = exportFixture(sink_port_fixture, 104, "port sink");
  must(!hasReason(sink_port, CsrReferenceUnsupportedReason::kClockGatingOrDivider), "port sink fabricated divider support");

  auto wrong_true = makeDividerFixture(SourceMode::kSequential, false, false, false);
  must(std::holds_alternative<CsrReferenceInputExportFailure>(exportCsrReferenceInputCandidate(wrong_true.database(), referenceInputTestContext(), 105)),
       "true divider with wrong disabled=false was accepted");
  auto wrong_false = makeDividerFixture(SourceMode::kMacro, true, false, false);
  must(std::holds_alternative<CsrReferenceInputExportFailure>(exportCsrReferenceInputCandidate(wrong_false.database(), referenceInputTestContext(), 106)),
       "non-divider with wrong disabled=true was accepted");

  auto loop_divider_fixture = makeDividerFixture(SourceMode::kSequential, true, true, true);
  const CsrReferenceInput loop_divider = exportFixture(loop_divider_fixture, 107, "loop-disabled divider");
  must(loop_divider.arcs[0].is_disabled && loop_divider.arcs[0].is_loop_disabled && !loop_divider.arcs[0].eligibility.clock,
       "loop-disabled divider flags or replay were conflated");
  auto loop_plain_fixture = makeDividerFixture(SourceMode::kMacro, false, true, true);
  const CsrReferenceInput loop_plain = exportFixture(loop_plain_fixture, 108, "loop-disabled non-divider");
  must(!hasReason(loop_plain, CsrReferenceUnsupportedReason::kClockGatingOrDivider) && !loop_plain.arcs[0].is_disabled && loop_plain.arcs[0].is_loop_disabled,
       "loop-disabled non-divider was treated as disabled divider");

  auto fanout_fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kDiamond);
  const CsrReferenceInput fanout = exportFixture(fanout_fixture, 109, "ordinary clock fanout");
  must(fanout.unsupported_reasons.empty(),
       "ordinary replayed clock fanout fabricated gating/divider support");
}

void testRemainingReasonOverlap()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  ParasiticNet net;
  net.set_net_name("p");
  fixture.database().get_parasitic_library().set_capacitive_unit("PF");
  fixture.database().get_parasitic_library().set_resistance_unit("OHM");
  fixture.database().get_parasitic_library().get_net_map().emplace("p", std::move(net));
  StaAnalysisContext context = referenceInputTestContext();
  context.rc_corner = "rc";
  auto result = exportCsrReferenceInputCandidate(fixture.database(), context, 110);
  must(std::holds_alternative<CsrReferenceInput>(result), "remaining-reason overlap export failed");
  const CsrReferenceInput& input = std::get<CsrReferenceInput>(result);
  const std::vector<CsrReferenceUnsupportedReason> all{CsrReferenceUnsupportedReason::kParasiticNet, CsrReferenceUnsupportedReason::kClockGatingOrDivider, CsrReferenceUnsupportedReason::kCaseAnalysis};
  must(input.unsupported_reasons == all, "remaining-reason overlap is not exact stable enum order");
  must(validateReferenceInputForSession(input, 110).empty(), "remaining-reason overlap is not strict-valid");
}

}  // namespace

int main()
{
  testExactReasonMatrix();
  testSupportFirstAndOverlap();
  testDividerAndGatingOrthogonality();
  testRemainingReasonOverlap();
  return 0;
}
