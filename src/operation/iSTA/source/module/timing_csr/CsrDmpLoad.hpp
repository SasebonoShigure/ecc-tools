// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include "CsrDmpDriver.hpp"

#if defined(__CUDACC__)
#define ISTA_RC_HD __host__ __device__
#else
#define ISTA_RC_HD
#endif

namespace ista::csr {
namespace dmp_detail {
ISTA_RC_HD inline double loadTime(double threshold, double pole1, double pole2, double residue1, double residue2,
                                                  double constant, double residue_pole1, double residue_pole2, double transition_time,
                                                  double transition_voltage)
{
  if (transition_voltage < threshold) {
    double logarithm_argument
        = residue1 * (std::exp(pole1 * transition_time) - 1.0) / ((1.0 - threshold) * pole1 * pole1 * transition_time);
    if (!(logarithm_argument > 0.0)) {
      return transition_time * threshold;
    }
    double time = std::log(logarithm_argument) / pole1;
    double exp_pole1_time = std::exp(-pole1 * time);
    double exp_pole2_time = std::exp(-pole2 * time);
    double exp_pole1_delta = std::exp(-pole1 * (time - transition_time));
    double exp_pole2_delta = std::exp(-pole2 * (time - transition_time));
    double voltage = (transition_time - residue_pole1 * (exp_pole1_delta - exp_pole1_time)
                      - residue_pole2 * (exp_pole2_delta - exp_pole2_time))
                     / transition_time;
    double derivative = (residue1 / pole1 * (exp_pole1_delta - exp_pole1_time)
                         - residue2 / pole2 * (exp_pole2_delta - exp_pole2_time))
                        / transition_time;
    if (std::abs(derivative) <= 1E-6) {
      return time;
    }
    return time - (voltage - threshold) / derivative;
  }

  double time = threshold * transition_time / transition_voltage;
  double exp_pole1_time = std::exp(-pole1 * time);
  double exp_pole2_time = std::exp(-pole2 * time);
  double voltage = (time - constant + residue_pole1 * exp_pole1_time + residue_pole2 * exp_pole1_time) / transition_time;
  double derivative = (1.0 - residue1 / pole1 * exp_pole1_time - residue2 / pole2 * exp_pole2_time) / transition_time;
  if (std::abs(derivative) <= 1E-6) {
    return time;
  }
  return time - (voltage - threshold) / derivative;
}

}  // namespace dmp_detail

// Preserve native one-pole fallback and the existing two-pole approximation.
ISTA_RC_HD inline CsrRcEvaluationStatus evaluateCsrDmpLoad(
    int order, const double* poles, const double* residues, double driver_slew,
    const CsrDmpThresholds& thresholds, double& wire_delay, double& load_slew)
{
  wire_delay = 0.0;
  load_slew = 0.0;
  if (order <= 0 || order > 2 || !poles || !residues)
    return CsrRcEvaluationStatus::kUnavailable;
  if (!std::isfinite(driver_slew) || driver_slew < 0.0 || !std::isfinite(poles[0]) || !(poles[0] > 0.0)
      || !std::isfinite(residues[0])
      || (order == 2 && (!std::isfinite(poles[1]) || !std::isfinite(residues[1])))
      || !(thresholds.threshold > 0.0 && thresholds.threshold < 1.0)
      || !(thresholds.lower >= 0.0 && thresholds.upper <= 1.0 && thresholds.upper > thresholds.lower)
      || !std::isfinite(thresholds.slew_derate) || thresholds.slew_derate <= 0.0)
    return CsrRcEvaluationStatus::kNumericFailure;
  const double pole1 = poles[0];
  const double one_pole_delay = 1.0 / pole1;
  if (!std::isfinite(one_pole_delay))
    return CsrRcEvaluationStatus::kNumericFailure;
  wire_delay = one_pole_delay;
  load_slew = driver_slew;
  if (order < 2 || std::abs(driver_slew) <= 1E-6)
    return CsrRcEvaluationStatus::kSuccess;
  const double pole2 = poles[1], residue1 = residues[0], residue2 = residues[1];
  if (!(pole2 > 0.0))
    return CsrRcEvaluationStatus::kSuccess;
  const double threshold = thresholds.threshold, lower_threshold = thresholds.lower;
  const double upper_threshold = thresholds.upper, slew_derate = thresholds.slew_derate;
  if (!(upper_threshold > lower_threshold) || !(slew_derate > 0.0))
    return CsrRcEvaluationStatus::kSuccess;
  double residue_pole1 = residue1 / (pole1 * pole1);
  double residue_pole2 = residue2 / (pole2 * pole2);
  double constant = residue_pole1 + residue_pole2;
  double transition_time = driver_slew * slew_derate / (upper_threshold - lower_threshold);
  if (!(transition_time > 0.0)) {
    return CsrRcEvaluationStatus::kSuccess;
  }
  double transition_voltage = (transition_time - constant + residue_pole1 * std::exp(-pole1 * transition_time)
                               + residue_pole2 * std::exp(-pole2 * transition_time))
                              / transition_time;
  double threshold_time = dmp_detail::loadTime(threshold, pole1, pole2, residue1, residue2, constant, residue_pole1,
                                                   residue_pole2, transition_time, transition_voltage);
  double lower_time = dmp_detail::loadTime(lower_threshold, pole1, pole2, residue1, residue2, constant, residue_pole1,
                                               residue_pole2, transition_time, transition_voltage);
  double upper_time = dmp_detail::loadTime(upper_threshold, pole1, pole2, residue1, residue2, constant, residue_pole1,
                                               residue_pole2, transition_time, transition_voltage);
  double dmp_wire_delay = threshold_time - transition_time * threshold;
  double dmp_load_slew = (upper_time - lower_time) / slew_derate;
  if (std::isfinite(dmp_wire_delay) && std::isfinite(dmp_load_slew) && dmp_load_slew >= 0.0) {
    wire_delay = dmp_wire_delay;
    load_slew = dmp_load_slew;
  }
  return CsrRcEvaluationStatus::kSuccess;
}
}  // namespace ista::csr
#undef ISTA_RC_HD
