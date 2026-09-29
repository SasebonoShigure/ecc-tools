// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingPropagator.hpp"

#include <array>
#include <optional>

#include "DataManager.hpp"
#include "DelayCalculator.hpp"

namespace ista {

void TimingPropagator::propagateDataSlewDelay(TPModel& tp_model)
{
  Database& database = STADM.getDatabase();
  seedDataSlewList(tp_model);
  struct Candidate
  {
    AnalysisType analysis;
    TransType transition;
    double slew;
  };
  std::vector<Candidate> candidates;
  const auto lane = [](AnalysisType analysis, TransType transition) {
    return (analysis == AnalysisType::kMax ? 0U : 2U) + (transition == TransType::kRise ? 0U : 1U);
  };
  for (std::string& pin_name : database.get_timing_order_list()) {
    auto& sink = database.get_timing_point_map().at(pin_name);
    candidates.clear();
    std::array<std::optional<double>, 4> extreme;
    const auto add = [&](AnalysisType analysis, TransType transition, double value) {
      candidates.push_back({analysis, transition, value});
      auto& best = extreme[lane(analysis, transition)];
      if (!best || (analysis == AnalysisType::kMax ? value > *best : value < *best))
        best = value;
    };
    // Seed identity precedes incoming row ordinal + input transition.
    for (auto analysis : {AnalysisType::kMax, AnalysisType::kMin})
      for (auto output : {TransType::kRise, TransType::kFall})
        if (hasDataSlew(sink, analysis, output))
          add(analysis, output, getDataSlew(sink, analysis, output));
    for (auto arc_idx : database.get_incoming_arc_list_map()[pin_name]) {
      auto& arc = database.get_arc_list()[arc_idx];
      if (isDisableArc(arc) || shouldStopDataSlewPropagation(arc))
        continue;
      auto& source = database.get_timing_point_map().at(arc.get_source_pin());
      for (auto analysis : {AnalysisType::kMax, AnalysisType::kMin})
        for (auto input : {TransType::kRise, TransType::kFall}) {
          if (!hasDataSlew(source, analysis, input))
            continue;
          for (auto output : getOutputTransTypeList(arc, analysis, input))
            if (const auto value = calculateDataSlewDelay(arc, source, analysis, input, output))
              add(analysis, output, *value);
        }
    }
    std::array<bool, 4> selected{};
    for (const auto& candidate : candidates) {
      const auto index = lane(candidate.analysis, candidate.transition);
      if (!selected[index] && !isBetterDelay(*extreme[index], candidate.slew, candidate.analysis)) {
        sink.get_data_slew_map()[candidate.analysis][candidate.transition] = candidate.slew;
        selected[index] = true;
      }
    }
  }
}

void TimingPropagator::seedDataSlewList(TPModel& tp_model)
{
  for (std::string& start_point : tp_model.get_start_point_list()) {
    seedDataSlew(start_point, AnalysisType::kMax);
    seedDataSlew(start_point, AnalysisType::kMin);
  }
}

void TimingPropagator::seedDataSlew(std::string& start_point, AnalysisType analysis_type)
{
  seedDataSlew(start_point, analysis_type, TransType::kRise);
  seedDataSlew(start_point, analysis_type, TransType::kFall);
}

void TimingPropagator::seedDataSlew(std::string& start_point, AnalysisType analysis_type, TransType trans_type)
{
  Database& database = STADM.getDatabase();
  TimingPoint& timing_point = database.get_timing_point_map()[start_point];
  if (isSequentialClockPin(start_point)
      && hasIncomingPhysicalSlewArc(start_point)
      && (timing_point.get_clock_slew_map().count(analysis_type) == 0
          || timing_point.get_clock_slew_map()[analysis_type].count(trans_type) == 0)) {
    // A propagated divider or gated-clock path will provide the physical slew
    // through this net arc. Seeding zero here would win min-slew selection.
    return;
  }
  timing_point.get_data_slew_map()[analysis_type][trans_type] = getStartPointSlew(start_point, analysis_type, trans_type);
}

std::optional<double> TimingPropagator::calculateDataSlewDelay(Arc& arc, TimingPoint& source_point, AnalysisType analysis_type,
                                           TransType input_trans_type, TransType output_trans_type)
{
  double input_slew = getDataSlew(source_point, analysis_type, input_trans_type);
  DCTask dc_task;
  dc_task.set_proc_type(DCProcType::kCalculate);
  dc_task.set_arc(&arc);
  dc_task.set_analysis_type(analysis_type);
  dc_task.set_input_trans_type(input_trans_type);
  dc_task.set_output_trans_type(output_trans_type);
  dc_task.set_input_slew(input_slew);
  if (isSequentialClockPin(arc.get_source_pin())) {
    // PT uses the ordinary propagated slew for C2Q delay, but uses the clock
    // slew for its output-transition lookup.  A CK reached through data logic
    // has no clock slew, so the latter intentionally falls back to zero.
    dc_task.set_output_slew_input_slew(getClockSlew(arc.get_source_pin(), analysis_type, input_trans_type));
  }
  STADC.calculate(dc_task);
  if (!dc_task.get_is_valid()) {
    return std::nullopt;
  }
  double arc_delay = dc_task.get_timing_result().get_delay();
  double output_slew = dc_task.get_timing_result().get_slew();
  updateGraphArcDelay(arc, analysis_type, input_trans_type, output_trans_type, arc_delay);
  return output_slew;
}

void TimingPropagator::updateGraphArcDelay(Arc& arc, AnalysisType analysis_type, TransType input_trans_type, TransType output_trans_type, double arc_delay)
{
  if (arc.get_graph_delay_map().count(analysis_type) == 0 || arc.get_graph_delay_map()[analysis_type].count(input_trans_type) == 0
      || arc.get_graph_delay_map()[analysis_type][input_trans_type].count(output_trans_type) == 0
      || isBetterDelay(arc_delay, arc.get_graph_delay_map()[analysis_type][input_trans_type][output_trans_type], analysis_type)) {
    arc.get_graph_delay_map()[analysis_type][input_trans_type][output_trans_type] = arc_delay;
  }
}

bool TimingPropagator::hasDataSlew(TimingPoint& timing_point, AnalysisType analysis_type, TransType trans_type)
{
  return timing_point.get_data_slew_map().count(analysis_type) > 0 && timing_point.get_data_slew_map()[analysis_type].count(trans_type) > 0;
}

double TimingPropagator::getDataSlew(TimingPoint& timing_point, AnalysisType analysis_type, TransType trans_type)
{
  if (!hasDataSlew(timing_point, analysis_type, trans_type)) {
    return 0.0;
  }
  return timing_point.get_data_slew_map()[analysis_type][trans_type];
}

bool TimingPropagator::isBetterDelay(double candidate_delay, double current_delay, AnalysisType analysis_type)
{
  if (analysis_type == AnalysisType::kMin) {
    return candidate_delay < current_delay - STA_ERROR;
  }
  return candidate_delay > current_delay + STA_ERROR;
}

}  // namespace ista
