// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingAnalyzer.hpp"
#include "DataManager.hpp"

namespace ista {
std::optional<TimingAnalyzer::CrprRoots> TimingAnalyzer::resolveCrprRoots(
    TimingPathState& state, std::string& end, AnalysisType analysis)
{
  if (state.get_crpr_clock_pin().empty()) return resolveCrprRoots(state.get_start_point(), end, analysis);
  std::pair<std::string, TransType> launch(state.get_crpr_clock_pin(), state.get_crpr_clock_trans_type());
  return resolveCrprRoots(launch, end, analysis);
}

std::optional<TimingAnalyzer::CrprRoots> TimingAnalyzer::resolveCrprRoots(
    std::string& start, std::string& end, AnalysisType analysis)
{
  auto& database = STADM.getDatabase();
  if (!isRegisterStartPoint(start) || (!isRegisterEndPoint(end) && !isTimingCheckEndPoint(end))) return std::nullopt;
  auto& pin = database.get_pin_map()[start];
  auto& instance = database.get_instance_map()[pin.get_instance_name()];
  std::pair<std::string, TransType> launch(instance.get_clock_pin_name(), getClockTransType(instance.get_clock_to_q_arc()));
  return resolveCrprRoots(launch, end, analysis);
}

std::optional<TimingAnalyzer::CrprRoots> TimingAnalyzer::resolveCrprRoots(
    std::pair<std::string, TransType>& launch, std::string& end, AnalysisType analysis)
{
  auto& database = STADM.getDatabase();
  if ((!isRegisterEndPoint(end) && !isTimingCheckEndPoint(end)) || database.get_pin_map().count(launch.first) == 0)
    return std::nullopt;
  auto& end_pin = database.get_pin_map()[end];
  auto& end_instance = database.get_instance_map()[end_pin.get_instance_name()];
  const auto launch_transition = launch.second == TransType::kNone ? TransType::kRise : launch.second;
  auto* check = getEndPointCheckArc(end, analysis);
  const auto capture_transition = check == nullptr ? TransType::kRise : getClockTransType(*check);
  return CrprRoots{{launch.first, analysis, launch_transition},
                   {end_instance.get_clock_pin_name(), getCaptureAnalysisType(analysis), capture_transition}};
}

double TimingAnalyzer::getClockReconvergencePessimism(TimingPathState& state, std::string& end,
                                                     AnalysisType analysis, std::string& common_pin_name)
{
  auto roots = resolveCrprRoots(state, end, analysis);
  return roots ? evaluateCrpr(*roots, common_pin_name) : 0.0;
}

double TimingAnalyzer::getClockReconvergencePessimism(std::string& start, std::string& end,
                                                     AnalysisType analysis, std::string& common_pin_name)
{
  auto roots = resolveCrprRoots(start, end, analysis);
  return roots ? evaluateCrpr(*roots, common_pin_name) : 0.0;
}

double TimingAnalyzer::evaluateCrpr(CrprRoots& roots, std::string& common_pin_name)
{
  auto launch_clock_path = getClockPathPinList(roots.launch.pin, roots.launch.analysis, roots.launch.transition);
  auto capture_clock_path = getClockPathPinList(roots.capture.pin, roots.capture.analysis, roots.capture.transition);
  shrinkClockPathToCrprPath(launch_clock_path);
  shrinkClockPathToCrprPath(capture_clock_path);
  std::pair<std::string, TransType> launch_common_pin;
  std::pair<std::string, TransType> capture_common_pin;
  std::size_t path_size = std::min(launch_clock_path.size(), capture_clock_path.size());
  for (std::size_t i = 0; i < path_size; i++) {
    if (launch_clock_path[i].first != capture_clock_path[i].first || launch_clock_path[i].second != capture_clock_path[i].second) {
      break;
    }
    launch_common_pin = launch_clock_path[i];
    capture_common_pin = capture_clock_path[i];
    common_pin_name = launch_clock_path[i].first;
  }
  if (common_pin_name.empty()) {
    return 0.0;
  }
  double launch_delay_delta = getClockCommonPathDelayDelta(launch_common_pin, roots.launch.analysis);
  double capture_delay_delta = getClockCommonPathDelayDelta(capture_common_pin, roots.capture.analysis);
  return std::min(launch_delay_delta, capture_delay_delta);
}

}  // namespace ista
