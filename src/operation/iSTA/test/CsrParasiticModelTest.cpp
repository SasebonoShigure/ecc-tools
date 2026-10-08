// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrParasiticModel.hpp"
#include "CsrParasiticTree.hpp"
#include "CsrRcDriverCache.hpp"
#include "CsrCpuReferenceParasitics.hpp"
#include "CsrArnoldiSpectrum.hpp"
#include "CsrArnoldiWaveform.hpp"
#include "CsrArnoldiDriver.hpp"
#include "CsrDmpLoad.hpp"
#include "CsrTableLookup.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace ista::csr;

namespace {
void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}
void near(double actual, double expected)
{
  require(std::abs(actual - expected) <= 1e-12 * (1.0 + std::abs(expected)), "RC numeric mismatch");
}
double firstMoment(const CsrArnoldiModel& model, std::size_t terminal)
{
  if (model.order < 2)
    return model.diagonal[0];
  return model.diagonal[0] + model.off_diagonal[0] * model.projection[terminal][1] / model.projection[0][0];
}

void rampBoundary()
{
  const auto thresholds = csrArnoldiThresholds(30.0, 70.0, 0.5);
  constexpr double cap = 0.013552116249999998;
  auto overflow = thresholds;
  overflow.slew_derate = 2.0;
  require(!std::isfinite(csrArnoldiDriverRamp(std::numeric_limits<double>::max(), 1.0, cap, overflow)),
          "ramp roundoff handling hid a non-finite transition");
  for (double slew : {0.22741790352985375, 0.22741790352985364}) {
    const double resistance = thresholds.slew_derate * slew / (thresholds.voltage_log * cap);
    const double minimum = resistance * cap * thresholds.min_slew_factor;
    near(csrArnoldiDriverRamp(slew, resistance, cap, thresholds), minimum);
    // A genuinely larger transition still needs the iterative ramp branch.
    require(csrArnoldiDriverRamp(slew * 1.1, resistance, cap, thresholds) > minimum,
            "finite ramp above the boundary was clamped");
  }
}

void analyticalNetworks()
{
  const std::vector<CsrRcTreeNode> tree{{0, 0.0, 0.01}, {0, 2.0, 0.03}};
  const std::vector<std::size_t> terminals{0, 1};
  const auto model = buildCsrParasiticModel(tree, terminals);
  near(model.arnoldi.total_capacitance, 0.04);
  near(model.arnoldi.diagonal[0], 0.045);
  near(firstMoment(model.arnoldi, 0), 0.0);
  near(firstMoment(model.arnoldi, 1), 0.06);
  require(model.dmp.valid, "pi model missing");
  near(model.dmp.driver_capacitance, 0.01);
  near(model.dmp.load_capacitance, 0.03);
  near(model.dmp.resistance, 2.0);
  require(model.dmp.loads[1].order == 1, "single-pole load");
  near(model.dmp.loads[1].poles[0], 1.0 / 0.06);
  require(model.fallback[1].valid, "missing native fallback");
  near(model.fallback[1].delay, 0.06);
  near(model.fallback[1].impulse, 0.06 * 0.06);
  auto owner = std::make_shared<CsrParasiticTree>();
  owner->terminals = {NodeId{0}, NodeId{1}};
  owner->lanes.fill(std::make_shared<const CsrParasiticModel>(model));
  CsrCpuRcCache empty;
  double net_delay = 0.0, net_slew = 0.0;
  require(evaluateCsrCpuNetTiming({owner, 1}, AnalysisMode::kMax, Transition::kRise,
              0.1, false, nullptr, {}, &empty, net_delay, net_slew) == CsrRcEvaluationStatus::kSuccess,
          "runtime missing driver did not use raw tree");
  near(net_delay, 0.06);
  near(net_slew, std::sqrt(0.01 + 0.06 * 0.06));
  require(evaluateCsrCpuNetTiming({owner, 1}, AnalysisMode::kMax, Transition::kRise,
              0.1, true, nullptr, {0.5, {0.1, 0.2}, {0.9, 0.8}}, &empty, net_delay, net_slew)
              == CsrRcEvaluationStatus::kSuccess, "runtime port fallback");
  near(net_delay, std::log(2.0) * 0.06);
  near(net_slew, 0.1 + std::log(9.0) / 0.5 * 0.06);
  require(evaluateCsrCpuNetTiming({owner, 2}, AnalysisMode::kMax, Transition::kRise,
              0.1, false, nullptr, {}, &empty, net_delay, net_slew) == CsrRcEvaluationStatus::kNumericFailure,
          "runtime accepted invalid terminal");


  const std::vector<CsrRcTreeNode> overflow{{0, 0.0, 1.0}, {0, 1e260, 1e-100}};
  const auto overflow_model = buildCsrParasiticModel(overflow, terminals);
  require(!overflow_model.fallback[1].valid, "overflow fixture did not exercise invalid impulse");
  owner->lanes.fill(std::make_shared<const CsrParasiticModel>(overflow_model));
  require(evaluateCsrCpuNetTiming({owner, 1}, AnalysisMode::kMax, Transition::kRise,
              0.1, false, nullptr, {}, &empty, net_delay, net_slew) == CsrRcEvaluationStatus::kNumericFailure,
          "RC overflow became a successful zero delay");

  const std::vector<CsrRcTreeNode> branch{
      {0, 0.0, 0.02}, {0, 3.0, 0.01}, {1, 2.0, 0.03}, {0, 4.0, 0.05}};
  const std::vector<std::size_t> branch_terminals{0, 2, 3};
  const auto branched = buildCsrParasiticModel(branch, branch_terminals);
  // Independent RC path sums: R1*(C1+C2)+R2*C2, and R3*C3.
  near(firstMoment(branched.arnoldi, 1), 0.18);
  near(firstMoment(branched.arnoldi, 2), 0.20);
  near(branched.fallback[1].delay, 0.18);
  near(branched.fallback[1].impulse, 2.0 * (3.0 * (0.01 * 0.12 + 0.03 * 0.18) + 2.0 * 0.03 * 0.18) - 0.18 * 0.18);
  near(branched.fallback[2].impulse, 0.20 * 0.20);
  auto changed = branch;
  changed[2].capacitance_pf = 0.06;
  const auto increased = buildCsrParasiticModel(changed, branch_terminals);
  near(firstMoment(increased.arnoldi, 1), 0.33);
  near(firstMoment(increased.arnoldi, 2), 0.20);
  near(firstMoment(branched.arnoldi, 1), 0.18);
}

void degenerateAndLarge()
{
  const std::vector<std::size_t> root{0};
  const std::vector<CsrRcTreeNode> zero{{0, 0.0, 0.0}};
  auto model = buildCsrParasiticModel(zero, root);
  require(model.arnoldi.order == 0 && !model.dmp.valid, "zero capacitance must use fallback");
  const std::vector<CsrRcTreeNode> lumped{{0, 0.0, 0.03}};
  model = buildCsrParasiticModel(lumped, root);
  require(model.arnoldi.order == 1 && model.dmp.valid, "lumped model");
  near(model.dmp.load_capacitance, 0.03);
  near(model.dmp.resistance, 0.0);

  std::vector<CsrRcTreeNode> tree{{0, 0.0, 0.01}};
  std::vector<std::size_t> terminals{0};
  for (std::size_t i = 1; i <= 4096; ++i) {
    tree.push_back({0, 1.0 + static_cast<double>(i % 7), 0.001});
    terminals.push_back(i);
  }
  model = buildCsrParasiticModel(tree, terminals);
  require(model.arnoldi.projection.size() == terminals.size(), "lost Arnoldi terminals");
  require(model.dmp.loads.size() == terminals.size(), "lost DMP terminals");
  near(firstMoment(model.arnoldi, 4096), tree[4096].resistance_kohm * 0.001);
}

void spectrumContract()
{
  const std::vector<CsrRcTreeNode> tree{{0, 0.0, 0.01}, {0, 2.0, 0.03}};
  const std::vector<std::size_t> terminals{0, 1};
  const auto model = buildCsrParasiticModel(tree, terminals).arnoldi;
  for (double resistance : {0.1, 1.0, 8.0}) {
    CsrArnoldiSpectrum spectrum;
    require(evaluateCsrArnoldiSpectrum(model.order, model.diagonal.data(),
        model.off_diagonal.data(), model.total_capacitance, resistance, spectrum)
        == CsrRcEvaluationStatus::kSuccess, "eigen solve failed");
    const double a = model.diagonal[0] + resistance * model.total_capacitance;
    const double b = model.off_diagonal[0];
    const double c = model.diagonal[1];
    const double gap = std::sqrt((a - c) * (a - c) + 4.0 * b * b);
    near(spectrum.eigenvalues[0], (a + c + gap) / 2.0);
    near(spectrum.eigenvalues[1], (a + c - gap) / 2.0);
    for (int i = 0; i < spectrum.order; ++i) {
      const auto* v = spectrum.eigenvectors[i];
      near(a * v[0] + b * v[1], spectrum.eigenvalues[i] * v[0]);
      near(b * v[0] + c * v[1], spectrum.eigenvalues[i] * v[1]);
    }
    for (const auto& projection : model.projection) {
      double residues[5]{};
      require(evaluateCsrArnoldiResidues(spectrum, model.sqrt_total_capacitance,
          projection.data(), residues) == CsrRcEvaluationStatus::kSuccess, "residue failed");
      near(residues[0] + residues[1], 1.0);
    }
  }
  const std::vector<CsrRcTreeNode> extreme{{0, 0.0, 1e-160}, {0, 1e160, 1e-160}};
  const auto fallback = buildCsrParasiticModel(extreme, terminals);
  require(fallback.arnoldi.order > 0 && !fallback.dmp.valid, "DMP failure discarded Arnoldi");
  near(fallback.arnoldi.diagonal[0], 0.5);
  CsrArnoldiSpectrum bad;
  require(evaluateCsrArnoldiSpectrum(0, nullptr, nullptr, 0.0, 1.0, bad)
      == CsrRcEvaluationStatus::kUnavailable, "empty model fallback");
  require(evaluateCsrArnoldiSpectrum(model.order, model.diagonal.data(),
      model.off_diagonal.data(), model.total_capacitance,
      std::numeric_limits<double>::infinity(), bad) == CsrRcEvaluationStatus::kNumericFailure,
      "invalid drive resistance");
}


// Independent scalar RC response, including the finite input ramp.
double analyticalVoltage(double time, double ramp, double tau)
{
  if (time < ramp)
    return 1.0 - time / ramp - tau * std::expm1(-time / tau) / ramp;
  return -tau * std::expm1(-ramp / tau) * std::exp(-(time - ramp) / tau) / ramp;
}
double analyticalCrossing(double target, double ramp, double tau)
{
  double low = 0.0, high = ramp + 100.0 * tau;
  for (int i = 0; i < 100; ++i) {
    const double mid = (low + high) / 2.0;
    if (analyticalVoltage(mid, ramp, tau) > target) low = mid;
    else high = mid;
  }
  return (low + high) / 2.0;
}
void waveformContract()
{
  const auto thresholds = csrArnoldiThresholds(10.0, 90.0, 0.8);
  near(thresholds.lower, 0.1);
  near(thresholds.upper, 0.9);
  const double residues[]{1.0};
  for (double tau : {0.01, 0.1, 1.0}) {
    const double poles[]{1.0 / tau};
    for (double ratio : {0.1, 1.0, 10.0, 100.0}) {
      const double ramp = ratio * tau;
      CsrArnoldiCrossings crossings;
      require(evaluateCsrArnoldiCrossings(ramp, 1, poles, residues, thresholds, crossings)
          == CsrRcEvaluationStatus::kSuccess, "RC threshold calculation");
      const double actual[]{crossings.upper_time, crossings.middle_time, crossings.lower_time};
      const double targets[]{0.9, 0.5, 0.1};
      for (int i = 0; i < 3; ++i) {
        const double expected = analyticalCrossing(targets[i], ramp, tau);
        require(std::abs(actual[i] - expected) < 1e-6, "RC analytical threshold mismatch");
      }
      // A single resistor/capacitor has Ceff=C at every charge measurement time.
      const double ceff = arnoldi_detail::effectiveCapacitance(ramp, 2.0, 1, poles, residues, ramp);
      near(ceff, tau / 2.0);
    }
  }
  CsrArnoldiCrossings invalid{1.0, 2.0, 3.0};
  const double wide_poles[]{1e-8, 10.0}, wide_residues[]{0.4, 0.6};
  require(evaluateCsrArnoldiCrossings(1.0, 2, wide_poles, wide_residues, thresholds, invalid)
      == CsrRcEvaluationStatus::kSuccess, "wide monotone tail failed");
  const double expected_tail = 1.0 - std::log(0.1 / (0.4 * (1.0 - std::exp(-1e-8)) / 1e-8)) / 1e-8;
  require(std::abs(invalid.lower_time - expected_tail) < 1e-6, "wide tail crossing mismatch");
  const double mixed_poles[]{0.01, 0.02, 10.0}, mixed_residues[]{1.0, -1.0, 1.0};
  double tail_time = 1.0, tail_value = 0.11;
  require(!arnoldi_detail::narrowMonotoneTail(1.0, 3, mixed_poles, mixed_residues, 0.1,
              100.0, tail_time, tail_value), "mixed waveform skipped without monotonicity proof");
  const double bad[]{std::numeric_limits<double>::infinity()};
  require(evaluateCsrArnoldiCrossings(1.0, 1, bad, residues, thresholds, invalid)
      == CsrRcEvaluationStatus::kNumericFailure, "nonfinite pole accepted");
  near(invalid.middle_time, 0.0);
}


struct RawTables
{
  const ChildTimingArcModel& child;
  double input_slew;
  double delay(double load) const
  {
    return child.delay_tables[0].present
        ? lookupCsrTimingTableRawLoad(child.delay_tables[0], input_slew, load,
              child.time_unit_scale, child.capacitance_unit_scale).value_or(std::numeric_limits<double>::quiet_NaN())
        : 0.0;
  }
  double slew(double load) const
  {
    return child.slew_tables[0].present
        ? lookupCsrTimingTableRawLoad(child.slew_tables[0], input_slew, load,
              child.time_unit_scale, child.capacitance_unit_scale).value_or(std::numeric_limits<double>::quiet_NaN())
        : input_slew;
  }
};
void driverContract()
{
  ChildTimingArcModel child;
  child.capacitance_unit_scale = 1000.0;
  child.slew_derate = 0.8;
  child.slew_lower_threshold_pct = {10.0, 10.0};
  child.slew_upper_threshold_pct = {90.0, 90.0};
  TimingTableModel table;
  table.present = true;
  table.variable1 = TableVariable::kOutputCapacitance;
  table.axes = {{0.0, 100.0}};
  table.values = {0.03, 0.23};  // delay = .03 + 2*C(pF)
  child.delay_tables[0] = table;
  table.values = {0.02, 0.32};  // slew = .02 + 3*C(pF)
  child.slew_tables[0] = table;
  const RawTables lookup{child, 0.1};
  near(lookup.delay(0.0123), 0.0546);
  near(*lookupCsrTimingTable(child.delay_tables[0], 0.1, 0.0123, 1.0, 1000.0), 0.056);

  const auto thresholds = csrArnoldiThresholds(10.0, 90.0, 0.8);
  const std::vector<std::size_t> terminals{0};
  const std::vector<CsrRcTreeNode> lumped{{0, 0.0, 0.04}};
  const auto model = buildCsrParasiticModel(lumped, terminals).arnoldi;
  const CsrArnoldiModelView view{model.order, model.diagonal.data(), model.off_diagonal.data(),
      model.projection[0].data(), model.total_capacitance, model.sqrt_total_capacitance};
  for (double offset : {0.02, 0.2}) {
    child.slew_tables[0].values = {offset, offset + 0.3};
    CsrArnoldiDriverResult result;
    require(evaluateCsrArnoldiDriver(view, thresholds, lookup, result)
        == CsrRcEvaluationStatus::kSuccess, "driver evaluation");
    near(result.effective_capacitance, 0.04);
    require(std::abs(result.gate_delay - lookup.delay(0.04)) < 1e-6, "lumped gate delay");
    double wire_delay = 1.0, load_slew = 0.0;
    require(evaluateCsrArnoldiLoad(result, model.sqrt_total_capacitance, model.projection[0].data(),
        thresholds, wire_delay, load_slew) == CsrRcEvaluationStatus::kSuccess, "driver load");
    near(wire_delay, 0.0);
    near(load_slew, result.driver_slew);
    auto tree = std::make_shared<CsrParasiticTree>();
    tree->terminals = {NodeId{0}};
    tree->lanes.fill(std::make_shared<const CsrParasiticModel>(buildCsrParasiticModel(lumped, terminals)));
    CsrCpuRcCache runtime;
    double runtime_delay = 0.0, runtime_slew = 0.0;
    require(evaluateCsrCpuRcChild({tree, 0}, {NodeId{0}, false, 0, 0, AnalysisMode::kMax, Transition::kRise},
                child, 0.1, runtime, runtime_delay, runtime_slew) == CsrRcEvaluationStatus::kSuccess,
            "runtime driver evaluation");
    near(runtime_delay, result.gate_delay);
    near(runtime_slew, result.driver_slew);
    require(evaluateCsrCpuRcLoad({tree, 0}, AnalysisMode::kMax, Transition::kRise, runtime_slew, nullptr,
                runtime, wire_delay, load_slew) == CsrRcEvaluationStatus::kSuccess, "runtime terminal read");
    near(wire_delay, 0.0);
    near(load_slew, runtime_slew);
  }
  CsrDmpDriverResult dmp;
  const auto dmp_thresholds = csrDmpThresholds(50.0, 10.0, 90.0, 0.8);
  require(evaluateCsrDmpDriver(0.0, 0.0, 0.04, dmp_thresholds, lookup, dmp)
      == CsrRcEvaluationStatus::kSuccess, "DMP lumped");
  near(dmp.effective_capacitance, 0.04);
  near(dmp.gate_delay, lookup.delay(0.04));
  near(dmp.driver_slew, lookup.slew(0.04));
  require(evaluateCsrDmpDriver(-1.0, 1.0, 0.04, dmp_thresholds, lookup, dmp)
      == CsrRcEvaluationStatus::kNumericFailure, "DMP negative capacitance");
  near(dmp.effective_capacitance, 0.0);
  near(dmp.gate_delay, 0.0);
  near(dmp.driver_slew, 0.0);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double bad_poles[2]{1.0, nan}, good_poles[2]{1.0, 2.0};
  const double bad_residues[2]{1.0, nan}, good_residues[2]{1.0, 1.0};
  double dmp_wire = 7.0, dmp_slew = 8.0;
  for (int invalid = 0; invalid < 3; ++invalid) {
    auto limits = dmp_thresholds;
    if (invalid == 2) limits.upper = nan;
    require(evaluateCsrDmpLoad(2, invalid == 0 ? bad_poles : good_poles,
                invalid == 1 ? bad_residues : good_residues, 1.0, limits, dmp_wire, dmp_slew)
                == CsrRcEvaluationStatus::kNumericFailure, "invalid DMP load accepted as fallback");
    near(dmp_wire, 0.0);
    near(dmp_slew, 0.0);
  }
  // Both table-less defaults remain the native ones, including slew passthrough.
  child.delay_tables[0].present = false;
  child.slew_tables[0].present = false;
  near(lookup.delay(0.04), 0.0);
  near(lookup.slew(0.04), 0.1);
}

void treePreparation()
{
  ParasiticNetModel net;
  net.map_key = net.embedded_name = "n";
  net.nodes = {{"u/Y", "u/Y", 0.01, 0.0, 0.0}, {"v/A", "v/A", 0.02, 0.0, 0.0},
               {"w/A", "w/A", 0.03, 0.0, 0.0}};
  // Steiner point has no explicit capacitance entry. Resistor order is native DFS order.
  net.resistors = {{"u/Y", "s", 1000.0}, {"s", "w/A", 2000.0}, {"s", "v/A", 3000.0}};
  std::vector<CsrRcPin> pins{{NodeId{7}, "u:Y", {}}, {NodeId{9}, "v:A", {}}, {NodeId{8}, "w:A", {}}};
  pins[0].capacitance = {{{0.001, 0.001}, {0.001, 0.001}}};
  pins[1].capacitance = {{{0.004, 0.006}, {0.004, 0.006}}};
  pins[2].capacitance = {{{0.003, 0.003}, {0.003, 0.003}}};
  const auto result = prepareCsrParasiticTree(net, NodeId{7}, pins);
  require(result.terminals == std::vector<NodeId>{NodeId{7}, NodeId{8}, NodeId{9}}, "RC terminal order");
  near(result.lanes[0]->arnoldi.total_capacitance, 0.068);
  near(result.lanes[1]->arnoldi.total_capacitance, 0.070);
  require(result.lanes[0] == result.lanes[2] && result.lanes[1] == result.lanes[3]
              && result.lanes[0] != result.lanes[1], "identical RC load lanes not shared");
  // Native resistor values are Ohms and each pin capacitance appears exactly once.
  near(firstMoment(result.lanes[0]->arnoldi, 1), 1.0 * (0.033 + 0.024) + 2.0 * 0.033);
  near(firstMoment(result.lanes[0]->arnoldi, 2), 1.0 * (0.033 + 0.024) + 3.0 * 0.024);
  for (int malformed = 0; malformed < 6; ++malformed) {
    auto bad = net;
    auto bad_pins = pins;
    if (malformed == 0) bad.resistors.push_back({"v/A", "w/A", 100.0});
    if (malformed == 1) bad.resistors.pop_back();
    if (malformed == 2) bad_pins[1].node = bad_pins[0].node;
    if (malformed == 3) bad_pins[1].capacitance[0][0] = -1.0;
    // A resistor-only Steiner node is valid; a pin must have native node_map identity.
    if (malformed == 4) bad.nodes.erase(bad.nodes.begin());
    if (malformed == 5) bad.nodes.erase(bad.nodes.begin() + 1);
    bool rejected = false;
    try { (void) prepareCsrParasiticTree(bad, NodeId{7}, bad_pins); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid RC tree accepted");
  }
}

void driverCache()
{
  std::array<CsrRcTimingRecord, 8> records;
  std::array<CsrRcTimingRow, 3> rows{{{0, 4, 0}, {4, 2, 0}, {6, 2, 0}}};
  CsrRcDriverLane driver{0, rows.size()};
  CsrRcTimingRecord pending;
  pending.input_slew = 0.1;
  pending.child_model = 7;
  pending.family = CsrRcDriverFamily::arnoldi;
  pending.arnoldi.driver_slew = 0.3;
  std::uint64_t a, b, ctq, unavailable;
  require(appendCsrRcTimingRecord(records.data(), rows[0], driver, pending, a), "cache A");
  pending.child_model = 8;
  require(appendCsrRcTimingRecord(records.data(), rows[1], driver, pending, b), "cache B");
  require(findCsrRcTimingRecord(records.data(), rows[0], 0.1) == a, "A must hit");
  selectCsrRcDriverRecords(records.data(), rows.data(), driver, 0.3);
  require(driver.selected_arnoldi == b && driver.miss_sequence == 2, "hit changed last writer");

  // A value-equal CTQ copy belongs to a distinct native child row.
  pending.child_model = 7;
  require(appendCsrRcTimingRecord(records.data(), rows[2], driver, pending, ctq), "cache CTQ");
  pending.input_slew = 0.2;
  pending.family = CsrRcDriverFamily::dmp;
  pending.dmp.driver_slew = 0.3;
  require(appendCsrRcTimingRecord(records.data(), rows[0], driver, pending, b), "cache DMP");
  selectCsrRcDriverRecords(records.data(), rows.data(), driver, 0.3);
  require(driver.selected_arnoldi == ctq && driver.selected_dmp == b, "lost family priority");
  pending.input_slew = 0.4;
  pending.family = CsrRcDriverFamily::unavailable;
  require(appendCsrRcTimingRecord(records.data(), rows[0], driver, pending, unavailable), "cache unavailable");
  require(findCsrRcTimingRecord(records.data(), rows[0], 0.4) == unavailable, "unavailable must hit");
  const auto sequence = driver.miss_sequence;
  selectCsrRcDriverRecords(records.data(), rows.data(), driver, 0.9);
  require(driver.selected_arnoldi == kCsrRcAbsent && driver.selected_dmp == kCsrRcAbsent, "stale driver selection");
  pending.input_slew = std::numeric_limits<double>::quiet_NaN();
  require(!appendCsrRcTimingRecord(records.data(), rows[0], driver, pending, b)
              && driver.miss_sequence == sequence, "failed append changed cache");
  pending = records[a];
  pending.input_slew = -0.0;
  require(appendCsrRcTimingRecord(records.data(), rows[0], driver, pending, b), "cache zero");
  require(findCsrRcTimingRecord(records.data(), rows[0], 0.0) == b, "Arnoldi zero identity");
  pending.input_slew = 1.0;
  require(!appendCsrRcTimingRecord(records.data(), rows[0], driver, pending, b), "capacity overflow accepted");
  for (const auto family : {CsrRcDriverFamily::dmp, CsrRcDriverFamily::unavailable}) {
    rows[0] = {0, 4, 0};
    driver = {0, 1};
    pending = {};
    pending.family = family;
    pending.input_slew = -0.0;
    pending.dmp.driver_slew = -0.0;
    require(appendCsrRcTimingRecord(records.data(), rows[0], driver, pending, a), "negative zero cache");
    require(findCsrRcTimingRecord(records.data(), rows[0], -0.0) == a
                && findCsrRcTimingRecord(records.data(), rows[0], 0.0) == kCsrRcAbsent,
            "native DMP string key merged signed zeros");
    if (family == CsrRcDriverFamily::dmp) {
      selectCsrRcDriverRecords(records.data(), rows.data(), driver, 0.0);
      require(driver.selected_dmp == kCsrRcAbsent, "DMP selected opposite signed zero");
      selectCsrRcDriverRecords(records.data(), rows.data(), driver, -0.0);
      require(driver.selected_dmp == a, "DMP lost matching signed zero");
    }
  }
}

void rejectedInputs()
{
  const std::vector<std::size_t> terminals{0, 1};
  const std::vector<std::vector<CsrRcTreeNode>> malformed{
      {}, {{0, 1.0, 0.1}}, {{0, 0.0, 0.1}, {1, 2.0, 0.1}},
      {{0, 0.0, 0.1}, {0, -1.0, 0.1}},
      {{0, 0.0, 0.1}, {0, 1.0, std::numeric_limits<double>::quiet_NaN()}}};
  for (const auto& tree : malformed) {
    bool rejected = false;
    try { (void) buildCsrParasiticModel(tree, terminals); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "malformed tree accepted");
  }
  const std::vector<CsrRcTreeNode> overflow{{0, 0.0, 1e308}, {0, 1e308, 1e308}};
  bool rejected = false;
  try { (void) buildCsrParasiticModel(overflow, terminals); }
  catch (const std::overflow_error&) { rejected = true; }
  require(rejected, "overflow accepted");
}
}  // namespace

int main()
{
  try {
    rampBoundary();
    analyticalNetworks();
    degenerateAndLarge();
    rejectedInputs();
    treePreparation();
    driverCache();
    spectrumContract();
    waveformContract();
    driverContract();
    std::cout << "RC structure reduction passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
