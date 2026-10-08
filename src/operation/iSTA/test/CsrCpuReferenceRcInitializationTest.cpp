// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <cmath>
#include <stdexcept>

#include "CsrCpuReferenceBackendInternal.hpp"
#include "FlatCsrEdgeView.hpp"

namespace ista::csr::test {
void runCpuReferenceRcInitializationContract()
{
  const auto require = [](bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
  };
  const std::vector<CsrRcTreeNode> tree{{0, 0.0, 0.01}, {0, 2.0, 0.03}};
  const std::vector<std::size_t> terminals{0, 1};
  auto owner = std::make_shared<CsrParasiticTree>();
  owner->terminals = {NodeId{2}, NodeId{3}};
  owner->lanes.fill(std::make_shared<const CsrParasiticModel>(buildCsrParasiticModel(tree, terminals)));
  ChildTimingArcModel child;
  child.signature.sense = TimingSense::kPositiveUnate;
  child.signature.library_name = "driver";
  child.slew_derate = 0.8;
  child.slew_lower_threshold_pct = {0.1, 0.1};
  child.slew_upper_threshold_pct = {0.9, 0.9};
  const TimingTableModel delay{true, TableVariable::kOutputCapacitance, TableVariable::kNone,
                               {{0.0, 1.0}}, {0.03, 2.03}};
  const TimingTableModel slew{true, TableVariable::kOutputCapacitance, TableVariable::kNone,
                              {{0.0, 1.0}}, {0.02, 3.02}};
  child.delay_tables = {delay, delay};
  child.slew_tables = {slew, slew};
  CsrReferenceInput input;
  input.nodes.resize(4);
  for (std::uint64_t slot = 0; slot < 2; ++slot) {
    ArcRecord arc;
    arc.id = ArcId{slot}; arc.source = NodeId{slot}; arc.sink = NodeId{2};
    arc.kind = ArcKind::kCell; arc.native_cell_arc_ordinal = 1 - slot;
    arc.parasitic = {owner, 0};
    // Deliberately reverse physical slots and Liberty order. Even disabled
    // graph arcs participate in native initialization until tombstoned.
    arc.is_disabled = slot == 0;
    arc.is_loop_disabled = slot == 0;
    child.output_threshold_pct = slot == 0 ? std::array<double, 2>{0.55, 0.55}
                                           : std::array<double, 2>{0.35, 0.35};
    arc.transfer.setChildren({child});
    input.arcs.push_back(std::move(arc));
  }
  FlatCsrEdgeView edges(input);
  CsrCpuReferenceWorkState work;
  require(initializeCsrCpuReferenceArcs(edges, work) == CsrCpuReferenceStageStatus::kSuccess,
          "RC initialization failed");
  const CsrCpuRcChildKey first_key{NodeId{2}, false, 0, 0, AnalysisMode::kMax, Transition::kRise};
  const auto& first = work.rc.timing.at({first_key, 0.0, false});
  const double driver_slew = first.timing.arnoldi.driver_slew;
  const auto* selected = work.rc.arnoldi_drivers.at(
      {NodeId{2}, AnalysisMode::kMax, Transition::kRise, driver_slew, false});
  require(selected->child->output_threshold_pct[0] == 0.55,
          "physical ArcId order replaced native initialization order");

  ReferencedLibraryCellModel load;
  load.present = true; load.library_name = "other";
  load.slew_derate = 0.8; load.slew_lower_threshold_pct = {0.1, 0.1};
  load.slew_upper_threshold_pct = {0.9, 0.9}; load.input_threshold_pct = {0.45, 0.45};
  const auto& model = owner->lanes[0]->arnoldi;
  double base_delay = 0.0, base_slew = 0.0, actual_delay = 0.0, actual_slew = 0.0;
  require(evaluateCsrArnoldiLoad(first.timing.arnoldi, model.sqrt_total_capacitance,
              model.projection[1].data(), csrArnoldiThresholds(0.1, 0.9, 0.8),
              base_delay, base_slew) == CsrRcEvaluationStatus::kSuccess, "test terminal unavailable");
  const double expected_delay = base_delay + base_slew * ((0.45 - 0.55) / 0.8);
  for (int repeat = 0; repeat < 2; ++repeat) {
    require(evaluateCsrCpuRcLoad({owner, 1}, AnalysisMode::kMax, Transition::kRise, driver_slew,
                &load, work.rc, actual_delay, actual_slew) == CsrRcEvaluationStatus::kSuccess,
            "selected driver terminal failed");
    require(std::abs(actual_delay - expected_delay) < 1e-9 && std::abs(actual_slew - base_slew) < 1e-9,
            "same-slew collision used the wrong native child threshold");
    double ignored = 0.0;
    require(evaluateCsrCpuCellTiming({owner, 0}, first_key, input.arcs[1].transfer.children()[0],
                0.0, -1.0, false, work.rc, ignored) == CsrRcEvaluationStatus::kSuccess,
            "existing first child did not hit");
  }
}
}  // namespace ista::csr::test
