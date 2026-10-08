// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrArnoldiDriver.hpp"
#include "CsrDmpLoad.hpp"
#include "CsrParasiticTree.hpp"
#include "CsrParasiticThreshold.hpp"
#include "CsrTableLookup.hpp"
#include "CsrCpuReferenceParasitics.hpp"
#include "CsrCpuReferenceBackendInternal.hpp"
#include "FlatCsrEdgeView.hpp"
#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "Logger.hpp"
#include "Utility.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace ista {
// Only this diagnostic target defines the friend; native math remains independent.
class CsrParasiticNativeTestAccess
{
 public:
  static double tableRamp(double slew)
  {
    TimingTable table; table.set_value_list({slew});
    TimingArc arc; arc.set_time_unit_scale(1.0); arc.set_cap_unit_scale(1.0);
    arc.get_slew_table_map()[TransType::kRise] = table;
    const double cap = 0.013552116249999998, log = std::log(0.7 / 0.3);
    const double resistance = 0.5 * slew / (log * cap);
    return STADC.calcParasiticArnoldiTableRamp(arc, TransType::kRise, 0.0, resistance, cap,
        0.3, 0.7, log, 0.5, 0.76143368254511556);
  }
  static const ParasiticArnoldiTimingResult& arnoldi(
      TimingArc& arc, AnalysisType analysis, TransType transition, double input_slew)
  {
    std::string pin = "p0";
    const double load = STADC.getOutputPinLoad(pin, analysis, transition);
    return STADC.getParasiticArnoldiTimingResult(pin, arc, analysis, transition, input_slew, load);
  }
  static ParasiticDmpTimingResult& dmp(TimingArc& arc, AnalysisType analysis, TransType transition, double input_slew)
  {
    std::string pin = "p0";
    const double load = STADC.getOutputPinLoad(pin, analysis, transition);
    return STADC.getParasiticDmpTimingResult(pin, arc, analysis, transition, input_slew, load);
  }
  static csr::CsrArnoldiCrossings crossings(const std::vector<double>& poles, const std::vector<double>& residues)
  {
    ParasiticArnoldiPoleResidue model;
    model.set_is_valid(true);
    model.set_order(static_cast<int32_t>(poles.size()));
    model.set_pole_list(poles);
    model.set_residue_list({residues});
    csr::CsrArnoldiCrossings result;
    STADC.solveParasiticArnoldiWaveformTime(
        1.0, model, 0, 0.9, result.upper_time, 0.5, result.middle_time, 0.1, result.lower_time);
    return result;
  }
  static std::pair<double, double> fallback(std::string sink, AnalysisType analysis, TransType transition)
  {
    Arc arc;
    arc.set_type(ArcType::kNet); arc.set_owner_name("n");
    arc.set_source_pin("p0"); arc.set_sink_pin(sink);
    // No driver result can use this unrequested slew: exercise the native
    // raw-tree cache, independently of reduced Arnoldi/DMP load evaluation.
    return {STADC.calcParasiticDelay(arc, analysis, transition, 1234.0),
            STADC.calcParasiticSlew(arc, analysis, transition, 1234.0)};
  }
  static std::pair<double, double> inputPort(std::string sink, AnalysisType analysis, TransType transition, double slew)
  {
    std::string source = "p0";
    auto& net = STADM.getDatabase().get_parasitic_library().get_net_map().at("n");
    return {STADC.calcParasiticArnoldiInputPortDelay(net, source, sink, analysis, transition).value(),
            STADC.calcParasiticArnoldiInputPortSlew(net, source, sink, analysis, transition, slew).value()};
  }
  static void adjust(TimingArc& arc, TransType transition, double& delay, double& slew)
  {
    std::string sink = "p1";
    STADC.adjustParasiticLoadThreshold(arc, sink, transition, delay, slew);
  }
  static double rootLoad()
  {
    std::string pin = "p0";
    return STADC.getOutputPinLoad(pin, AnalysisType::kMax, TransType::kRise);
  }
  static std::pair<double, double> byLoad(TimingArc& arc, double input_slew, double load)
  {
    return {STADC.calcTimingArcDelayByLoad(arc, TransType::kRise, input_slew, load),
            STADC.calcTimingArcSlewByLoad(arc, TransType::kRise, input_slew, load)};
  }
  static ParasiticDmpModel& dmpModel(AnalysisType analysis, TransType transition)
  {
    std::string source = "p0";
    auto& net = STADM.getDatabase().get_parasitic_library().get_net_map().at("n");
    return STADC.getParasiticDmpModel(net, source, analysis, transition);
  }
};
}  // namespace ista

namespace {
using namespace ista;
using namespace ista::csr;

void require(bool ok, const char* message)
{
  if (!ok) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* field)
{
  if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual - expected) > 1e-6)
    throw std::runtime_error(std::string(field) + ": " + std::to_string(actual) + " != " + std::to_string(expected));
}
void compareNetArc(const CsrParasiticBinding& binding, AnalysisMode analysis, Transition transition,
                   double input_slew, bool input_port, const InputPortRcThresholds& thresholds,
                   CsrCpuRcCache& cache, double initial_delay, double delay, double slew,
                   const ReferencedLibraryCellModel* load_cell = nullptr)
{
  CsrReferenceInput input;
  input.nodes.resize(binding.tree->terminals.size());
  for (auto& node : input.nodes) node.is_port = true;
  input.nodes[0].is_port = input_port;
  ArcRecord arc;
  arc.id = ArcId{0}; arc.source = NodeId{0};
  arc.sink = binding.tree->terminals[binding.terminal];
  arc.kind = ArcKind::kNet; arc.parasitic = binding;
  input.arcs.push_back(arc);
  if (load_cell) {
    input.nodes[arc.sink.value()].is_port = false;
    input.nodes[arc.sink.value()].instance_cell = load_cell->cell_name;
    input.referenced_library_cells.push_back(*load_cell);
  }
  ista::Arc native_arc;
  native_arc.set_type(ArcType::kNet); native_arc.set_owner_name("n");
  native_arc.set_source_pin("p0"); native_arc.set_sink_pin("p" + std::to_string(binding.terminal));
  DCTask initialization;
  initialization.set_proc_type(DCProcType::kInitialize); initialization.set_arc(&native_arc);
  STADC.calculate(initialization);
  require(initialization.get_is_valid(), "native public net initialization failed");
  near(native_arc.get_trans_delay_map()
           [analysis == AnalysisMode::kMax ? AnalysisType::kMax : AnalysisType::kMin]
           [transition == Transition::kRise ? TransType::kRise : TransType::kFall],
       initial_delay, "native initialized delay consulted a dynamic driver");
  FlatCsrEdgeView edges(input);
  CsrCpuReferenceWorkState work;
  work.rc_nodes = input.nodes;
  work.rc_cells = input.referenced_library_cells;
  work.input_port_thresholds = thresholds;
  work.rc = std::move(cache);
  require(initializeCsrCpuReferenceArcs(edges, work) == CsrCpuReferenceStageStatus::kSuccess,
          "RC net initialization failed");
  near(work.result.arcs.input_output_delays.values[csrArcIoLane(0, analysis, transition, transition)],
       initial_delay, "initial net delay used dynamic driver cache");
  CsrDynamicArcValue result;
  require(evaluateCsrDynamicArc(edges, ArcId{0}, analysis, transition, transition, input_slew, work, result)
              == CsrCpuReferenceStageStatus::kSuccess && result.present, "dynamic net dispatch failed");
  near(result.delay, delay, "dynamic net delay");
  near(result.slew, slew, "dynamic net slew");
  cache = std::move(work.rc);
}

class NativeScope
{
 public:
  NativeScope()
  {
    Logger::initInst(); Utility::initInst(); DataManager::initInst(); DelayCalculator::initInst();
    STADC.init();
  }
  ~NativeScope()
  {
    STADC.destroy();
    DelayCalculator::destroyInst(); DataManager::destroyInst(); Utility::destroyInst(); Logger::destroyInst();
  }
};
struct Tables
{
  TimingTableModel delay_table;
  TimingTableModel slew_table;
  double input_slew{0.1};
  double time_scale{1.0};
  double delay(double load) const
  {
    return lookupCsrTimingTableRawLoad(delay_table, input_slew, load, time_scale, 1000.0)
        .value_or(std::numeric_limits<double>::quiet_NaN());
  }
  double slew(double load) const
  {
    return lookupCsrTimingTableRawLoad(slew_table, input_slew, load, time_scale, 1000.0)
        .value_or(std::numeric_limits<double>::quiet_NaN());
  }
};

void installTree(const std::vector<CsrRcTreeNode>& tree, double pin_load)
{
  auto& db = STADM.getDatabase();
  db.get_pin_map().clear();
  db.get_net_map().clear();
  db.get_parasitic_library().get_net_map().clear();
  db.get_timing_constraint().get_port_constraint_map().clear();
  Net net;
  net.set_net_name("n");
  net.set_driver_pin("p0");
  ParasiticNet parasitic;
  parasitic.set_net_name("n");
  double total = 0.0;
  for (std::size_t i = 0; i < tree.size(); ++i) {
    const std::string name = "p" + std::to_string(i);
    Pin pin;
    pin.set_full_name(name); pin.set_pin_name(name); pin.set_net_name("n");
    pin.set_is_port(i != 0);
    pin.set_direction(i == 0 ? ista::PinDirection::kOutput : ista::PinDirection::kInput);
    db.get_pin_map().emplace(name, pin);
    net.get_pin_name_list().push_back(name);
    if (i) net.get_load_pin_list().push_back(name);
    if (i) {
      TimingPortConstraint constraint;
      constraint.set_port_name(name);
      constraint.set_has_load(true);
      constraint.set_load(pin_load);
      db.get_timing_constraint().get_port_constraint_map().emplace(name, constraint);
    }
    ParasiticNode node;
    node.set_node_name(name);
    node.set_capacitance(tree[i].capacitance_pf);
    parasitic.get_node_map().emplace(name, node);
    total += tree[i].capacitance_pf;
    if (i) {
      ParasiticResistor resistor;
      resistor.set_source_node("p" + std::to_string(tree[i].parent));
      resistor.set_sink_node(name);
      resistor.set_resistance(tree[i].resistance_kohm * 1000.0);
      parasitic.get_resistor_list().push_back(resistor);
    }
  }
  parasitic.set_lumped_capacitance(total);
  auto& library = db.get_parasitic_library();
  library.set_capacitive_unit("PF"); library.set_resistance_unit("OHM");
  library.get_net_map().emplace("n", std::move(parasitic));
  db.get_net_map().emplace("n", std::move(net));
  STADC.resetForEditRollback();
}

void compare(const std::vector<CsrRcTreeNode>& tree, double slew_offset, double time_scale,
             double input_slew, double output_threshold, const std::string& load_library = {})
{
  const double pin_load = 0.002;
  installTree(tree, pin_load);
  ReferencedLibraryCellModel sink_cell;
  const ReferencedLibraryCellModel* load_cell = nullptr;
  if (!load_library.empty()) {
    auto& db = STADM.getDatabase();
    auto& pin = db.get_pin_map().at("p1");
    pin.set_is_port(false); pin.set_instance_name("sink");
    Instance instance; instance.set_instance_name("sink"); instance.set_cell_name("LOAD");
    db.get_instance_map().insert_or_assign("sink", instance);
    TimingCell cell; cell.set_cell_name("LOAD"); cell.set_library_name(load_library);
    cell.set_slew_derate_from_library(0.6);
    cell.set_slew_lower_threshold_pct_rise(15.0); cell.set_slew_lower_threshold_pct_fall(15.0);
    cell.set_slew_upper_threshold_pct_rise(85.0); cell.set_slew_upper_threshold_pct_fall(85.0);
    cell.set_input_threshold_pct_rise(45.0); cell.set_input_threshold_pct_fall(45.0);
    TimingCellPort port; port.set_port_name("p1"); port.set_capacitance(pin_load);
    cell.get_port_map().emplace("p1", port);
    db.get_timing_library().get_cell_map().insert_or_assign("LOAD", cell);
    sink_cell.present = true; sink_cell.cell_name = "LOAD"; sink_cell.library_name = load_library;
    sink_cell.slew_derate = 0.6; sink_cell.slew_lower_threshold_pct = {0.15, 0.15};
    sink_cell.slew_upper_threshold_pct = {0.85, 0.85}; sink_cell.input_threshold_pct = {0.45, 0.45};
    load_cell = &sink_cell;
  }
  Tables tables;
  tables.input_slew = input_slew;
  tables.time_scale = time_scale;
  tables.delay_table = {true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance,
                        {{0.0, 1.0}, {0.0, 1000.0}}, {0.03, 2.03, 0.13, 2.13}};
  tables.slew_table = {true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance,
                       {{0.0, 1.0}, {0.0, 1000.0}}, {slew_offset, slew_offset + 3.0, slew_offset + 0.2, slew_offset + 3.2}};
  TimingTable delay, slew;
  delay.set_variable_type1(TimingTableVariableType::kInputTransition);
  delay.set_variable_type2(TimingTableVariableType::kOutputCapacitance);
  delay.set_axis_list({{0.0, 1.0}, {0.0, 1000.0}});
  delay.set_value_list({0.03, 2.03, 0.13, 2.13});
  slew.set_variable_type1(TimingTableVariableType::kInputTransition);
  slew.set_variable_type2(TimingTableVariableType::kOutputCapacitance);
  slew.set_axis_list({{0.0, 1.0}, {0.0, 1000.0}});
  slew.set_value_list({slew_offset, slew_offset + 3.0, slew_offset + 0.2, slew_offset + 3.2});
  TimingArc child;
  child.set_library_name("driver.lib");
  child.set_sense(TimingArcSense::kPositive);
  child.set_cap_unit_scale(1000.0); child.set_slew_derate(0.8);
  child.set_time_unit_scale(time_scale);
  child.set_output_threshold_pct_rise(output_threshold);
  child.set_output_threshold_pct_fall(output_threshold);
  child.set_slew_lower_threshold_pct_rise(10.0); child.set_slew_lower_threshold_pct_fall(10.0);
  child.set_slew_upper_threshold_pct_rise(90.0); child.set_slew_upper_threshold_pct_fall(90.0);
  for (auto transition : {TransType::kRise, TransType::kFall}) {
    child.get_delay_table_map()[transition] = delay;
    child.get_slew_table_map()[transition] = slew;
  }
  TimingCellArc cell;
  cell.get_timing_arc_list().push_back(child);
  ParasiticNetModel input;
  input.map_key = input.embedded_name = "n";
  std::vector<std::string> names;
  names.reserve(tree.size());
  std::vector<CsrRcPin> pins;
  for (std::size_t i = 0; i < tree.size(); ++i) {
    names.push_back("p" + std::to_string(i));
    input.nodes.push_back({names.back(), names.back(), tree[i].capacitance_pf, 0.0, 0.0});
    if (i) input.resistors.push_back({"p" + std::to_string(tree[i].parent), names.back(), tree[i].resistance_kohm * 1000.0});
    const double capacitance = i ? pin_load : 0.0;
    pins.push_back({NodeId{i}, names.back(), {{{capacitance, capacitance}, {capacitance, capacitance}}}});
  }
  const auto prepared = prepareCsrParasiticTree(input, NodeId{0}, pins);
  require(prepared.terminals.size() == tree.size(), "prepared RC terminal count");
  const auto& reduced = *prepared.lanes[0];
  const auto& model = reduced.arnoldi;
  const CsrArnoldiModelView view{model.order, model.diagonal.data(), model.off_diagonal.data(),
      model.projection[0].data(), model.total_capacitance, model.sqrt_total_capacitance};
  const auto thresholds = csrArnoldiThresholds(10.0, 90.0, 0.8);
  const CsrParasiticBinding binding{std::make_shared<const CsrParasiticTree>(prepared), 0};
  ChildTimingArcModel exported_child;
  exported_child.signature.library_name = "driver.lib";
  exported_child.time_unit_scale = time_scale;
  exported_child.capacitance_unit_scale = 1000.0;
  exported_child.slew_derate = 0.8;
  exported_child.slew_lower_threshold_pct = {10.0, 10.0};
  exported_child.slew_upper_threshold_pct = {90.0, 90.0};
  exported_child.output_threshold_pct = {output_threshold, output_threshold};
  exported_child.delay_tables = {tables.delay_table, tables.delay_table};
  exported_child.slew_tables = {tables.slew_table, tables.slew_table};
  CsrCpuRcCache runtime;
  // Force dispatch to the independently tested native DMP family without
  // claiming that this otherwise valid fixture naturally disables Arnoldi.
  auto dmp_model = std::make_shared<CsrParasiticModel>(reduced);
  dmp_model->arnoldi.order = 0;
  auto dmp_tree = std::make_shared<CsrParasiticTree>();
  dmp_tree->terminals = prepared.terminals; dmp_tree->lanes.fill(dmp_model);
  CsrCpuRcCache dmp_runtime;
  CsrArnoldiDriverResult result;
  require(evaluateCsrArnoldiDriver(view, thresholds, tables, result) == CsrRcEvaluationStatus::kSuccess, "CSR driver failed");
  for (auto analysis : {AnalysisType::kMax, AnalysisType::kMin}) {
    for (auto transition : {TransType::kRise, TransType::kFall}) {
      DCTask task;
      task.set_proc_type(DCProcType::kCalculate);
      task.set_timing_cell_arc(&cell); task.set_output_pin("p0");
      task.set_analysis_type(analysis); task.set_input_trans_type(transition); task.set_output_trans_type(transition);
      task.set_input_slew(input_slew);
      STADC.calculate(task);
      require(task.get_is_valid(), "native driver failed");
      const auto& native = CsrParasiticNativeTestAccess::arnoldi(
          cell.get_timing_arc_list().front(), analysis, transition, input_slew);
      require(native.get_is_valid(), "native Arnoldi branch unavailable");
      near(result.effective_capacitance, native.get_effective_capacitance(), "effective capacitance");
      near(result.gate_delay, task.get_timing_result().get_delay(), "gate delay");
      near(result.driver_slew, task.get_timing_result().get_slew(), "driver slew");
      const auto a = analysis == AnalysisType::kMax ? AnalysisMode::kMax : AnalysisMode::kMin;
      const auto tr = transition == TransType::kRise ? Transition::kRise : Transition::kFall;
      const CsrCpuRcChildKey child_key{NodeId{0}, false, 0, 0, a, tr};
      double evaluated_delay = 0.0, evaluated_slew = 0.0;
      require(evaluateCsrCpuCellTiming(binding, child_key, exported_child, input_slew, -1.0, false,
                  runtime, evaluated_delay) == CsrRcEvaluationStatus::kSuccess
              && evaluateCsrCpuCellTiming(binding, child_key, exported_child, input_slew, -1.0, true,
                  runtime, evaluated_slew) == CsrRcEvaluationStatus::kSuccess,
              "CPU runtime driver evaluation failed");
      near(evaluated_delay, task.get_timing_result().get_delay(), "runtime gate delay");
      near(evaluated_slew, task.get_timing_result().get_slew(), "runtime driver slew");
      const auto count = runtime.timing.size();
      double repeated = 0.0;
      require(evaluateCsrCpuCellTiming(binding, child_key, exported_child, input_slew, -1.0, false,
                  runtime, repeated) == CsrRcEvaluationStatus::kSuccess && runtime.timing.size() == count,
              "delay/slew or repeated call created duplicate driver evaluations");
      near(repeated, evaluated_delay, "cached gate delay");
      auto second_key = child_key;
      std::get<2>(second_key) = 1;
      require(evaluateCsrCpuCellTiming(binding, second_key, exported_child, input_slew, -1.0, false,
                  runtime, repeated) == CsrRcEvaluationStatus::kSuccess, "second native child failed");
      const CsrCpuRcDriverKey driver_key{NodeId{0}, a, tr, evaluated_slew, false};
      const auto* second = runtime.arnoldi_drivers.at(driver_key);
      require(evaluateCsrCpuCellTiming(binding, child_key, exported_child, input_slew, -1.0, false,
                  runtime, repeated) == CsrRcEvaluationStatus::kSuccess
              && runtime.arnoldi_drivers.at(driver_key) == second, "cache hit overwrote last real miss");
      auto ctq_key = child_key;
      std::get<1>(ctq_key) = true;
      require(evaluateCsrCpuCellTiming(binding, ctq_key, exported_child, input_slew, -1.0, false,
                  runtime, repeated) == CsrRcEvaluationStatus::kSuccess
              && runtime.arnoldi_drivers.at(driver_key) != second, "Instance CTQ identity was interned with graph child");
      CsrCpuRcCache zeros;
      require(evaluateCsrCpuCellTiming(binding, child_key, exported_child, 0.0, -1.0, false, zeros, repeated)
                  == CsrRcEvaluationStatus::kSuccess
              && evaluateCsrCpuCellTiming(binding, child_key, exported_child, -0.0, -1.0, false, zeros, repeated)
                  == CsrRcEvaluationStatus::kSuccess
              && zeros.timing.size() == 1, "Arnoldi signed zeros were not a shared hit");
      auto& native_dmp = CsrParasiticNativeTestAccess::dmp(
          cell.get_timing_arc_list().front(), analysis, transition, input_slew);
      const auto& native_model = CsrParasiticNativeTestAccess::dmpModel(analysis, transition);
      require(reduced.dmp.valid == native_model.get_is_valid(), "DMP structure availability differs");
      require(native_dmp.get_is_valid(), "native DMP solve failed on a valid fixture");
      if (reduced.dmp.valid) {
        near(reduced.dmp.driver_capacitance, native_model.get_driver_capacitance(), "DMP driver capacitance");
        near(reduced.dmp.resistance, native_model.get_pi_resistance(), "DMP pi resistance");
        near(reduced.dmp.load_capacitance, native_model.get_load_capacitance(), "DMP load capacitance");
        CsrDmpDriverResult dmp;
        require(evaluateCsrDmpDriver(reduced.dmp.driver_capacitance, reduced.dmp.resistance,
                    reduced.dmp.load_capacitance, csrDmpThresholds(output_threshold, 10.0, 90.0, 0.8), tables, dmp)
                    == CsrRcEvaluationStatus::kSuccess, "CSR DMP driver failed");
        near(dmp.gate_delay, native_dmp.get_gate_delay(), "DMP gate delay");
        near(dmp.driver_slew, native_dmp.get_driver_slew(), "DMP slew");
        near(dmp.effective_capacitance, native_dmp.get_effective_capacitance(), "DMP Ceff");
        double runtime_dmp_delay = 0.0, runtime_dmp_slew = 0.0;
        require(evaluateCsrCpuRcChild({dmp_tree, 0}, child_key, exported_child, input_slew,
                    dmp_runtime, runtime_dmp_delay, runtime_dmp_slew) == CsrRcEvaluationStatus::kSuccess,
                "runtime DMP dispatch failed");
        near(runtime_dmp_delay, native_dmp.get_gate_delay(), "runtime DMP gate");
        near(runtime_dmp_slew, native_dmp.get_driver_slew(), "runtime DMP slew");
        for (std::size_t terminal = 1; terminal < tree.size(); ++terminal) {
          const auto& load_model = reduced.dmp.loads[terminal];
          double wire = 0.0, slew = 0.0;
          const auto status = evaluateCsrDmpLoad(load_model.order, load_model.poles.data(), load_model.residues.data(),
              dmp.driver_slew, csrDmpThresholds(output_threshold, 10.0, 90.0, 0.8), wire, slew);
          const std::string pin = "p" + std::to_string(terminal);
          require((status == CsrRcEvaluationStatus::kSuccess) == native_dmp.get_wire_delay_map().contains(pin),
                  "DMP load availability differs");
          if (status == CsrRcEvaluationStatus::kSuccess) {
            if (!load_cell) {
              near(wire, native_dmp.get_wire_delay_map().at(pin), "DMP wire delay");
              near(slew, native_dmp.get_load_slew_map().at(pin), "DMP load slew");
            }
            double cached_delay = 0.0, cached_slew = 0.0;
            require(evaluateCsrCpuRcLoad({dmp_tree, terminal}, a, tr, runtime_dmp_slew, load_cell,
                        dmp_runtime, cached_delay, cached_slew) == CsrRcEvaluationStatus::kSuccess,
                    "runtime DMP terminal dispatch failed");
            near(cached_delay, native_dmp.get_wire_delay_map().at(pin), "runtime DMP wire");
            near(cached_slew, native_dmp.get_load_slew_map().at(pin), "runtime DMP load");
          }
        }
      }
      for (std::size_t terminal = 1; terminal < tree.size(); ++terminal) {
        Arc arc;
        arc.set_type(ArcType::kNet); arc.set_owner_name("n");
        arc.set_source_pin("p0"); arc.set_sink_pin("p" + std::to_string(terminal));
        DCTask load;
        load.set_proc_type(DCProcType::kCalculate); load.set_arc(&arc); load.set_analysis_type(analysis);
        load.set_input_trans_type(transition); load.set_output_trans_type(transition);
        load.set_input_slew(task.get_timing_result().get_slew());
        STADC.calculate(load);
        require(load.get_is_valid(), "native load failed");
        double wire_delay = 0.0, load_slew = 0.0;
        require(evaluateCsrArnoldiLoad(result, model.sqrt_total_capacitance, model.projection[terminal].data(),
                    thresholds, wire_delay, load_slew) == CsrRcEvaluationStatus::kSuccess, "CSR load failed");
        if (!load_cell) {
          near(wire_delay, load.get_timing_result().get_delay(), "wire delay");
          near(load_slew, load.get_timing_result().get_slew(), "load slew");
        }
        double cached_wire = 0.0, cached_load_slew = 0.0;
        require(evaluateCsrCpuRcLoad({binding.tree, terminal}, a, tr, evaluated_slew, load_cell,
                    runtime, cached_wire, cached_load_slew) == CsrRcEvaluationStatus::kSuccess,
                "runtime terminal evaluation failed");
        near(cached_wire, load.get_timing_result().get_delay(), "runtime wire delay");
        near(cached_load_slew, load.get_timing_result().get_slew(), "runtime load slew");
        const auto fallback = CsrParasiticNativeTestAccess::fallback("p" + std::to_string(terminal), analysis, transition);
        require(reduced.fallback[terminal].valid, "raw fallback unavailable");
        near(reduced.fallback[terminal].delay, fallback.first, "raw fallback delay");
        near(std::sqrt(1234.0 * 1234.0 + reduced.fallback[terminal].impulse), fallback.second, "raw fallback slew");
        compareNetArc({binding.tree, terminal}, a, tr, evaluated_slew, false, {}, runtime,
                      fallback.first, cached_wire, cached_load_slew, load_cell);
        double fallback_delay = 0.0, fallback_slew = 0.0;
        require(evaluateCsrCpuNetTiming({binding.tree, terminal}, a, tr, 1234.0, false, nullptr,
                    {}, &runtime, fallback_delay, fallback_slew) == CsrRcEvaluationStatus::kSuccess,
                "runtime raw-tree fallback failed");
        near(fallback_delay, fallback.first, "runtime raw-tree delay");
        near(fallback_slew, fallback.second, "runtime raw-tree slew");
      }
    }
  }
  // Switch only the driver role; the independent raw RC model is rebuilt.
  auto& db = STADM.getDatabase();
  db.get_pin_map().at("p0").set_is_port(true);
  auto& global = db.get_timing_library();
  for (bool degenerate : {false, true}) {
    const double lower = degenerate ? 1.0 : 35.0;
    global.set_slew_lower_threshold_pct_rise(lower);
    global.set_slew_lower_threshold_pct_fall(lower);
    global.set_slew_upper_threshold_pct_rise(85.0);
    global.set_slew_upper_threshold_pct_fall(85.0);
    global.set_slew_derate_from_library(0.6);
    STADC.resetForEditRollback();
    const auto port_thresholds = csrArnoldiThresholds(lower, 85.0, 0.6);
    for (std::size_t terminal = 0; terminal < tree.size(); ++terminal)
      for (double slew : {input_slew, -input_slew})
        for (auto transition : {TransType::kRise, TransType::kFall}) {
          const auto native_port = CsrParasiticNativeTestAccess::inputPort(
              "p" + std::to_string(terminal), AnalysisType::kMax, transition, slew);
          double wire = 0.0, propagated_slew = 0.0;
          require(evaluateCsrArnoldiInputPort(view, model.projection[terminal].data(),
                      port_thresholds, slew, wire, propagated_slew) == CsrRcEvaluationStatus::kSuccess,
                  "CSR input-port calculation failed");
          near(wire, native_port.first, "input-port wire delay");
          near(propagated_slew, native_port.second, "input-port global-threshold slew");
          const InputPortRcThresholds globals{0.6, {lower, lower}, {85.0, 85.0}};
          require(evaluateCsrCpuNetTiming({binding.tree, terminal}, AnalysisMode::kMax,
                      transition == TransType::kRise ? Transition::kRise : Transition::kFall,
                      slew, true, nullptr, globals, nullptr, wire, propagated_slew)
                      == CsrRcEvaluationStatus::kSuccess, "runtime input-port fallback failed");
          near(wire, native_port.first, "runtime port delay");
          near(propagated_slew, native_port.second, "runtime port slew");
          if (slew >= 0.0) {
            CsrCpuRcCache empty;
            compareNetArc({binding.tree, terminal}, AnalysisMode::kMax,
                          transition == TransType::kRise ? Transition::kRise : Transition::kFall,
                          slew, true, globals, empty, native_port.first, native_port.first, native_port.second);
          }
        }
  }
  db.get_pin_map().at("p0").set_is_port(false);
  auto& tested_arc = cell.get_timing_arc_list().front();
  tested_arc.get_delay_table_map().erase(TransType::kRise);
  tables.delay_table.present = false;
  STADC.resetForEditRollback();
  const auto& missing = CsrParasiticNativeTestAccess::dmp(
      tested_arc, AnalysisType::kMax, TransType::kRise, input_slew);
  CsrDmpDriverResult rejected;
  require(!missing.get_is_valid()
              && evaluateCsrDmpDriver(reduced.dmp.driver_capacitance, reduced.dmp.resistance,
                  reduced.dmp.load_capacitance, csrDmpThresholds(output_threshold, 10.0, 90.0, 0.8), tables, rejected)
                  != CsrRcEvaluationStatus::kSuccess,
          "missing DMP table accepted");
}
void compareRuntimeFallback()
{
  installTree({{0, 0.0, 0.15}}, 0.0);
  TimingArc native;
  native.set_cap_unit_scale(10.0);
  TimingTable delay, slew;
  delay.set_variable_type1(TimingTableVariableType::kOutputCapacitance);
  delay.set_axis_list({{2.0, 3.0}}); delay.set_value_list({1.0, 1.0});
  slew.set_variable_type1(TimingTableVariableType::kOutputCapacitance);
  slew.set_axis_list({{2.0, 3.0}}); slew.set_value_list({0.0, 1.0});
  native.get_delay_table_map()[TransType::kRise] = delay;
  native.get_slew_table_map()[TransType::kRise] = slew;
  require(!CsrParasiticNativeTestAccess::dmp(native, AnalysisType::kMax, TransType::kRise, 0.0).get_is_valid(),
          "native finite negative raw slew did not reject DMP");
  const auto by_load = CsrParasiticNativeTestAccess::byLoad(native, 0.0, 0.15);
  ChildTimingArcModel child;
  child.capacitance_unit_scale = 10.0;
  child.delay_tables[0] = {true, TableVariable::kOutputCapacitance, TableVariable::kNone, {{2.0, 3.0}}, {1.0, 1.0}};
  child.slew_tables[0] = {true, TableVariable::kOutputCapacitance, TableVariable::kNone, {{2.0, 3.0}}, {0.0, 1.0}};
  const std::vector<CsrRcTreeNode> tree{{0, 0.0, 0.15}};
  const std::vector<std::size_t> terminals{0};
  auto model = std::make_shared<CsrParasiticModel>(buildCsrParasiticModel(tree, terminals));
  // Force only the preceding Arnoldi family unavailable to exercise dispatch.
  // Native direct DMP/ByLoad above remains independent; this is not an
  // assertion that the native automatic selector rejects this Arnoldi model.
  model->arnoldi.order = 0;
  auto owner = std::make_shared<CsrParasiticTree>();
  owner->terminals = {NodeId{0}}; owner->lanes.fill(model);
  const CsrParasiticBinding binding{owner, 0};
  const CsrCpuRcChildKey key{NodeId{0}, false, 0, 0, AnalysisMode::kMax, Transition::kRise};
  CsrCpuRcCache cache;
  double d = 0.0, s = 0.0;
  require(evaluateCsrCpuCellTiming(binding, key, child, 0.0, -1.0, false, cache, d) == CsrRcEvaluationStatus::kSuccess
              && evaluateCsrCpuCellTiming(binding, key, child, 0.0, -1.0, true, cache, s) == CsrRcEvaluationStatus::kSuccess,
          "finite DMP unavailable blocked ordinary NLDM");
  near(d, by_load.first, "runtime ordinary fallback delay");
  near(s, by_load.second, "runtime ordinary fallback slew");
  require(cache.arnoldi_drivers.empty() && cache.dmp_drivers.empty(), "unavailable response published a driver");

  child.slew_tables[0].values = {1.0, 2.0};
  CsrCpuRcCache zeros;
  require(evaluateCsrCpuCellTiming(binding, key, child, 0.0, -1.0, false, zeros, d) == CsrRcEvaluationStatus::kSuccess
              && evaluateCsrCpuCellTiming(binding, key, child, -0.0, -1.0, false, zeros, d) == CsrRcEvaluationStatus::kSuccess
              && zeros.timing.size() == 2 && zeros.dmp_drivers.size() == 1, "DMP signed zero timing keys merged");

  CsrCpuRcCache invalid;
  child.slew_tables[0].values[0] = std::numeric_limits<double>::quiet_NaN();
  require(evaluateCsrCpuCellTiming(binding, key, child, 0.0, -1.0, false, invalid, d)
              == CsrRcEvaluationStatus::kNumericFailure && invalid.timing.empty(), "non-finite DMP failure published");

  const std::vector<CsrRcTreeNode> rounding{{0, 0.0, 1.0}, {0, 1.0, 1e-16}, {1, 1.0, 1e-16}};
  installTree(rounding, 0.0);
  const std::vector<std::size_t> rounding_terminals{0, 1, 2};
  const auto reduced = buildCsrParasiticModel(rounding, rounding_terminals);
  const double native_load = CsrParasiticNativeTestAccess::rootLoad();
  require(reduced.root_load == native_load && std::ceil(native_load * 1000.0) == 1001.0
              && std::ceil(reduced.arnoldi.total_capacitance * 1000.0) == 1000.0,
          "ordinary ceil lost native DFS accumulation order");
}

void compareLoadThresholds()
{
  auto& db = STADM.getDatabase();
  auto& pin = db.get_pin_map().at("p1");
  pin.set_is_port(false);
  pin.set_instance_name("threshold_sink");
  Instance instance;
  instance.set_instance_name("threshold_sink");
  instance.set_cell_name("LOAD");
  db.get_instance_map().emplace("threshold_sink", instance);
  TimingCell load;
  load.set_cell_name("LOAD"); load.set_library_name("load.lib");
  TimingArc driver;
  driver.set_library_name("driver.lib");
  driver.set_output_threshold_pct_rise(35.0); driver.set_output_threshold_pct_fall(35.0);
  driver.set_slew_lower_threshold_pct_rise(20.0); driver.set_slew_lower_threshold_pct_fall(20.0);
  driver.set_slew_upper_threshold_pct_rise(80.0); driver.set_slew_upper_threshold_pct_fall(80.0);
  driver.set_slew_derate(0.8);
  for (double derate : {0.6, 0.0, -1.0})
    for (double lower : {15.0, 85.0}) {
      load.set_input_threshold_pct_rise(45.0); load.set_input_threshold_pct_fall(45.0);
      load.set_slew_lower_threshold_pct_rise(lower); load.set_slew_lower_threshold_pct_fall(lower);
      load.set_slew_upper_threshold_pct_rise(85.0); load.set_slew_upper_threshold_pct_fall(85.0);
      load.set_slew_derate_from_library(derate);
      db.get_timing_library().get_cell_map().insert_or_assign("LOAD", load);
      for (auto transition : {TransType::kRise, TransType::kFall}) {
        double native_delay = 0.1, native_slew = 0.2;
        double delay = native_delay, slew = native_slew;
        CsrParasiticNativeTestAccess::adjust(driver, transition, native_delay, native_slew);
        require(adjustCsrParasiticLoadThreshold(
                    {20.0 * 0.01, 80.0 * 0.01, 35.0 * 0.01, 0.8},
                    {lower * 0.01, 85.0 * 0.01, 45.0 * 0.01, derate},
                    transition == TransType::kFall, delay, slew) == CsrRcEvaluationStatus::kSuccess,
                "load threshold adjustment failed");
        near(delay, native_delay, "cross-library threshold delay");
        near(slew, native_slew, "cross-library threshold slew");
      }
    }
}

}  // namespace

int main()
{
  try {
    NativeScope native;
    for (double slew : {0.22741790352985375, 0.22741790352985364})
      require(std::abs(CsrParasiticNativeTestAccess::tableRamp(slew) - 0.10218581909453474) < 1e-12,
              "native Arnoldi ramp is discontinuous at its roundoff boundary");
    const std::vector<std::vector<CsrRcTreeNode>> trees{
        {{0, 0.0, 0.04}},
        {{0, 0.0, 0.01}, {0, 2.0, 0.03}},
        {{0, 0.0, 1e-8}, {0, 2.0, 0.03}},
        {{0, 0.0, 0.02}, {0, 3.0, 0.01}, {1, 2.0, 0.03}, {0, 4.0, 0.05}},
        {{0, 0.0, 0.02}, {0, 1.0, 0.01}, {1, 2.0, 0.03}, {2, 3.0, 0.02},
         {3, 0.5, 0.01}, {4, 2.0, 0.04}, {5, 1.0, 0.02}, {6, 3.0, 0.03}}};
    for (const auto& original : trees)
      for (double scale : {0.1, 1.0, 10.0}) {
        auto tree = original;
        for (auto& node : tree) node.resistance_kohm *= scale;
        for (double slew_offset : {0.02, 0.2}) {
          compare(tree, slew_offset, 1.0, 0.1, 50.0);
          compare(tree, slew_offset, 2.0, 0.2, 40.0);
          compare(tree, slew_offset, 0.5, 0.01, 60.0);
        }
      }
    compare(trees[1], 0.02, 1.0, 0.1, 35.0, "driver.lib");
    compare(trees[1], 0.02, 1.0, 0.1, 35.0, "other.lib");
    compareLoadThresholds();
    compareRuntimeFallback();
    const std::vector<std::pair<std::vector<double>, std::vector<double>>> waveforms{
        {{1e-4, 10.0}, {0.4, 0.6}}, {{1e-5, 10.0}, {0.4, 0.6}},
        {{1e-4, 10.0}, {0.8, 0.2}}, {{1e-4, 10.0}, {0.95, 0.05}},
        {{0.01, 0.02, 10.0}, {1.0, -1.0, 1.0}}};
    for (const auto& [poles, residues] : waveforms) {
      const auto native_crossings = CsrParasiticNativeTestAccess::crossings(poles, residues);
      CsrArnoldiCrossings target;
      require(evaluateCsrArnoldiCrossings(1.0, static_cast<int>(poles.size()), poles.data(), residues.data(),
                  csrArnoldiThresholds(0.1, 0.9, 0.8), target) == CsrRcEvaluationStatus::kSuccess,
              "native waveform comparison failed");
      near(target.upper_time, native_crossings.upper_time, "upper waveform crossing");
      near(target.middle_time, native_crossings.middle_time, "middle waveform crossing");
      near(target.lower_time, native_crossings.lower_time, "lower waveform crossing");
    }
    std::cout << "Independent native Arnoldi/DMP comparison passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
