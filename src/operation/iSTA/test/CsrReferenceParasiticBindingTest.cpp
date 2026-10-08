// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "Database.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"
#include "Net.hpp"
#include "ParasiticNet.hpp"
#include "ParasiticNode.hpp"
#include "ParasiticResistor.hpp"
#include "Pin.hpp"
#include "TimingCell.hpp"
#include "TimingCellPort.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingLibrary.hpp"
#include "TimingPortConstraint.hpp"

using namespace ista::csr;
using namespace ista::csr::test;
namespace {
void require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}
CsrReferenceInput exportInput(CsrReferenceInputNativeFixture& fixture,
                             std::uint64_t token, const StaAnalysisContext& context)
{
  auto result = exportCsrReferenceInputCandidate(fixture.database(), context, token);
  require(std::holds_alternative<CsrReferenceInput>(result), "RC fixture export failed");
  auto input = std::get<CsrReferenceInput>(std::move(result));
  require(validateReferenceInputForSession(input, token).empty(), "RC fixture is not strict-valid");
  return input;
}
}  // namespace

void testSharedParasiticBindings()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  auto& timing_library = fixture.database().get_timing_library();
  timing_library.set_slew_derate_from_library(0.5);
  timing_library.set_slew_lower_threshold_pct_rise(0.125);
  timing_library.set_slew_lower_threshold_pct_fall(0.25);
  timing_library.set_slew_upper_threshold_pct_rise(0.75);
  timing_library.set_slew_upper_threshold_pct_fall(0.875);
  auto& port = fixture.database().get_timing_library().get_cell_map()
      .at("DFF_X1").get_port_map().at("Q");
  port.set_capacitance(0.003);
  port.get_trans_capacitance_map()[ista::AnalysisType::kMin][ista::TransType::kFall] = 0.007;
  auto& library = fixture.database().get_parasitic_library();
  library.set_capacitive_unit("PF");
  library.set_resistance_unit("OHM");
  ista::ParasiticNet net;
  net.set_net_name("out_net");
  for (const auto& name : {"ff1:Q", "out"}) {
    ista::ParasiticNode node;
    node.set_node_name(name);
    node.set_capacitance(0.01);
    net.get_node_map().emplace(name, node);
  }
  ista::ParasiticResistor resistor;
  resistor.set_source_node("ff1:Q");
  resistor.set_sink_node("out");
  resistor.set_resistance(100.0);
  net.get_resistor_list().push_back(resistor);
  library.get_net_map().emplace("out_net", std::move(net));
  auto context = referenceInputTestContext();
  context.rc_corner = "rc";
  auto input = exportInput(fixture, 38, context);
  const InputPortRcThresholds expected_thresholds{0.5, {0.125, 0.25}, {0.75, 0.875}};
  require(input.parasitics.input_port_thresholds == expected_thresholds,
          "input-port thresholds were taken from a cell instead of the global library");
  FlatTimingPayloadView thresholds_view(input);
  require(thresholds_view.inputPortRcThresholds() == expected_thresholds,
          "backend view lost global input-port thresholds");
  auto invalid = input;
  invalid.parasitics.input_port_thresholds.lower[1] = std::numeric_limits<double>::quiet_NaN();
  require(!validateReferenceInputIntrinsic(invalid).empty(), "nonfinite global threshold accepted");
  std::shared_ptr<const CsrParasiticTree> owner;
  for (const auto& arc : input.arcs) {
    if (input.nodes[arc.sink.value()].net_name != "out_net") {
      require(!arc.parasitic.tree, "RC owner leaked onto a no-RC net");
      continue;
    }
    require(arc.parasitic.tree && !arc.output_load, "RC arc lost its prepared owner");
    if (owner) require(owner == arc.parasitic.tree, "one net reduced more than once");
    else owner = arc.parasitic.tree;
    require(owner->terminals[arc.parasitic.terminal] == arc.sink, "RC terminal is misbound");
  }
  require(owner && owner->lanes[0] == owner->lanes[1]
          && owner->lanes[0] == owner->lanes[2] && owner->lanes[0] != owner->lanes[3],
          "RC load lane sharing ignored actual driver capacitance");
  const double expected = 0.023 + fixture.database().get_timing_constraint()
      .get_port_constraint_map().at("out").get_load();
  require(std::abs(owner->lanes[0]->arnoldi.total_capacitance - expected) < 1e-12,
          "RC wire, driver or port capacitance counted incorrectly");
  require(std::abs(owner->lanes[3]->arnoldi.total_capacitance - expected - 0.004) < 1e-12,
          "RC driver transition capacitance was omitted");
  FlatCsrEdgeView view(input);
  for (const auto& arc : input.arcs)
    require(view.arc(arc.id).value().parasitic.tree == arc.parasitic.tree,
            "flat view copied or lost the RC owner");
  auto second = exportInput(fixture, 39, context);
  const auto& second_binding = std::ranges::find_if(second.arcs,
      [](const ArcRecord& arc) { return bool(arc.parasitic.tree); })->parasitic;
  const auto& first_binding = std::ranges::find_if(input.arcs,
      [](const ArcRecord& arc) { return bool(arc.parasitic.tree); })->parasitic;
  require(first_binding != second_binding, "independent RC owners passed an expected-owner check");
  require(first_binding.tree->net_name == second_binding.tree->net_name
          && first_binding.tree->terminals == second_binding.tree->terminals,
          "independent RC preparation changed terminal identity");
  for (std::size_t lane = 0; lane < first_binding.tree->lanes.size(); ++lane)
    require(*first_binding.tree->lanes[lane] == *second_binding.tree->lanes[lane],
            "independent RC preparation changed the reduced model");
  // Derived owners are not fingerprint payload: the public validator must
  // still reject changed models, missing owners, and wrong terminal slots.
  const auto rc_arc = static_cast<std::size_t>(std::ranges::find_if(input.arcs,
      [](const ArcRecord& arc) { return bool(arc.parasitic.tree); }) - input.arcs.begin());
  auto changed = input;
  changed.arcs[rc_arc].parasitic = {};
  require(!validateReferenceInput(changed).empty(), "missing RC binding accepted");
  changed = input;
  changed.arcs[rc_arc].parasitic.terminal = owner->terminals.size();
  require(!validateReferenceInput(changed).empty(), "out-of-range RC terminal accepted");
  changed = input;
  auto corrupt = std::make_shared<CsrParasiticTree>(*owner);
  auto corrupt_lane = std::make_shared<CsrParasiticModel>(*corrupt->lanes[0]);
  corrupt_lane->root_load += 1.0;
  corrupt->lanes[0] = corrupt_lane;
  changed.arcs[rc_arc].parasitic.tree = corrupt;
  require(!validateReferenceInput(changed).empty(), "RC model not bound to raw input");
  corrupt->lanes[0].reset();
  require(!validateReferenceInput(changed).empty(), "null RC model lane accepted");
  changed = input;
  const auto no_rc = std::ranges::find_if(changed.arcs,
      [](const ArcRecord& arc) { return !arc.parasitic.tree; });
  require(no_rc != changed.arcs.end(), "fixture has no no-RC arc");
  no_rc->parasitic = first_binding;
  require(!validateReferenceInput(changed).empty(), "RC binding on a no-RC net accepted");
  auto physical_context = context;
  physical_context.rc_corner = "pre_route";
  const auto physical = exportInput(fixture, 42, physical_context);
  require(std::ranges::find(physical.unsupported_reasons, CsrReferenceUnsupportedReason::kParasiticNet)
              == physical.unsupported_reasons.end(), "prepared pre-route tree remains unsupported");
  require(std::ranges::find(input.unsupported_reasons, CsrReferenceUnsupportedReason::kParasiticNet)
              != input.unsupported_reasons.end(), "general SPEF scope was silently expanded");
  auto& raw_edges = library.get_net_map().at("out_net").get_resistor_list();
  const auto saved_edges = raw_edges;
  raw_edges.clear();
  const auto disconnected = exportInput(fixture, 43, physical_context);
  require(std::ranges::find(disconnected.unsupported_reasons, CsrReferenceUnsupportedReason::kParasiticNet)
              != disconnected.unsupported_reasons.end(), "disconnected pre-route input became supported");
  require(std::ranges::none_of(disconnected.arcs, [](const ArcRecord& arc) { return bool(arc.parasitic.tree); }),
          "unsupported raw RC retained partial prepared models");
  raw_edges = saved_edges;
  // Sequential clock-pin seeds are deferred too, but only Q drivers own
  // deferred load models. Clock slew/arrival comes from the incoming net.
  ista::ParasiticNet clock_rc;
  clock_rc.set_net_name("clk_net");
  for (const auto& name : {"clk", "ff0:CK"}) {
    ista::ParasiticNode node;
    node.set_node_name(name);
    node.set_capacitance(0.01);
    clock_rc.get_node_map().emplace(name, node);
  }
  ista::ParasiticResistor clock_resistor;
  clock_resistor.set_source_node("clk");
  clock_resistor.set_sink_node("ff0:CK");
  clock_resistor.set_resistance(100.0);
  clock_rc.get_resistor_list().push_back(clock_resistor);
  library.get_net_map().emplace("clk_net", std::move(clock_rc));
  auto clock_input = exportInput(fixture, 44, physical_context);
  const auto clock_seed = std::ranges::find_if(clock_input.start_seeds, [&](const auto& seed) {
    return clock_input.nodes[seed.seed_node.value()].key.full_name == "ff0:CK";
  });
  require(clock_seed != clock_input.start_seeds.end()
              && clock_seed->availability == StartSeedAvailability::kDeferredSequential
              && !clock_seed->parasitic.tree, "clock sink seed acquired a driver RC model");
  clock_seed->parasitic = first_binding;
  require(!validateReferenceInput(clock_input).empty(), "clock sink accepted a deferred driver model");
  library.get_net_map().erase("clk_net");
  timing_library.set_slew_lower_threshold_pct_fall(1.0);
  auto degenerate = exportInput(fixture, 41, context);
  require(degenerate.parasitics.input_port_thresholds.lower[1] == 1.0,
          "finite degenerate global threshold was rejected before native fallback");
  require(degenerate.source_identity.parasitic_fingerprint != input.source_identity.parasitic_fingerprint,
          "global threshold change omitted from RC identity");
  require(degenerate.source_identity.topology_fingerprint == input.source_identity.topology_fingerprint
              && degenerate.source_identity.library_fingerprint == input.source_identity.library_fingerprint,
          "global threshold changed unrelated identities");
  input = {};
  require(owner->terminals.size() == 2 && std::isfinite(owner->lanes[0]->arnoldi.total_capacitance),
          "RC owner expired with its input");

  // A Q start without graph arcs must retain its RC through the seed alone.
  auto& database = fixture.database();
  std::erase_if(database.get_arc_list(), [](const ista::Arc& arc) {
    return arc.get_source_pin() == "ff1:Q" || arc.get_sink_pin() == "ff1:Q";
  });
  database.get_outgoing_arc_list_map().clear();
  database.get_incoming_arc_list_map().clear();
  for (std::size_t slot = 0; slot < database.get_arc_list().size(); ++slot) {
    const auto& arc = database.get_arc_list()[slot];
    database.get_outgoing_arc_list_map()[arc.get_source_pin()].push_back(slot);
    database.get_incoming_arc_list_map()[arc.get_sink_pin()].push_back(slot);
  }
  auto& bare_net = database.get_net_map().at("out_net");
  bare_net.set_load_pin_list({});
  bare_net.set_pin_name_list({"ff1:Q"});
  database.get_pin_map().at("out").set_net_name("");
  auto& bare_rc = library.get_net_map().at("out_net");
  bare_rc.get_node_map().erase("out");
  bare_rc.get_resistor_list().clear();
  auto isolated = exportInput(fixture, 40, context);
  const auto seed = std::ranges::find_if(isolated.start_seeds, [&](const StartSeedModel& value) {
    return isolated.nodes[value.seed_node.value()].key.full_name == "ff1:Q";
  });
  require(seed != isolated.start_seeds.end()
              && seed->availability == StartSeedAvailability::kDeferredSequential
              && seed->parasitic.tree && seed->parasitic.terminal == 0,
          "arc-free Q did not receive a prepared RC seed");
  const StartSeedModel retained = *seed;
  isolated = {};
  require(retained.parasitic.tree->terminals == std::vector<NodeId>{retained.seed_node}
          && std::abs(retained.parasitic.tree->lanes[0]->arnoldi.total_capacitance - 0.013) < 1e-12,
          "arc-free Q seed lost its own RC or driver capacitance");
}
