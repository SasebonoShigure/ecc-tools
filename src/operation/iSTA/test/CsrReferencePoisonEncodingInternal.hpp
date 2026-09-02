// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#include "CsrReferenceInputIndependentEncodingInternal.hpp"
#include "CsrReferencePoisonSupport.hpp"
#include "TimingPathState.hpp"
#include "TimingPoint.hpp"

namespace ista::csr::test::poison_wire {

using wire::Writer;

inline constexpr std::array kPoisonAnalyses = {ista::AnalysisType::kMax, ista::AnalysisType::kMin};
inline constexpr std::array kPoisonTransitions = {ista::TransType::kRise, ista::TransType::kFall, ista::TransType::kNone};
inline constexpr std::array kPoisonPathSources = {ista::PathSourceType::kInput, ista::PathSourceType::kRegister};

inline std::uint8_t analysisToken(ista::AnalysisType value)
{
  switch (value) {
    case ista::AnalysisType::kMax:
      return 0;
    case ista::AnalysisType::kMin:
      return 1;
    case ista::AnalysisType::kNone:
      break;
  }
  throw std::runtime_error("poison canary encountered an unknown analysis mode");
}

inline std::uint8_t transitionToken(ista::TransType value)
{
  switch (value) {
    case ista::TransType::kRise:
      return 0;
    case ista::TransType::kFall:
      return 1;
    case ista::TransType::kNone:
      return 2;
  }
  throw std::runtime_error("poison canary encountered an unknown transition");
}

inline std::uint8_t pathSourceToken(ista::PathSourceType value)
{
  switch (value) {
    case ista::PathSourceType::kInput:
      return 0;
    case ista::PathSourceType::kRegister:
      return 1;
    case ista::PathSourceType::kNone:
      break;
  }
  throw std::runtime_error("poison canary encountered an unknown path source");
}

inline void appendCanaryDouble(Writer& writer, double value)
{
  if (std::isnan(value))
    throw std::runtime_error("poison canary cannot encode NaN");
  if (value == std::numeric_limits<double>::infinity()) {
    writer.appendU8(1);
  } else if (value == -std::numeric_limits<double>::infinity()) {
    writer.appendU8(2);
  } else {
    writer.appendFiniteDouble(value);
  }
}

inline double changedDouble(double value)
{
  if (std::isnan(value))
    throw std::runtime_error("poison cannot mutate NaN derived state");
  if (value == std::numeric_limits<double>::infinity())
    return 1.0;
  if (value == -std::numeric_limits<double>::infinity())
    return -1.0;
  const double changed = std::nextafter(value, std::numeric_limits<double>::infinity());
  return changed == value ? value - 1.0 : changed;
}

inline ista::TransType changedTransition(ista::TransType value)
{
  return value == ista::TransType::kRise ? ista::TransType::kFall : ista::TransType::kRise;
}

inline void appendBinding(Writer& writer, std::string_view family, const ReferencePoisonBinding& binding)
{
  writer.appendString("ista-csr-reference-input-canary/" + std::string(family) + "/1.0.0");
  writer.appendU64(binding.repeat_index);
  writer.appendString(binding.run_nonce);
  writer.appendU64(binding.session_token);
}

inline void appendArcKey(Writer& writer, const RawArc& arc)
{
  writer.appendU64(arc.native_rank);
  writer.appendString(arc.source);
  writer.appendString(arc.sink);
  writer.appendU8(static_cast<std::uint8_t>(arc.kind));
  writer.appendString(arc.owner);
  writer.appendString(arc.library_source_port);
  writer.appendString(arc.library_sink_port);
}

template <typename Value, typename AppendValue>
void appendAnalysisTransitionMap(Writer& writer, const std::map<ista::AnalysisType, std::map<ista::TransType, Value>>& values, AppendValue append_value)
{
  std::uint64_t count = 0;
  for (ista::AnalysisType analysis : kPoisonAnalyses) {
    const auto analysis_it = values.find(analysis);
    if (analysis_it == values.end())
      continue;
    for (ista::TransType transition : kPoisonTransitions)
      count += analysis_it->second.contains(transition);
  }
  writer.appendU64(count);
  for (ista::AnalysisType analysis : kPoisonAnalyses) {
    const auto analysis_it = values.find(analysis);
    if (analysis_it == values.end())
      continue;
    for (ista::TransType transition : kPoisonTransitions) {
      const auto value_it = analysis_it->second.find(transition);
      if (value_it == analysis_it->second.end())
        continue;
      writer.appendU8(analysisToken(analysis));
      writer.appendU8(transitionToken(transition));
      append_value(writer, value_it->second);
    }
  }
}

inline void appendInputOutputMap(Writer& writer, const std::map<ista::AnalysisType, std::map<ista::TransType, std::map<ista::TransType, double>>>& values)
{
  std::uint64_t count = 0;
  for (ista::AnalysisType analysis : kPoisonAnalyses) {
    const auto analysis_it = values.find(analysis);
    if (analysis_it == values.end())
      continue;
    for (ista::TransType input : kPoisonTransitions) {
      const auto input_it = analysis_it->second.find(input);
      if (input_it == analysis_it->second.end())
        continue;
      for (ista::TransType output : kPoisonTransitions)
        count += input_it->second.contains(output);
    }
  }
  writer.appendU64(count);
  for (ista::AnalysisType analysis : kPoisonAnalyses) {
    const auto analysis_it = values.find(analysis);
    if (analysis_it == values.end())
      continue;
    for (ista::TransType input : kPoisonTransitions) {
      const auto input_it = analysis_it->second.find(input);
      if (input_it == analysis_it->second.end())
        continue;
      for (ista::TransType output : kPoisonTransitions) {
        const auto output_it = input_it->second.find(output);
        if (output_it == input_it->second.end())
          continue;
        writer.appendU8(analysisToken(analysis));
        writer.appendU8(transitionToken(input));
        writer.appendU8(transitionToken(output));
        appendCanaryDouble(writer, output_it->second);
      }
    }
  }
}

inline void appendTimingArcMap(Writer& writer,
                               const std::map<std::int32_t, std::map<ista::AnalysisType, std::map<ista::TransType, std::map<ista::TransType, double>>>>& values)
{
  std::uint64_t count = 0;
  for (const auto& [arc_index, analyses] : values) {
    static_cast<void>(arc_index);
    for (ista::AnalysisType analysis : kPoisonAnalyses) {
      const auto analysis_it = analyses.find(analysis);
      if (analysis_it == analyses.end())
        continue;
      for (ista::TransType input : kPoisonTransitions) {
        const auto input_it = analysis_it->second.find(input);
        if (input_it == analysis_it->second.end())
          continue;
        for (ista::TransType output : kPoisonTransitions)
          count += input_it->second.contains(output);
      }
    }
  }
  writer.appendU64(count);
  for (const auto& [arc_index, analyses] : values) {
    for (ista::AnalysisType analysis : kPoisonAnalyses) {
      const auto analysis_it = analyses.find(analysis);
      if (analysis_it == analyses.end())
        continue;
      for (ista::TransType input : kPoisonTransitions) {
        const auto input_it = analysis_it->second.find(input);
        if (input_it == analysis_it->second.end())
          continue;
        for (ista::TransType output : kPoisonTransitions) {
          const auto output_it = input_it->second.find(output);
          if (output_it == input_it->second.end())
            continue;
          writer.appendI32(arc_index);
          writer.appendU8(analysisToken(analysis));
          writer.appendU8(transitionToken(input));
          writer.appendU8(transitionToken(output));
          appendCanaryDouble(writer, output_it->second);
        }
      }
    }
  }
}

inline void appendPathStates(Writer& writer, ista::TimingPoint& point)
{
  auto& values = point.get_path_state_map();
  std::uint64_t count = 0;
  for (ista::AnalysisType analysis : kPoisonAnalyses)
    for (ista::PathSourceType source : kPoisonPathSources)
      for (ista::TransType transition : kPoisonTransitions) {
        const auto analysis_it = values.find(analysis);
        if (analysis_it == values.end())
          continue;
        const auto source_it = analysis_it->second.find(source);
        if (source_it == analysis_it->second.end())
          continue;
        const auto transition_it = source_it->second.find(transition);
        if (transition_it != source_it->second.end())
          count += transition_it->second.size();
      }
  writer.appendU64(count);
  for (ista::AnalysisType analysis : kPoisonAnalyses)
    for (ista::PathSourceType source : kPoisonPathSources)
      for (ista::TransType transition : kPoisonTransitions) {
        const auto analysis_it = values.find(analysis);
        if (analysis_it == values.end())
          continue;
        const auto source_it = analysis_it->second.find(source);
        if (source_it == analysis_it->second.end())
          continue;
        const auto transition_it = source_it->second.find(transition);
        if (transition_it == source_it->second.end())
          continue;
        for (auto& [clock_tag, state] : transition_it->second) {
          writer.appendU8(analysisToken(analysis));
          writer.appendU8(pathSourceToken(source));
          writer.appendU8(transitionToken(transition));
          writer.appendString(clock_tag);
          appendCanaryDouble(writer, state.get_arrival());
          appendCanaryDouble(writer, state.get_slew());
          appendCanaryDouble(writer, state.get_launch_time());
          writer.appendString(state.get_start_point());
          writer.appendString(state.get_predecessor());
          writer.appendString(state.get_clock_name());
          writer.appendString(state.get_crpr_clock_pin());
          writer.appendU64(state.get_predecessor_arc_idx());
          appendCanaryDouble(writer, state.get_predecessor_arc_delay());
          writer.appendU8(transitionToken(state.get_trans_type()));
          writer.appendU8(transitionToken(state.get_predecessor_trans_type()));
          writer.appendU8(transitionToken(state.get_crpr_clock_trans_type()));
        }
      }
}

}  // namespace ista::csr::test::poison_wire
