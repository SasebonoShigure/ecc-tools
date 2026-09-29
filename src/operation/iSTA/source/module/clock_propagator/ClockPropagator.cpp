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
// WHETHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "ClockPropagator.hpp"

#include <array>
#include <optional>

#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "Logger.hpp"
#include "Monitor.hpp"

namespace ista {

// public

void ClockPropagator::initInst()
{
  if (_cp_instance == nullptr) {
    _cp_instance = new ClockPropagator();
  }
}

ClockPropagator& ClockPropagator::getInst()
{
  if (_cp_instance == nullptr) {
    STALOG.error(Loc::current(), "The instance not initialized!");
  }
  return *_cp_instance;
}

void ClockPropagator::destroyInst()
{
  if (_cp_instance != nullptr) {
    delete _cp_instance;
    _cp_instance = nullptr;
  }
}

// function

void ClockPropagator::propagate()
{
  Monitor monitor;
  STALOG.info(Loc::current(), "Starting...");
  initTimingPointList();
  CPModel cp_model = initCPModel();
  buildClockSourceList(cp_model);
  markClockPointList(cp_model);
  propagateClockArrival(cp_model);
  STALOG.info(Loc::current(), "Completed", monitor.getStatsInfo());
}

// private

ClockPropagator* ClockPropagator::_cp_instance = nullptr;

bool ClockPropagator::isDisableArc(Arc& arc)
{
  return arc.get_is_disable_arc() || arc.get_is_loop_disable();
}

CPModel ClockPropagator::initCPModel()
{
  CPModel cp_model;
  return cp_model;
}

void ClockPropagator::buildClockSourceList(CPModel& cp_model)
{
  Database& database = STADM.getDatabase();
  for (auto& [name, clock] : database.get_timing_constraint().get_clock_map()) {
    cp_model.clock_list.emplace_back(name, clock.get_source_list(), clock.get_is_propagated());
  }
}

void ClockPropagator::initTimingPointList()
{
  Database& database = STADM.getDatabase();
  for (std::pair<const std::string, TimingPoint>& timing_pair : database.get_timing_point_map()) {
    timing_pair.second.set_arrival(-std::numeric_limits<double>::infinity());
    timing_pair.second.set_required(std::numeric_limits<double>::infinity());
    timing_pair.second.set_slack(0.0);
    timing_pair.second.set_launch_time(0.0);
    timing_pair.second.get_predecessor().clear();
    timing_pair.second.get_clock_name().clear();
    timing_pair.second.get_clock_arrival_map().clear();
    timing_pair.second.get_clock_slew_map().clear();
    timing_pair.second.get_physical_clock_arrival_map().clear();
    timing_pair.second.get_physical_clock_slew_map().clear();
    timing_pair.second.get_path_state_map().clear();
    timing_pair.second.get_data_slew_map().clear();
    timing_pair.second.get_clock_predecessor_map().clear();
    timing_pair.second.get_clock_predecessor_arc_delay_map().clear();
    timing_pair.second.get_clock_predecessor_trans_type_map().clear();
    timing_pair.second.get_physical_clock_predecessor_map().clear();
    timing_pair.second.get_physical_clock_predecessor_arc_delay_map().clear();
    timing_pair.second.get_physical_clock_predecessor_trans_type_map().clear();
    timing_pair.second.set_predecessor_arc_idx(std::numeric_limits<std::size_t>::max());
    timing_pair.second.set_is_clock_point(false);
  }
}

void ClockPropagator::markClockPointList(CPModel& cp_model)
{
  for (auto& clock : cp_model.clock_list) {
    markClockPoint(clock);
  }
}

void ClockPropagator::markClockPoint(CPClock& clock)
{
  Database& database = STADM.getDatabase();
  std::queue<std::string> pin_queue;
  for (const auto& clock_source : clock.get_source_list()) {
    if (!database.get_timing_point_map().contains(clock_source)) {
      STALOG.warn(Loc::current(), "clock '", clock.get_clock_name(), "' has no source");
      continue;
    }
    pin_queue.push(clock_source);
  }

  while (!pin_queue.empty()) {
    std::string pin_name = pin_queue.front();
    pin_queue.pop();

    TimingPoint& timing_point = database.get_timing_point_map()[pin_name];
    if (is_clock_tree_overlap(timing_point, clock)) {
      STALOG.error(Loc::current(), "clock trees overlap at pin '", pin_name, "': '", timing_point.get_clock_name(), "' and '", clock.get_clock_name(), "'");
    }
    if (timing_point.get_clock_name() == clock.get_clock_name()) {
      continue;
    }

    timing_point.set_is_clock_point(true);
    timing_point.set_clock_name(clock.get_clock_name());
    clock.add_clock_pin(pin_name);

    if (shouldStopClockPropagation(pin_name)) {
      continue;
    }
    for (const auto& arc_idx : database.get_outgoing_arc_list_map()[pin_name]) {
      Arc& arc = database.get_arc_list()[arc_idx];
      if (isDisableArc(arc)) {
        continue;
      }
      pin_queue.push(arc.get_sink_pin());
    }
  }
}

void ClockPropagator::propagateClockArrival(CPModel& cp_model)
{
  for (CPClock& clock : cp_model.clock_list) {
    seedPhysicalClockState(clock);
    propagatePhysicalClock(clock, true);
    propagatePhysicalClock(clock, false);
    updateEffectiveClockState(clock);
  }
}

void ClockPropagator::seedPhysicalClockState(CPClock& clock)
{
  Database& database = STADM.getDatabase();
  for (const auto& pin_name : clock.get_source_list()) {
    if (!database.get_timing_point_map().contains(pin_name)) {
      continue;
    }
    TimingPoint& timing_point = database.get_timing_point_map()[pin_name];
    timing_point.get_physical_clock_arrival_map()[AnalysisType::kMax][TransType::kRise] = 0.0;
    timing_point.get_physical_clock_arrival_map()[AnalysisType::kMax][TransType::kFall] = 0.0;
    timing_point.get_physical_clock_arrival_map()[AnalysisType::kMin][TransType::kRise] = 0.0;
    timing_point.get_physical_clock_arrival_map()[AnalysisType::kMin][TransType::kFall] = 0.0;
    timing_point.get_physical_clock_slew_map()[AnalysisType::kMax][TransType::kRise] = 0.0;
    timing_point.get_physical_clock_slew_map()[AnalysisType::kMax][TransType::kFall] = 0.0;
    timing_point.get_physical_clock_slew_map()[AnalysisType::kMin][TransType::kRise] = 0.0;
    timing_point.get_physical_clock_slew_map()[AnalysisType::kMin][TransType::kFall] = 0.0;
  }
}

void ClockPropagator::updateEffectiveClockState(CPClock& clock)
{
  Database& database = STADM.getDatabase();
  for (const auto& pin_name : clock.get_clock_point_list()) {
    TimingPoint& timing_point = database.get_timing_point_map()[pin_name];
    if (clock.get_is_propagated()) {
      timing_point.set_clock_arrival_map(timing_point.get_physical_clock_arrival_map());
      timing_point.set_clock_slew_map(timing_point.get_physical_clock_slew_map());
      timing_point.set_clock_predecessor_map(timing_point.get_physical_clock_predecessor_map());
      timing_point.set_clock_predecessor_arc_delay_map(timing_point.get_physical_clock_predecessor_arc_delay_map());
      timing_point.set_clock_predecessor_trans_type_map(timing_point.get_physical_clock_predecessor_trans_type_map());
      continue;
    }

    for (AnalysisType analysis_type : {AnalysisType::kMax, AnalysisType::kMin}) {
      for (TransType trans_type : {TransType::kRise, TransType::kFall}) {
        timing_point.get_clock_arrival_map()[analysis_type][trans_type] = 0.0;
        timing_point.get_clock_slew_map()[analysis_type][trans_type] = 0.0;
      }
    }
  }
}

void ClockPropagator::propagatePhysicalClock(CPClock& clock, bool slew_pass)
{
  Database& database = STADM.getDatabase();
  struct Candidate {
    AnalysisType analysis;
    TransType output;
    double value;
    Arc* arc;
    double delay;
    TransType input;
  };
  std::vector<Candidate> candidates;
  const auto lane = [](AnalysisType analysis, TransType transition) {
    return (analysis == AnalysisType::kMax ? 0U : 2U) + (transition == TransType::kRise ? 0U : 1U);
  };
  for (std::string& name : database.get_timing_order_list()) {
    auto& sink = database.get_timing_point_map().at(name);
    if (!sink.get_is_clock_point() || sink.get_clock_name() != clock.get_clock_name())
      continue;
    candidates.clear();
    std::array<std::optional<double>, 4> extreme;
    const auto add = [&](Candidate candidate) {
      candidates.push_back(candidate);
      auto& best = extreme[lane(candidate.analysis, candidate.output)];
      if (!best || (candidate.analysis == AnalysisType::kMax ? candidate.value > *best : candidate.value < *best))
        best = candidate.value;
    };
    auto& values = slew_pass ? sink.get_physical_clock_slew_map() : sink.get_physical_clock_arrival_map();
    for (auto analysis : {AnalysisType::kMax, AnalysisType::kMin})
      for (auto output : {TransType::kRise, TransType::kFall})
        if (values.contains(analysis) && values.at(analysis).contains(output))
          add({analysis, output, values.at(analysis).at(output), nullptr, 0.0, output});
    for (auto index : database.get_incoming_arc_list_map()[name]) {
      auto& arc = database.get_arc_list()[index];
      if (isDisableArc(arc) || shouldStopClockPropagation(arc.get_source_pin()))
        continue;
      auto& source = database.get_timing_point_map().at(arc.get_source_pin());
      if (!source.get_is_clock_point() || source.get_clock_name() != clock.get_clock_name())
        continue;
      for (auto analysis : {AnalysisType::kMax, AnalysisType::kMin})
        for (auto input : {TransType::kRise, TransType::kFall}) {
          auto& slews = source.get_physical_clock_slew_map();
          if (!slews.contains(analysis) || !slews.at(analysis).contains(input)
              || (!slew_pass && !hasClockArrival(source, analysis, input)))
            continue;
          for (auto output : getOutputTransTypeList(arc, analysis, input)) {
            if (slew_pass) {
              DCTask task;
              task.set_proc_type(DCProcType::kCalculate);
              task.set_arc(&arc);
              task.set_analysis_type(analysis);
              task.set_input_trans_type(input);
              task.set_output_trans_type(output);
              task.set_input_slew(slews.at(analysis).at(input));
              STADC.calculate(task);
              if (!task.get_is_valid())
                continue;
              auto& result = task.get_timing_result();
              updateGraphArcDelay(arc, analysis, input, output, result.get_delay());
              add({analysis, output, result.get_slew(), &arc, result.get_delay(), input});
            } else {
              const auto delay = getArcDelay(arc, analysis, input, output);
              add({analysis, output, roundTime(getClockArrival(source, analysis, input) + delay), &arc, delay, input});
            }
          }
        }
    }
    std::array<bool, 4> selected{};
    for (const auto& candidate : candidates) {
      const auto index = lane(candidate.analysis, candidate.output);
      if (selected[index] || isBetterDelay(*extreme[index], candidate.value, candidate.analysis))
        continue;
      selected[index] = true;
      values[candidate.analysis][candidate.output] = candidate.value;
      if (!slew_pass && candidate.arc != nullptr)
        updateClockPredecessor(sink, candidate.analysis, candidate.output, candidate.input, *candidate.arc, candidate.delay);
    }
  }
}

bool ClockPropagator::hasClockArrival(TimingPoint& timing_point, AnalysisType analysis_type, TransType trans_type)
{
  return timing_point.get_physical_clock_arrival_map().count(analysis_type) > 0
         && timing_point.get_physical_clock_arrival_map()[analysis_type].count(trans_type) > 0;
}

double ClockPropagator::getClockArrival(TimingPoint& timing_point, AnalysisType analysis_type, TransType trans_type)
{
  if (!hasClockArrival(timing_point, analysis_type, trans_type)) {
    return 0.0;
  }
  return timing_point.get_physical_clock_arrival_map()[analysis_type][trans_type];
}

void ClockPropagator::updateClockPredecessor(TimingPoint& timing_point, AnalysisType analysis_type, TransType trans_type, TransType predecessor_trans_type,
                                             Arc& arc, double arc_delay)
{
  timing_point.get_physical_clock_predecessor_map()[analysis_type][trans_type] = arc.get_source_pin();
  timing_point.get_physical_clock_predecessor_arc_delay_map()[analysis_type][trans_type] = arc_delay;
  timing_point.get_physical_clock_predecessor_trans_type_map()[analysis_type][trans_type] = predecessor_trans_type;
}

bool ClockPropagator::shouldStopClockPropagation(std::string& pin_name)
{
  Database& database = STADM.getDatabase();
  Pin& pin = database.get_pin_map()[pin_name];
  if (pin.get_is_port() || !database.get_instance_map().contains(pin.get_instance_name())) {
    return false;
  }
  Instance& instance = database.get_instance_map()[pin.get_instance_name()];
  return instance.get_is_sequential() && !instance.get_is_clock_gating() && pin_name == instance.get_clock_pin_name();
}

void ClockPropagator::updateGraphArcDelay(Arc& arc, AnalysisType analysis_type, TransType input_trans_type, TransType output_trans_type, double arc_delay)
{
  if (arc.get_graph_delay_map().count(analysis_type) == 0 || arc.get_graph_delay_map()[analysis_type].count(input_trans_type) == 0
      || arc.get_graph_delay_map()[analysis_type][input_trans_type].count(output_trans_type) == 0
      || isBetterDelay(arc_delay, arc.get_graph_delay_map()[analysis_type][input_trans_type][output_trans_type], analysis_type)) {
    arc.get_graph_delay_map()[analysis_type][input_trans_type][output_trans_type] = arc_delay;
  }
}

bool ClockPropagator::isBetterDelay(double candidate_delay, double current_delay, AnalysisType analysis_type)
{
  if (analysis_type == AnalysisType::kMin) {
    return candidate_delay < current_delay - STA_ERROR;
  }
  return candidate_delay > current_delay + STA_ERROR;
}

double ClockPropagator::roundTime(double time)
{
  return std::round(time * 1E15) / 1E15;
}
std::vector<TransType> ClockPropagator::getOutputTransTypeList(Arc& arc, AnalysisType analysis_type, TransType input_trans_type)
{
  std::vector<TransType> output_trans_type_list;
  if (arc.get_input_output_delay_map().count(analysis_type) == 0 || arc.get_input_output_delay_map()[analysis_type].count(input_trans_type) == 0) {
    return output_trans_type_list;
  }
  for (std::pair<const TransType, double>& delay_pair : arc.get_input_output_delay_map()[analysis_type][input_trans_type]) {
    output_trans_type_list.push_back(delay_pair.first);
  }
  return output_trans_type_list;
}

double ClockPropagator::getArcDelay(Arc& arc, AnalysisType analysis_type, TransType input_trans_type)
{
  if (arc.get_trans_delay_map().count(analysis_type) > 0 && arc.get_trans_delay_map()[analysis_type].count(input_trans_type) > 0) {
    return arc.get_trans_delay_map()[analysis_type][input_trans_type];
  }
  if (analysis_type == AnalysisType::kMin) {
    return arc.get_delay_min();
  }
  return arc.get_delay_max();
}

double ClockPropagator::getArcDelay(Arc& arc, AnalysisType analysis_type, TransType input_trans_type, TransType output_trans_type)
{
  if (arc.get_graph_delay_map().count(analysis_type) > 0 && arc.get_graph_delay_map()[analysis_type].count(input_trans_type) > 0
      && arc.get_graph_delay_map()[analysis_type][input_trans_type].count(output_trans_type) > 0) {
    return arc.get_graph_delay_map()[analysis_type][input_trans_type][output_trans_type];
  }
  if (arc.get_input_output_delay_map().count(analysis_type) > 0 && arc.get_input_output_delay_map()[analysis_type].count(input_trans_type) > 0
      && arc.get_input_output_delay_map()[analysis_type][input_trans_type].count(output_trans_type) > 0) {
    return arc.get_input_output_delay_map()[analysis_type][input_trans_type][output_trans_type];
  }
  return getArcDelay(arc, analysis_type, input_trans_type);
}

}  // namespace ista
