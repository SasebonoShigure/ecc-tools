// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingAnalyzer.hpp"
#include "DataManager.hpp"
#include <algorithm>

namespace ista {
void TimingAnalyzer::buildPathTrace(std::string& end_point, AnalysisType analysis_type, PathSourceType source_type, TransType trans_type,
                                      std::string& start_point, std::vector<std::string>& path_pin_name_list, std::vector<TransType>& path_trans_type_list,
                                      std::vector<std::size_t>& path_arc_idx_list)
{
  const std::string clock_tag{getClockName(start_point)};
  std::string pin_name = end_point;
  TransType current_trans_type = trans_type;
  while (!pin_name.empty()) {
    path_pin_name_list.push_back(pin_name);
    path_trans_type_list.push_back(current_trans_type);
    TimingPathState& path_state = getPathStateMap(timingPoint(pin_name), analysis_type, source_type, current_trans_type)[clock_tag];
    pin_name = path_state.get_predecessor();
    if (!pin_name.empty())
      path_arc_idx_list.push_back(path_state.get_predecessor_arc_idx());
    current_trans_type = path_state.get_predecessor_trans_type();
  }
  std::reverse(path_pin_name_list.begin(), path_pin_name_list.end());
  std::reverse(path_trans_type_list.begin(), path_trans_type_list.end());
  std::reverse(path_arc_idx_list.begin(), path_arc_idx_list.end());
}

void TimingAnalyzer::updatePathDelay(TimingPath& timing_path, Arc* arc, double arc_delay)
{
  if (arc == nullptr) {
    return;
  }
  if (arc->get_type() == ArcType::kCell) {
    timing_path.set_cell_delay(timing_path.get_cell_delay() + arc_delay);
  } else if (arc->get_type() == ArcType::kNet) {
    timing_path.set_net_delay(timing_path.get_net_delay() + arc_delay);
  }
}

TimingPathPoint TimingAnalyzer::makeTimingPathPoint(std::string& pin_name, Arc* arc, TimingPoint& timing_point,
                                                      TimingPathState& path_state, TransType trans_type)
{
  Database& database = STADM.getDatabase();
  Pin& pin = database.get_pin_map()[pin_name];
  TimingPathPoint path_point;
  path_point.set_pin_name(pin_name);
  path_point.set_instance_name(pin.get_instance_name());
  if (!pin.get_instance_name().empty()) {
    path_point.set_cell_name(database.get_instance_map()[pin.get_instance_name()].get_cell_name());
  }
  path_point.set_net_name(pin.get_net_name());
  path_point.set_arrival(path_state.get_arrival());
  path_point.set_required(timing_point.get_required());
  path_point.set_slack(timing_point.get_slack());
  path_point.set_trans_type(trans_type);
  if (arc != nullptr) {
    path_point.set_arc_name(arc->get_arc_name());
    path_point.set_source_pin(arc->get_source_pin());
    path_point.set_sink_pin(arc->get_sink_pin());
    path_point.set_arc_type(arc->get_type());
    path_point.set_arc_delay(path_state.get_predecessor_arc_delay());
  }
  return path_point;
}

}  // namespace ista
