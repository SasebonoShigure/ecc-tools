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
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "AnalysisType.hpp"
#include "PathSourceType.hpp"
#include "STAHeader.hpp"
#include "TimingPathState.hpp"
#include "TransType.hpp"

namespace ista {

class TimingPoint
{
 public:
  TimingPoint() = default;
  ~TimingPoint() = default;
  // getter
  [[nodiscard]] double get_arrival() const noexcept { return _arrival; }
  [[nodiscard]] double get_required() const noexcept { return _required; }
  [[nodiscard]] double get_slack() const noexcept { return _slack; }
  [[nodiscard]] double get_launch_time() const noexcept { return _launch_time; }
  [[nodiscard]] int32_t get_level() const noexcept { return _level; }
  [[nodiscard]] std::string& get_predecessor() noexcept { return _predecessor; }
  [[nodiscard]] const std::string& get_predecessor() const noexcept { return _predecessor; }
  [[nodiscard]] std::string& get_clock_name() noexcept { return _clock_name; }
  [[nodiscard]] std::string_view get_clock_name() const noexcept { return _clock_name; }
  [[nodiscard]] std::size_t get_predecessor_arc_idx() const noexcept { return _predecessor_arc_idx; }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, double>>& get_clock_arrival_map() noexcept { return _clock_arrival_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, double>>& get_clock_arrival_map() const noexcept { return _clock_arrival_map; }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, double>>& get_clock_slew_map() noexcept { return _clock_slew_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, double>>& get_clock_slew_map() const noexcept { return _clock_slew_map; }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, double>>& get_physical_clock_arrival_map() noexcept { return _physical_clock_arrival_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, double>>& get_physical_clock_arrival_map() const noexcept
  {
    return _physical_clock_arrival_map;
  }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, double>>& get_physical_clock_slew_map() noexcept { return _physical_clock_slew_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, double>>& get_physical_clock_slew_map() const noexcept { return _physical_clock_slew_map; }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, std::string>>& get_clock_predecessor_map() noexcept { return _clock_predecessor_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, std::string>>& get_clock_predecessor_map() const noexcept { return _clock_predecessor_map; }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, double>>& get_clock_predecessor_arc_delay_map() noexcept { return _clock_predecessor_arc_delay_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, double>>& get_clock_predecessor_arc_delay_map() const noexcept
  {
    return _clock_predecessor_arc_delay_map;
  }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, TransType>>& get_clock_predecessor_trans_type_map() noexcept
  {
    return _clock_predecessor_trans_type_map;
  }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, TransType>>& get_clock_predecessor_trans_type_map() const noexcept
  {
    return _clock_predecessor_trans_type_map;
  }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, std::string>>& get_physical_clock_predecessor_map() noexcept
  {
    return _physical_clock_predecessor_map;
  }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, std::string>>& get_physical_clock_predecessor_map() const noexcept
  {
    return _physical_clock_predecessor_map;
  }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, double>>& get_physical_clock_predecessor_arc_delay_map() noexcept
  {
    return _physical_clock_predecessor_arc_delay_map;
  }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, double>>& get_physical_clock_predecessor_arc_delay_map() const noexcept
  {
    return _physical_clock_predecessor_arc_delay_map;
  }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, TransType>>& get_physical_clock_predecessor_trans_type_map() noexcept
  {
    return _physical_clock_predecessor_trans_type_map;
  }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, TransType>>& get_physical_clock_predecessor_trans_type_map() const noexcept
  {
    return _physical_clock_predecessor_trans_type_map;
  }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, double>>& get_data_slew_map() noexcept { return _data_slew_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, double>>& get_data_slew_map() const noexcept { return _data_slew_map; }
  // The innermost key is the launch clock name, not the individual startpoint.
  [[nodiscard]] std::map<AnalysisType, std::map<PathSourceType, std::map<TransType, std::map<std::string, TimingPathState>>>>& get_path_state_map() noexcept
  {
    return _path_state_map;
  }
  [[nodiscard]] const std::map<AnalysisType, std::map<PathSourceType, std::map<TransType, std::map<std::string, TimingPathState>>>>& get_path_state_map()
      const noexcept
  {
    return _path_state_map;
  }
  [[nodiscard]] bool get_is_clock_point() const noexcept { return _is_clock_point; }
  // setter
  void set_arrival(const double arrival) noexcept { _arrival = arrival; }
  void set_required(const double required) noexcept { _required = required; }
  void set_slack(const double slack) noexcept { _slack = slack; }
  void set_launch_time(const double launch_time) noexcept { _launch_time = launch_time; }
  void set_level(const int32_t level) noexcept { _level = level; }
  void set_predecessor(std::string_view predecessor) { _predecessor = predecessor; }
  void set_clock_name(std::string_view clock_name) { _clock_name = clock_name; }
  void set_predecessor_arc_idx(const std::size_t predecessor_arc_idx) noexcept { _predecessor_arc_idx = predecessor_arc_idx; }
  void set_clock_arrival_map(const std::map<AnalysisType, std::map<TransType, double>>& clock_arrival_map) { _clock_arrival_map = clock_arrival_map; }
  void set_clock_slew_map(const std::map<AnalysisType, std::map<TransType, double>>& clock_slew_map) { _clock_slew_map = clock_slew_map; }
  void set_physical_clock_arrival_map(const std::map<AnalysisType, std::map<TransType, double>>& clock_arrival_map)
  {
    _physical_clock_arrival_map = clock_arrival_map;
  }
  void set_physical_clock_slew_map(const std::map<AnalysisType, std::map<TransType, double>>& clock_slew_map) { _physical_clock_slew_map = clock_slew_map; }
  void set_clock_predecessor_map(const std::map<AnalysisType, std::map<TransType, std::string>>& clock_predecessor_map)
  {
    _clock_predecessor_map = clock_predecessor_map;
  }
  void set_clock_predecessor_arc_delay_map(const std::map<AnalysisType, std::map<TransType, double>>& clock_predecessor_arc_delay_map)
  {
    _clock_predecessor_arc_delay_map = clock_predecessor_arc_delay_map;
  }
  void set_clock_predecessor_trans_type_map(const std::map<AnalysisType, std::map<TransType, TransType>>& clock_predecessor_trans_type_map)
  {
    _clock_predecessor_trans_type_map = clock_predecessor_trans_type_map;
  }
  void set_physical_clock_predecessor_map(const std::map<AnalysisType, std::map<TransType, std::string>>& clock_predecessor_map)
  {
    _physical_clock_predecessor_map = clock_predecessor_map;
  }
  void set_physical_clock_predecessor_arc_delay_map(const std::map<AnalysisType, std::map<TransType, double>>& clock_predecessor_arc_delay_map)
  {
    _physical_clock_predecessor_arc_delay_map = clock_predecessor_arc_delay_map;
  }
  void set_physical_clock_predecessor_trans_type_map(const std::map<AnalysisType, std::map<TransType, TransType>>& clock_predecessor_trans_type_map)
  {
    _physical_clock_predecessor_trans_type_map = clock_predecessor_trans_type_map;
  }
  void set_data_slew_map(const std::map<AnalysisType, std::map<TransType, double>>& data_slew_map) { _data_slew_map = data_slew_map; }
  void set_path_state_map(const std::map<AnalysisType, std::map<PathSourceType, std::map<TransType, std::map<std::string, TimingPathState>>>>& path_state_map)
  {
    _path_state_map = path_state_map;
  }
  void set_is_clock_point(const bool is_clock_point) noexcept { _is_clock_point = is_clock_point; }
  // function

 private:
  double _arrival = -std::numeric_limits<double>::infinity();
  double _required = std::numeric_limits<double>::infinity();
  double _slack = 0.0;
  double _launch_time = 0.0;
  int32_t _level = 0;
  std::string _predecessor;
  std::string _clock_name;
  std::size_t _predecessor_arc_idx = std::numeric_limits<std::size_t>::max();
  // Effective state drives timing analysis; physical state retains the full clock-tree calculation.
  std::map<AnalysisType, std::map<TransType, double>> _clock_arrival_map;
  std::map<AnalysisType, std::map<TransType, double>> _clock_slew_map;
  std::map<AnalysisType, std::map<TransType, double>> _physical_clock_arrival_map;
  std::map<AnalysisType, std::map<TransType, double>> _physical_clock_slew_map;
  std::map<AnalysisType, std::map<TransType, std::string>> _clock_predecessor_map;
  std::map<AnalysisType, std::map<TransType, double>> _clock_predecessor_arc_delay_map;
  std::map<AnalysisType, std::map<TransType, TransType>> _clock_predecessor_trans_type_map;
  std::map<AnalysisType, std::map<TransType, std::string>> _physical_clock_predecessor_map;
  std::map<AnalysisType, std::map<TransType, double>> _physical_clock_predecessor_arc_delay_map;
  std::map<AnalysisType, std::map<TransType, TransType>> _physical_clock_predecessor_trans_type_map;
  std::map<AnalysisType, std::map<TransType, double>> _data_slew_map;
  std::map<AnalysisType, std::map<PathSourceType, std::map<TransType, std::map<std::string, TimingPathState>>>> _path_state_map;
  bool _is_clock_point = false;
};

}  // namespace ista
