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

#include "AnalysisType.hpp"
#include "ArcType.hpp"
#include "STAHeader.hpp"
#include "TransType.hpp"

namespace ista {
namespace csr::detail {
struct PreparedTimingResult;
void publishPreparedTimingResult(PreparedTimingResult&) noexcept;
}  // namespace csr::detail

class TimingCellArc;

using TimingArcDelayMap = std::map<std::int32_t, std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>>>;
using TimingArcDelayFallbackKey = std::tuple<std::int32_t, AnalysisType, TransType, TransType>;
using TimingArcDelayFallbackKeySet = std::set<TimingArcDelayFallbackKey>;

struct TimingArcDelayState
{
  TimingArcDelayMap delay_map;
  TimingArcDelayFallbackKeySet fallback_key_set;

  void swap(TimingArcDelayState& other) noexcept
  {
    delay_map.swap(other.delay_map);
    fallback_key_set.swap(other.fallback_key_set);
  }

  friend void swap(TimingArcDelayState& left, TimingArcDelayState& right) noexcept { left.swap(right); }
  friend bool operator==(const TimingArcDelayState&, const TimingArcDelayState&) = default;
};

class Arc
{
 public:
  Arc() = default;
  ~Arc() = default;
  // getter
  std::string& get_arc_name() { return _arc_name; }
  std::string& get_source_pin() { return _source_pin; }
  const std::string& get_source_pin() const { return _source_pin; }
  std::string& get_sink_pin() { return _sink_pin; }
  const std::string& get_sink_pin() const { return _sink_pin; }
  std::string& get_owner_name() { return _owner_name; }
  const std::string& get_owner_name() const { return _owner_name; }
  std::string& get_library_source_port() { return _library_source_port; }
  const std::string& get_library_source_port() const { return _library_source_port; }
  std::string& get_library_sink_port() { return _library_sink_port; }
  const std::string& get_library_sink_port() const { return _library_sink_port; }
  ArcType get_type() const { return _type; }
  double get_delay() const noexcept { return _delay; }
  double get_delay_max() const noexcept { return _delay_max; }
  double get_delay_min() const noexcept { return _delay_min; }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, double>>& get_trans_delay_map() noexcept { return _trans_delay_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, double>>& get_trans_delay_map() const noexcept { return _trans_delay_map; }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>>& get_input_output_delay_map() noexcept
  {
    return _input_output_delay_map;
  }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>>& get_input_output_delay_map() const noexcept
  {
    return _input_output_delay_map;
  }
  [[nodiscard]] std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>>& get_graph_delay_map() noexcept { return _graph_delay_map; }
  [[nodiscard]] const std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>>& get_graph_delay_map() const noexcept
  {
    return _graph_delay_map;
  }
  [[nodiscard]] const TimingArcDelayState& get_timing_arc_delay_state() const noexcept { return _timing_arc_delay_state; }
  [[nodiscard]] TimingArcDelayMap& get_timing_arc_delay_map() noexcept
  {
    _timing_arc_delay_state.fallback_key_set.clear();
    return _timing_arc_delay_state.delay_map;
  }
  [[nodiscard]] const TimingArcDelayMap& get_timing_arc_delay_map() const noexcept { return _timing_arc_delay_state.delay_map; }
  [[nodiscard]] std::map<TransType, TransType>& get_trans_type_map() noexcept { return _trans_type_map; }
  [[nodiscard]] const std::map<TransType, TransType>& get_trans_type_map() const noexcept { return _trans_type_map; }
  TimingCellArc* get_timing_cell_arc() { return _timing_cell_arc; }
  const TimingCellArc* get_timing_cell_arc() const { return _timing_cell_arc; }
  bool get_is_clock_arc() const { return _is_clock_arc; }
  bool get_is_disable_arc() const { return _is_disable_arc; }
  bool get_is_loop_disable() const { return _is_loop_disable; }
  // setter
  void set_arc_name(const std::string& arc_name) { _arc_name = arc_name; }
  void set_source_pin(const std::string& source_pin) { _source_pin = source_pin; }
  void set_sink_pin(const std::string& sink_pin) { _sink_pin = sink_pin; }
  void set_owner_name(const std::string& owner_name) { _owner_name = owner_name; }
  void set_library_source_port(const std::string& library_source_port) { _library_source_port = library_source_port; }
  void set_library_sink_port(const std::string& library_sink_port) { _library_sink_port = library_sink_port; }
  void set_type(const ArcType& type) { _type = type; }
  void set_delay(const double delay) noexcept { _delay = delay; }
  void set_delay_max(const double delay_max) noexcept { _delay_max = delay_max; }
  void set_delay_min(const double delay_min) noexcept { _delay_min = delay_min; }
  void set_trans_delay_map(const std::map<AnalysisType, std::map<TransType, double>>& trans_delay_map) { _trans_delay_map = trans_delay_map; }
  void set_input_output_delay_map(const std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>>& input_output_delay_map)
  {
    _input_output_delay_map = input_output_delay_map;
  }
  void set_graph_delay_map(const std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>>& graph_delay_map)
  {
    _graph_delay_map = graph_delay_map;
  }
  void set_timing_arc_delay_map(const TimingArcDelayMap& timing_arc_delay_map)
  {
    TimingArcDelayState replacement;
    replacement.delay_map = timing_arc_delay_map;
    _timing_arc_delay_state.swap(replacement);
  }
  void replace_timing_arc_delay_state(TimingArcDelayState&& timing_arc_delay_state) noexcept
  {
    _timing_arc_delay_state.swap(timing_arc_delay_state);
    pruneTimingArcDelayFallbackKeys();
  }
  void set_trans_type_map(const std::map<TransType, TransType>& trans_type_map) { _trans_type_map = trans_type_map; }
  void set_timing_cell_arc(TimingCellArc* timing_cell_arc) { _timing_cell_arc = timing_cell_arc; }
  void set_is_clock_arc(const bool is_clock_arc) { _is_clock_arc = is_clock_arc; }
  void set_is_disable_arc(const bool is_disable_arc) { _is_disable_arc = is_disable_arc; }
  void set_is_loop_disable(const bool is_loop_disable) { _is_loop_disable = is_loop_disable; }
  // function
  void update_timing_arc_delay(int32_t timing_arc_idx, AnalysisType analysis_type, TransType input_trans_type, TransType output_trans_type, double delay,
                               bool is_initialization)
  {
    pruneTimingArcDelayFallbackKeys();
    const TimingArcDelayFallbackKey fallback_key = std::make_tuple(timing_arc_idx, analysis_type, input_trans_type, output_trans_type);
    auto& analysis_delay_map = _timing_arc_delay_state.delay_map[timing_arc_idx];
    const auto analysis_it = analysis_delay_map.find(analysis_type);
    const bool has_delay = analysis_it != analysis_delay_map.end() && analysis_it->second.contains(input_trans_type)
                           && analysis_it->second.at(input_trans_type).contains(output_trans_type);

    if (!has_delay) {
      if (!is_initialization) {
        _timing_arc_delay_state.fallback_key_set.erase(fallback_key);
      }
      analysis_delay_map[analysis_type][input_trans_type][output_trans_type] = delay;
      if (is_initialization) {
        _timing_arc_delay_state.fallback_key_set.insert(fallback_key);
      }
      return;
    }

    const bool is_fallback = _timing_arc_delay_state.fallback_key_set.contains(fallback_key);
    if (!is_initialization && is_fallback) {
      analysis_delay_map[analysis_type][input_trans_type][output_trans_type] = delay;
      _timing_arc_delay_state.fallback_key_set.erase(fallback_key);
      return;
    }
    if (is_initialization && !is_fallback) {
      return;
    }

    double& cached_delay = analysis_delay_map[analysis_type][input_trans_type][output_trans_type];
    if ((analysis_type == AnalysisType::kMin && delay < cached_delay) || (analysis_type == AnalysisType::kMax && delay > cached_delay)) {
      cached_delay = delay;
    }
  }

 private:
  friend void csr::detail::publishPreparedTimingResult(csr::detail::PreparedTimingResult&) noexcept;

  [[nodiscard]] static bool containsTimingArcDelay(const TimingArcDelayMap& delay_map, const TimingArcDelayFallbackKey& key) noexcept
  {
    const auto& [timing_arc_idx, analysis_type, input_trans_type, output_trans_type] = key;
    const auto arc_it = delay_map.find(timing_arc_idx);
    if (arc_it == delay_map.end()) {
      return false;
    }
    const auto analysis_it = arc_it->second.find(analysis_type);
    if (analysis_it == arc_it->second.end()) {
      return false;
    }
    const auto input_it = analysis_it->second.find(input_trans_type);
    return input_it != analysis_it->second.end() && input_it->second.contains(output_trans_type);
  }

  void pruneTimingArcDelayFallbackKeys() noexcept
  {
    auto& fallback_keys = _timing_arc_delay_state.fallback_key_set;
    for (auto key_it = fallback_keys.begin(); key_it != fallback_keys.end();) {
      if (!containsTimingArcDelay(_timing_arc_delay_state.delay_map, *key_it)) {
        key_it = fallback_keys.erase(key_it);
      } else {
        ++key_it;
      }
    }
  }

  std::string _arc_name;
  std::string _source_pin;
  std::string _sink_pin;
  std::string _owner_name;
  std::string _library_source_port;
  std::string _library_sink_port;
  ArcType _type = ArcType::kNone;
  double _delay = 1.0;
  double _delay_max = 1.0;
  double _delay_min = 1.0;
  std::map<AnalysisType, std::map<TransType, double>> _trans_delay_map;
  std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>> _input_output_delay_map;
  std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>> _graph_delay_map;
  TimingArcDelayState _timing_arc_delay_state;
  std::map<TransType, TransType> _trans_type_map;
  TimingCellArc* _timing_cell_arc = nullptr;
  bool _is_clock_arc = false;
  bool _is_disable_arc = false;
  bool _is_loop_disable = false;
};

}  // namespace ista
