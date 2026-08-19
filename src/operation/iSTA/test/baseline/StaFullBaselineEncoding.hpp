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

#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>

#include "DataManager.hpp"
#include "json.hpp"

namespace ista::baseline_test {

using Json = nlohmann::json;

inline Json sentinel(const std::string& kind)
{
  return Json{{"sentinel", kind}};
}

inline Json encodeNumber(double value)
{
  if (std::isnan(value)) {
    throw std::runtime_error("NaN is not a valid baseline value");
  }
  if (std::isinf(value)) {
    return sentinel(value > 0.0 ? "pos_inf" : "neg_inf");
  }
  return value;
}

template <typename Enum, std::size_t N>
std::string enumName(Enum value, const std::array<const char*, N>& names, const char* type)
{
  std::size_t index = static_cast<std::size_t>(value);
  if (index >= names.size()) {
    throw std::runtime_error(std::string("unknown ") + type);
  }
  return names[index];
}

inline std::string analysisName(AnalysisType value)
{
  return GetAnalysisTypeName{}(value);
}

inline std::string sourceName(PathSourceType value)
{
  return enumName(value, std::array{"none", "input", "register"}, "PathSourceType");
}

inline std::string transName(TransType value)
{
  return GetTransTypeName{}(value);
}

inline std::string arcTypeName(ArcType value)
{
  return GetArcTypeName{}(value);
}

inline std::string directionName(PinDirection value)
{
  return GetPinDirectionName{}(value);
}

inline std::string checkName(TimingCheckType value)
{
  return GetTimingCheckTypeName{}(value);
}

inline std::string powerGroupName(PowerGroupType value)
{
  return enumName(value, std::array{"none", "io_pad", "memory", "black_box", "clock_network", "register", "sequential", "combinational"}, "PowerGroupType");
}

inline std::string arcSenseName(TimingArcSense value)
{
  return enumName(value, std::array{"none", "positive", "negative", "non_unate"}, "TimingArcSense");
}

inline std::string timeUnitName(TimingTimeUnit value)
{
  return enumName(value, std::array{"none", "ns", "ps", "fs"}, "TimingTimeUnit");
}

inline std::string capUnitName(TimingCapacitiveUnit value)
{
  return enumName(value, std::array{"none", "pf", "ff", "f"}, "TimingCapacitiveUnit");
}

inline std::string resistanceUnitName(TimingResistanceUnit value)
{
  return enumName(value, std::array{"none", "ohm", "kohm"}, "TimingResistanceUnit");
}

template <typename T, typename Encoder>
Json encodeAnalysisTrans(std::map<AnalysisType, std::map<TransType, T>>& values, Encoder encode)
{
  Json result = Json::array();
  for (auto& [analysis, trans_values] : values) {
    for (auto& [trans, value] : trans_values) {
      result.push_back({{"analysis", analysisName(analysis)}, {"transition", transName(trans)}, {"value", encode(value)}});
    }
  }
  return result;
}

template <typename T, typename Encoder>
Json encodeAnalysisInputOutput(std::map<AnalysisType, std::map<TransType, std::map<TransType, T>>>& values, Encoder encode)
{
  Json result = Json::array();
  for (auto& [analysis, inputs] : values) {
    for (auto& [input, outputs] : inputs) {
      for (auto& [output, value] : outputs) {
        result.push_back(
            {{"analysis", analysisName(analysis)}, {"input_transition", transName(input)}, {"output_transition", transName(output)}, {"value", encode(value)}});
      }
    }
  }
  return result;
}

}  // namespace ista::baseline_test
