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
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>

#include "AnalysisType.hpp"
#include "Arc.hpp"
#include "CsrRealDifferentialNativeReadback.hpp"
#include "TransType.hpp"

namespace ista::csr::real_diff {
namespace {

constexpr std::array<AnalysisType, kAnalysisCount> kNativeAnalyses{AnalysisType::kMax, AnalysisType::kMin};
constexpr std::array<TransType, kTransitionCount> kNativeTransitions{TransType::kRise, TransType::kFall};

std::size_t analysisIndex(AnalysisType analysis)
{
  switch (analysis) {
    case AnalysisType::kMax:
      return 0;
    case AnalysisType::kMin:
      return 1;
    case AnalysisType::kNone:
      break;
  }
  throw std::runtime_error("reference native readback: unknown Arc analysis lane");
}

std::size_t transitionIndex(TransType transition)
{
  switch (transition) {
    case TransType::kRise:
      return 0;
    case TransType::kFall:
      return 1;
    case TransType::kNone:
      break;
  }
  throw std::runtime_error("reference native readback: unknown Arc transition lane");
}

Transition convertTransition(TransType transition)
{
  switch (transition) {
    case TransType::kRise:
      return Transition::kRise;
    case TransType::kFall:
      return Transition::kFall;
    case TransType::kNone:
      return Transition::kNone;
  }
  throw std::runtime_error("reference native readback: unknown Arc transition value");
}

double checkedDouble(double value)
{
  if (std::isnan(value)) {
    throw std::runtime_error("reference native readback: NaN Arc timing value");
  }
  return value;
}

template <typename NativeValue, typename NeutralValue, typename Convert>
void importLaneMap(const std::map<AnalysisType, std::map<TransType, NativeValue>>& source, NeutralLaneMatrix<NeutralValue>& destination, Convert convert)
{
  for (const auto& [analysis, transitions] : source) {
    const std::size_t analysis_index = analysisIndex(analysis);
    for (const auto& [transition, value] : transitions) {
      destination[analysis_index][transitionIndex(transition)] = convert(value);
    }
  }
}

void importArcLaneMap(const std::map<AnalysisType, std::map<TransType, std::map<TransType, double>>>& source, NeutralArcLaneMatrix<double>& destination)
{
  for (const auto& [analysis, inputs] : source) {
    const std::size_t analysis_index = analysisIndex(analysis);
    for (const auto& [input_transition, outputs] : inputs) {
      const std::size_t input_index = transitionIndex(input_transition);
      for (const auto& [output_transition, value] : outputs) {
        destination[analysis_index][input_index][transitionIndex(output_transition)] = checkedDouble(value);
      }
    }
  }
}

bool hasTimingArcDelay(const TimingArcDelayMap& delay_map, const TimingArcDelayFallbackKey& key)
{
  const auto& [timing_arc_index, analysis, input_transition, output_transition] = key;
  const auto arc = delay_map.find(timing_arc_index);
  if (arc == delay_map.end()) {
    return false;
  }
  const auto analysis_lane = arc->second.find(analysis);
  if (analysis_lane == arc->second.end()) {
    return false;
  }
  const auto input_lane = analysis_lane->second.find(input_transition);
  return input_lane != analysis_lane->second.end() && input_lane->second.contains(output_transition);
}

void validateTimingArcState(const TimingArcDelayState& state, const std::set<std::int32_t>& known_indices)
{
  for (const auto& [timing_arc_index, analyses] : state.delay_map) {
    if (!known_indices.contains(timing_arc_index)) {
      throw std::runtime_error("reference native readback: timing-arc delay index does not resolve");
    }
    for (const auto& [analysis, inputs] : analyses) {
      analysisIndex(analysis);
      for (const auto& [input_transition, outputs] : inputs) {
        transitionIndex(input_transition);
        for (const auto& [output_transition, delay] : outputs) {
          transitionIndex(output_transition);
          checkedDouble(delay);
        }
      }
    }
  }
  for (const TimingArcDelayFallbackKey& fallback : state.fallback_key_set) {
    const auto& [timing_arc_index, analysis, input_transition, output_transition] = fallback;
    if (!known_indices.contains(timing_arc_index)) {
      throw std::runtime_error("reference native readback: fallback timing-arc index does not resolve");
    }
    analysisIndex(analysis);
    transitionIndex(input_transition);
    transitionIndex(output_transition);
    if (!hasTimingArcDelay(state.delay_map, fallback)) {
      throw std::runtime_error("reference native readback: fallback key is not a delay-map subset");
    }
  }
}

NeutralArcEntry readTimingArc(std::int32_t timing_arc_index, const TimingArcDelayState& state)
{
  NeutralArcEntry entry;
  entry.timing_arc_index = timing_arc_index;
  const auto arc = state.delay_map.find(timing_arc_index);
  if (arc == state.delay_map.end()) {
    return entry;
  }
  for (std::size_t analysis_index = 0; analysis_index < kAnalysisCount; ++analysis_index) {
    const AnalysisType analysis = kNativeAnalyses[analysis_index];
    const auto analysis_lane = arc->second.find(analysis);
    if (analysis_lane == arc->second.end()) {
      continue;
    }
    for (std::size_t input_index = 0; input_index < kTransitionCount; ++input_index) {
      const TransType input_transition = kNativeTransitions[input_index];
      const auto input_lane = analysis_lane->second.find(input_transition);
      if (input_lane == analysis_lane->second.end()) {
        continue;
      }
      for (std::size_t output_index = 0; output_index < kTransitionCount; ++output_index) {
        const TransType output_transition = kNativeTransitions[output_index];
        const auto value = input_lane->second.find(output_transition);
        if (value == input_lane->second.end()) {
          continue;
        }
        const TimingArcDelayFallbackKey fallback{timing_arc_index, analysis, input_transition, output_transition};
        entry.lanes[analysis_index][input_index][output_index] = NeutralArcDelay{checkedDouble(value->second), state.fallback_key_set.contains(fallback)};
      }
    }
  }
  return entry;
}

}  // namespace

NeutralArc readNativeTimingArc(const Arc& arc, std::size_t native_rank, const NativeReadbackKeys& keys)
{
  NeutralArc timing;
  timing.key = requireNativeArcKey(keys, native_rank);
  timing.delay = checkedDouble(arc.get_delay());
  timing.min_delay = checkedDouble(arc.get_delay_min());
  timing.max_delay = checkedDouble(arc.get_delay_max());
  importLaneMap(arc.get_trans_delay_map(), timing.transition_delays, [](double value) { return checkedDouble(value); });
  importArcLaneMap(arc.get_input_output_delay_map(), timing.input_output_delays);
  importArcLaneMap(arc.get_graph_delay_map(), timing.graph_delays);
  for (const auto& [input_transition, output_transition] : arc.get_trans_type_map()) {
    timing.transition_types[transitionIndex(input_transition)] = convertTransition(output_transition);
  }

  std::set<std::int32_t> timing_arc_indices;
  for (const LibraryArcSignature& signature : timing.key.base.library_arcs) {
    if (!timing_arc_indices.insert(signature.arc_index).second) {
      throw std::runtime_error("reference native readback: duplicate signed timing-arc index");
    }
  }
  const TimingArcDelayState& state = arc.get_timing_arc_delay_state();
  validateTimingArcState(state, timing_arc_indices);
  timing.timing_arcs.reserve(timing_arc_indices.size());
  for (std::int32_t timing_arc_index : timing_arc_indices) {
    timing.timing_arcs.push_back(readTimingArc(timing_arc_index, state));
  }
  return timing;
}

}  // namespace ista::csr::real_diff
