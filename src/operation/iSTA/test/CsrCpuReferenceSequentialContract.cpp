#include "CsrModelTestMutation.hpp"
#include <algorithm>
#include <limits>

#include "CsrCpuReferenceBackendInternal.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr::test {
namespace {

CsrReferenceInput sequentialFixture()
{
  CsrReferenceInput input;
  input.nodes.resize(4);
  const std::array<std::string, 4> pins{"CK", "Q", "out", "RN"};
  for (std::size_t index = 0; index < input.nodes.size(); ++index) {
    auto& node = input.nodes[index];
    node.id = NodeId{index};
    node.key.pin_name = pins[index];
    node.key.full_name = index == 2 ? "out" : "ff:" + pins[index];
    node.key.instance_name = index == 2 ? "" : "ff";
    node.instance_cell = index == 2 ? "" : "DFF";
    node.is_port = index == 2;
    node.is_sequential = index != 2;
  }
  input.clock_tags = {"clk"};
  input.clock_owner_by_node.resize(4);
  input.clock_owner_by_node[0] = "clk";
  input.ordered_start_ids = {NodeId{0}, NodeId{1}, NodeId{3}};
  input.ordered_root_ids = input.ordered_start_ids;
  input.ordered_end_ids = {NodeId{2}, NodeId{3}};
  for (const auto node : input.ordered_start_ids) {
    StartSeedModel seed;
    seed.seed_node = node;
    seed.path_start_node = node;
    seed.availability = StartSeedAvailability::kDeferredSequential;
    seed.clock_tag = "clk";
    if (node.value() != 3) {
      seed.path_source = PathSource::kRegister;
      seed.path_start_node = NodeId{1};
      seed.crpr_node = NodeId{0};
      seed.crpr_transition = Transition::kFall;
    }
    input.start_seeds.push_back(seed);
  }
  ReferencedLibraryCellModel cell;
  cell.cell_name = "DFF";
  cell.present = true;
  cell.is_sequential = true;
  cell.clock_pin_name = "CK";
  cell.output_pin_name = "Q";
  cell.timing_checks.resize(1);
  LibraryCellArcModel ctq;
  ctq.transfer.is_clock_arc = true;
  ChildTimingArcModel child;
  child.signature.sense = TimingSense::kNonUnate;
  child.signature.trigger_transition = Transition::kFall;
  for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
    child.delay_tables[transition] = {true, TableVariable::kOutputCapacitance, TableVariable::kNone, {{0.0, 10.0}}, {1.0, 11.0}};
    child.slew_tables[transition] = {true, TableVariable::kInputTransition, TableVariable::kNone, {{0.0, 10.0}}, {0.5, 0.5}};
  }
  ctq.transfer.setChildren({child});
  cell.cell_arcs.push_back(ctq);
  input.referenced_library_cells.push_back(cell);
  ArcRecord net;
  net.source = NodeId{1};
  net.sink = NodeId{2};
  net.kind = ArcKind::kNet;
  net.eligibility = {true, true, true, false};
  net.output_load = ArcOutputLoadMatrix{{{{2.0, 2.0}}, {{2.0, 2.0}}}};
  input.arcs.push_back(net);
  input.out_row_offsets = {0, 0, 1, 1, 1};
  input.out_arc_ids = {ArcId{0}};
  input.in_row_offsets = {0, 0, 0, 1, 1};
  input.in_arc_ids = {ArcId{0}};
  return input;
}

CsrCpuReferenceWorkState initialize(const CsrReferenceInput& input, bool clock_present)
{
  FlatTimingPayloadView payloads(input);
  CsrCpuReferenceWorkState work;
  FlatCsrEdgeView edges(input);
  const auto levels = replayFifoLevelization(edges, payloads);
  requireCpuReference(levels.complete && !levels.view_error, "sequential fixture must levelize");
  requireCpuReference(initializeCsrCpuReferenceResult(payloads, levels, work) == CsrCpuReferenceStageStatus::kSuccess, "raw sequential initialization failed");
  if (clock_present) {
    // Deliberately distinct lanes prove the selected falling trigger is used.
    work.result.effective_clock.arrivals.values[0] = 99.0;
    work.result.effective_clock.arrivals.values[1] = 7.0;
    work.result.effective_clock.arrivals.values[2] = 99.0;
    work.result.effective_clock.arrivals.values[3] = 3.0;
    work.result.effective_clock.slews.values[0] = 99.0;
    work.result.effective_clock.slews.values[1] = 4.0;
    work.result.effective_clock.slews.values[2] = 99.0;
    work.result.effective_clock.slews.values[3] = 2.0;
  }
  requireCpuReference(initializeCsrCpuSequentialSeeds(edges, payloads, work) == CsrCpuReferenceStageStatus::kSuccess, "deferred seed calculation failed");
  return work;
}

}  // namespace

void runCpuReferenceSequentialContracts()
{
  auto input = sequentialFixture();
  auto work = initialize(input, true);
  const std::vector<double> arrivals{7.0, 10.0, -std::numeric_limits<double>::infinity(), 0.0};
  const std::vector<double> slews{4.0, 4.0, 2.0, 2.0, 0.5, 0.5, 0.5, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  requireCpuReference(work.result.nodes.arrivals.values == arrivals && work.result.data_slews.values == slews, "CK/Q/scalar-only sequential values differ");
  std::vector<std::vector<CsrCpuReferencePathState>> paths(work.path_rows.size());
  for (std::size_t node = 0; node != 2; ++node) {
    for (AnalysisMode analysis : {AnalysisMode::kMax, AnalysisMode::kMin}) {
      for (Transition transition : {Transition::kRise, Transition::kFall}) {
        CsrCpuReferencePathState path;
        path.clock_tag = ClockTagId{0};
        path.arrival = (analysis == AnalysisMode::kMax ? 7.0 : 3.0) + (node == 1 ? 3.0 : 0.0);
        path.slew = node == 1 ? 0.5 : (analysis == AnalysisMode::kMax ? 4.0 : 2.0);
        path.launch = analysis == AnalysisMode::kMax ? 7.0 : 3.0;
        path.start_node = NodeId{1};
        path.crpr_node = NodeId{0};
        path.crpr_transition = Transition::kFall;
        paths[csrPathRow(node, analysis, PathSource::kRegister, transition)] = {path};
      }
    }
  }
  requireCpuReference(work.path_rows == paths, "register launch/winner/CRPR identity differs");
  FlatCsrEdgeView edges(input);
  FlatTimingPayloadView payloads(input);
  requireCpuReference(initializeCsrCpuReferenceArcs(edges, work) == CsrCpuReferenceStageStatus::kSuccess, "sequential arc init failed");
  requireCpuReference(propagateCsrCpuReferenceForward(edges, work) == CsrCpuReferenceStageStatus::kSuccess, "register forward propagation failed");
  requireCpuReference(propagateCsrCpuReferenceRequired(edges, payloads, work) == CsrCpuReferenceStageStatus::kSuccess, "register result publication failed");
  const auto row = csrPathRow(2, AnalysisMode::kMax, PathSource::kRegister, Transition::kRise);
  const auto flat = work.result.paths.row_offsets[row];
  requireCpuReference(work.result.paths.row_offsets[row + 1] == flat + 1 && work.result.paths.arrivals.values[flat] == 10.0
                          && work.result.paths.crpr_nodes.presence[flat] == 1 && work.result.paths.crpr_nodes.values[flat] == NodeId{0}
                          && work.result.paths.crpr_transitions.values[flat] == Transition::kFall,
                      "published register path lost timing or CRPR identity");
  input.arcs.front().output_load = ArcOutputLoadMatrix{{{{4.0, 4.0}}, {{4.0, 4.0}}}};
  const auto loaded = initialize(input, true);
  requireCpuReference(loaded.result.nodes.arrivals.values == std::vector<double>{7.0, 12.0, -std::numeric_limits<double>::infinity(), 0.0},
                      "Q delay did not use changed output load");
  input.clock_owner_by_node[0].reset();
  for (auto& seed : input.start_seeds) {
    seed.path_source.reset();
    seed.path_start_node = seed.seed_node;
    seed.crpr_node.reset();
    seed.crpr_transition.reset();
  }
  const auto unclocked = initialize(input, false);
  requireCpuReference(unclocked.result.nodes.arrivals.values == std::vector<double>{0.0, 5.0, -std::numeric_limits<double>::infinity(), 0.0},
                      "unclocked Q scalar calculation must still run");
  requireCpuReference(std::ranges::all_of(unclocked.path_rows, [](const auto& row) { return row.empty(); }), "unclocked sequential start invented a path");
}

void runCpuReferenceCheckContracts()
{
  auto input = sequentialFixture();
  input.clocks = {{"clk", "clk", {}, 100.0, {0.0, 50.0}, false}, {"other", "other", {}, 1000.0, {0.0, 500.0}, false}};
  input.nodes[2].key.full_name = "ff:CK2";
  input.nodes[2].key.instance_name = "ff";
  input.nodes[2].key.pin_name = "CK2";
  input.nodes[2].instance_cell = "DFF";
  input.nodes[2].is_port = false;
  input.nodes[2].is_sequential = true;
  input.clock_owner_by_node[2] = "other";
  input.referenced_library_cells[0].timing_checks.resize(2);
  input.referenced_library_cells[0].timing_checks[0].clock_port = "CK";
  input.referenced_library_cells[0].timing_checks[0].data_port = "Q";
  input.referenced_library_cells[0].timing_checks[0].kind = TimingCheckKind::kSetup;
  input.clock_tags = {"clk", "other"};
  input.nodes[3].is_endpoint = true;
  input.ordered_end_ids = {NodeId{3}};
  auto& check = input.referenced_library_cells[0].timing_checks[1];
  check.clock_port = "CK2";
  check.data_port = "RN";
  check.kind = TimingCheckKind::kSetup;
  check.clock_transition = Transition::kFall;
  ChildTimingArcModel child;
  child.time_unit_scale = 10.0;
  child.capacitance_unit_scale = 123.0;  // Check axes are both time, never capacitance.
  child.signature.trigger_transition = Transition::kRise;
  for (auto& table : child.check_tables)
    table = {true, TableVariable::kConstrainedTransition, TableVariable::kInputTransition, {{0.0, 100.0}, {0.0, 100.0}}, {0.0, 100.0, 200.0, 300.0}};
  check.setChildren({child});
  TimingCheckEndpointModel endpoint;
  endpoint.instance_name = "ff";
  endpoint.check_kind = TimingCheckKind::kSetup;
  endpoint.resolved_data_node = NodeId{3};
  endpoint.referenced_check_ordinal = 1;
  endpoint.resolved_clock_node = NodeId{2};
  input.timing_check_endpoints = {endpoint};
  auto work = initialize(input, true);
  for (std::size_t lane = 8; lane < 12; ++lane) {
    work.result.effective_clock.arrivals.values[lane] = 999.0;
    work.result.effective_clock.slews.values[lane] = 999.0;
  }
  CsrCpuReferencePathState path;
  path.arrival = 10.0;
  path.slew = 1.25;
  path.clock_tag = ClockTagId{0};
  work.path_rows[csrPathRow(3, AnalysisMode::kMax, PathSource::kInput, Transition::kRise)] = {path};
  path.arrival = 10.0 + 0.5e-6;
  path.slew = 8.0;
  auto& input_fall = work.path_rows[csrPathRow(3, AnalysisMode::kMax, PathSource::kInput, Transition::kFall)];
  input_fall = {path};
  path.arrival = 10.0 + 1.2e-6;
  path.clock_tag = ClockTagId{1};
  input_fall.push_back(path);
  path.arrival = 100.0;
  path.slew = 5.0;
  work.path_rows[csrPathRow(3, AnalysisMode::kMax, PathSource::kRegister, Transition::kFall)] = {path};
  FlatTimingPayloadView payloads(input);
  requireCpuReference(seedCsrCpuSequentialRequired(payloads, work) == CsrCpuReferenceStageStatus::kSuccess, "setup calculation failed");
  requireCpuReference(work.result.nodes.requireds.values[3] == 98.5, "input priority/two-stage winner/time-axis conversion differs");
  work.path_rows[csrPathRow(3, AnalysisMode::kMax, PathSource::kInput, Transition::kRise)].clear();
  input_fall.clear();
  requireCpuReference(seedCsrCpuSequentialRequired(payloads, work) == CsrCpuReferenceStageStatus::kSuccess, "register setup failed");
  requireCpuReference(work.result.nodes.requireds.values[3] == 91.0, "register endpoint slew was not used");

  auto second = check;
  second.kind = TimingCheckKind::kRecovery;
  second.setChildren({});
  second.check_time = 2.0;
  ista::csr::test::mutateChildren(check, [&](auto& children) { children[0].check_tables[1] = {}; });
  input.referenced_library_cells[0].timing_checks.push_back(second);
  endpoint.referenced_check_ordinal = 2;
  endpoint.check_kind = TimingCheckKind::kRecovery;
  input.timing_check_endpoints.push_back(endpoint);
  work.result.nodes.requireds.values[3] = 1234.0;
  requireCpuReference(seedCsrCpuSequentialRequired(payloads, work) == CsrCpuReferenceStageStatus::kSuccess, "missing transition failed");
  requireCpuReference(work.result.nodes.requireds.values[3] == 1234.0, "missing transition incorrectly tried a later check");
  input.timing_check_endpoints[0].check_kind = TimingCheckKind::kHold;
  input.referenced_library_cells[0].timing_checks[1].kind = TimingCheckKind::kHold;
  requireCpuReference(seedCsrCpuSequentialRequired(payloads, work) == CsrCpuReferenceStageStatus::kSuccess, "recovery fallback failed");
  requireCpuReference(work.result.nodes.requireds.values[3] == 101.0, "max required must use recovery rather than hold");
  auto& recovery = input.referenced_library_cells[0].timing_checks[2];
  recovery.setChildren({child});
  ista::csr::test::mutateChildren(recovery, [&](auto& children) { children[0].time_unit_scale = std::numeric_limits<double>::max(); });
  requireCpuReference(seedCsrCpuSequentialRequired(payloads, work) == CsrCpuReferenceStageStatus::kNumericFailure,
                      "check time-axis overflow was not rejected");
}

}  // namespace ista::csr::test
