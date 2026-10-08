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
#pragma once

#include <cmath>
#include <cfloat>
#include "CsrArnoldiSpectrum.hpp"

#if defined(__CUDACC__)
#define ISTA_RC_HD __host__ __device__
#else
#define ISTA_RC_HD
#endif

namespace ista::csr {
namespace arnoldi_detail {

// Preserve native iteration order and stopping tolerances. These helpers
// operate on one worker's finite, validated order-five response.
ISTA_RC_HD inline double hInverse(double value)
{
  double x = 0.0;
  if (value < 1.0) {
    x = std::sqrt(2.0 * value) + 0.4 * value;
    if (value < 1E-4) {
      return x;
    }
  } else {
    x = value + 1.0;
  }
  for (int iter = 0; iter < 4; iter++) {
    double exp_x = std::exp(-x);
    double function_value = x + exp_x - 1.0 - value;
    x += function_value / (exp_x - 1.0);
  }
  return x;
}

ISTA_RC_HD inline void rampPoint(double pole_ramp, double voltage, double& pole_time, double& derivative)
{
  if (pole_ramp > 30.0) {
    pole_time = 1.0 + pole_ramp * (1.0 - voltage);
    derivative = 1.0 - voltage;
    return;
  }
  double exp_pole_ramp = std::exp(pole_ramp);
  if ((1.0 - pole_ramp * voltage) * exp_pole_ramp >= 1.0) {
    pole_time = std::log((exp_pole_ramp - 1.0) / (pole_ramp * voltage));
    derivative = exp_pole_ramp / (exp_pole_ramp - 1.0) - 1.0 / pole_ramp;
    return;
  }
  pole_time = hInverse((1.0 - voltage) * pole_ramp);
  derivative = (1.0 - voltage) / (pole_time - (1.0 - voltage) * pole_ramp);
}

ISTA_RC_HD inline double rampTime(double pole, double ramp, double voltage)
{
  double pole_ramp = pole * ramp;
  if (pole_ramp > 30.0) {
    return (1.0 + pole_ramp * (1.0 - voltage)) / pole;
  }
  double exp_pole_ramp = std::exp(pole_ramp);
  if ((1.0 - pole_ramp * voltage) * exp_pole_ramp >= 1.0) {
    return std::log((exp_pole_ramp - 1.0) / (pole_ramp * voltage)) / pole;
  }
  return hInverse((1.0 - voltage) * pole_ramp) / pole;
}

ISTA_RC_HD inline void solveRamp(double pole, double transition_time, double lower_threshold, double upper_threshold, double& ramp)
{
  for (int iter = 0; iter < 5; iter++) {
    double lower_pole_time = 0.0;
    double lower_derivative = 0.0;
    double upper_pole_time = 0.0;
    double upper_derivative = 0.0;
    rampPoint(pole * ramp, lower_threshold, lower_pole_time, lower_derivative);
    rampPoint(pole * ramp, upper_threshold, upper_pole_time, upper_derivative);
    double function_value = (lower_pole_time - upper_pole_time) / pole - transition_time;
    double derivative = lower_derivative - upper_derivative;
    if (std::abs(derivative) < 1e-6) {
      return;
    }
    ramp -= function_value / derivative;
    if (std::abs(function_value) < 1E-6) {
      return;
    }
  }
}

ISTA_RC_HD inline double voltage(double time, double driver_ramp, int order, const double* pole_list, const double* residue_list)
{
  double voltage = 0.0;
  for (int pole_idx = 0; pole_idx < order; pole_idx++) {
    double pole_time = pole_list[pole_idx] * time;
    double pole_ramp = pole_list[pole_idx] * driver_ramp;
    double response = 0.0;
    if (time < driver_ramp) {
      response = 1.0 - time / driver_ramp + (1.0 - std::exp(-pole_time)) / pole_ramp;
    } else {
      response = std::exp(pole_ramp - pole_time) * (1.0 - std::exp(-pole_ramp)) / pole_ramp;
    }
    voltage += residue_list[pole_idx] * response;
  }
  return voltage;
}

ISTA_RC_HD inline void voltageAndDerivative(double time, double driver_ramp, int order, const double* pole_list, const double* residue_list, double& voltage, double& derivative)
{
  voltage = 0.0;
  derivative = 0.0;
  for (int pole_idx = 0; pole_idx < order; pole_idx++) {
    double pole = pole_list[pole_idx];
    double pole_time = pole * time;
    double pole_ramp = pole * driver_ramp;
    double response = 0.0;
    double response_derivative = 0.0;
    if (time < driver_ramp) {
      double ramp_response = (1.0 - std::exp(-pole_time)) / pole_ramp;
      response = 1.0 - time / driver_ramp + ramp_response;
      response_derivative = -pole * ramp_response;
    } else {
      response = std::exp(pole_ramp - pole_time) * (1.0 - std::exp(-pole_ramp)) / pole_ramp;
      response_derivative = -pole * response;
    }
    voltage += residue_list[pole_idx] * response;
    derivative += residue_list[pole_idx] * response_derivative;
  }
}

ISTA_RC_HD inline double bracketedTime(double driver_ramp, int order, const double* pole_list, const double* residue_list, double voltage, double lower_time, double upper_time, double lower_voltage, double upper_voltage)
{
  double lower_function = lower_voltage - voltage;
  double upper_function = upper_voltage - voltage;
  if (lower_function == 0.0) {
    return lower_time;
  }
  if (upper_function == 0.0) {
    return upper_time;
  }

  double result_time = (lower_function * upper_time - upper_function * lower_time) / (lower_function - upper_function);
  double low_time = lower_time;
  double high_time = upper_time;
  if (lower_function < upper_function) {
    low_time = lower_time;
    high_time = upper_time;
    if (0.0 < lower_function) {
      return lower_time;
    }
    if (upper_function < 0.0) {
      return upper_time;
    }
  } else {
    low_time = upper_time;
    high_time = lower_time;
    if (0.0 < upper_function) {
      return upper_time;
    }
    if (lower_function < 0.0) {
      return lower_time;
    }
  }

  double old_delta = std::abs(upper_time - lower_time);
  double delta = old_delta;
  double function_value = 0.0;
  double derivative = 0.0;
  voltageAndDerivative(result_time, driver_ramp, order, pole_list, residue_list, function_value, derivative);
  function_value -= voltage;
  double last_function_value = 0.0;
  for (int iter = 1; iter < 10; iter++) {
    if ((((result_time - high_time) * derivative - function_value) * ((result_time - low_time) * derivative - function_value) >= 0.0)
        || (std::abs(2.0 * function_value) > std::abs(old_delta * derivative))) {
      old_delta = delta;
      delta = 0.5 * (high_time - low_time);
      if (last_function_value * function_value > 0.0) {
        if (function_value < 0.0) {
          delta = 0.9348 * (high_time - low_time);
        } else {
          delta = 0.0625 * (high_time - low_time);
        }
      }
      last_function_value = function_value;
      result_time = low_time + delta;
      if (low_time == result_time) {
        return result_time;
      }
    } else {
      old_delta = delta;
      delta = function_value / derivative;
      last_function_value = 0.0;
      double temp_time = result_time;
      result_time -= delta;
      if (temp_time == result_time) {
        return result_time;
      }
    }
    if (std::abs(delta) < 1E-6) {
      return result_time;
    }
    voltageAndDerivative(result_time, driver_ramp, order, pole_list, residue_list, function_value, derivative);
    function_value -= voltage;
    if (function_value < 0.0) {
      low_time = result_time;
    } else {
      high_time = result_time;
    }
  }
  if (std::abs(function_value) < 1E-6) {
    return result_time;
  }
  return 0.5 * (low_time + high_time);
}

// After the ramp, nonnegative residues give a monotonically decreasing sum
// of exponentials. Narrow a long tail to the original step width before using
// the native root solver; do not skip across a mixed-sign waveform.
ISTA_RC_HD inline bool narrowMonotoneTail(double driver_ramp, int order, const double* poles,
                                         const double* residues, double threshold, double step,
                                         double& time, double& value)
{
  for (int i = 0; i < order; ++i)
    if (residues[i] < 0.0) return false;
  if (time < driver_ramp || !std::isfinite(step) || !(step > 0.0))
    return false;
  double low = time, low_value = value, high = time, high_value = value;
  double stride = step;
  do {
    high = low + stride;
    if (!std::isfinite(high) || !(high > low)) return false;
    high_value = voltage(high, driver_ramp, order, poles, residues);
    if (!std::isfinite(high_value)) return false;
    if (high_value <= threshold) break;
    low = high;
    low_value = high_value;
    stride *= 2.0;
  } while (true);
  while (high - low > step) {
    const double mid = low + 0.5 * (high - low);
    if (!(mid > low && mid < high)) return false;
    const double mid_value = voltage(mid, driver_ramp, order, poles, residues);
    if (!std::isfinite(mid_value)) return false;
    if (mid_value > threshold) {
      low = mid;
      low_value = mid_value;
    } else {
      high = mid;
    }
  }
  time = low;
  value = low_value;
  return true;
}

ISTA_RC_HD inline bool thresholdTimes(double driver_ramp, int order, const double* pole_list, const double* residue_list, double upper_threshold, double& upper_time, double mid_threshold, double& mid_time, double lower_threshold, double& lower_time)
{
  // Keep the native short search. For long tails, skip only where monotonicity
  // is proven; unsupported mixed-sign long searches remain explicit failures.
  unsigned bracket_steps = 0;
  constexpr unsigned kMaxBracketSteps = 4096;
  int pole_idx = 0;
  if (residue_list[0] < 0.5) {
    for (int idx = 1; idx < order; idx++) {
      if (residue_list[idx] > 0.3 && residue_list[idx] > residue_list[0]) {
        pole_idx = idx;
        break;
      }
    }
  }
  double search_pole = (pole_list[pole_idx] < 10.0 ? pole_list[pole_idx] : 10.0);
  double waveform_at_ramp = 0.0;
  for (int idx = 0; idx < order; idx++) {
    double pole_ramp = pole_list[idx] * driver_ramp;
    waveform_at_ramp += residue_list[idx] * (1.0 - std::exp(-pole_ramp)) / pole_ramp;
  }

  double hi_min_time = 0.0;
  double hi_max_time = 0.0;
  double hi_min_voltage = 0.0;
  double hi_max_voltage = 0.0;
  double mid_min_time = 0.0;
  double mid_max_time = 0.0;
  double mid_min_voltage = 0.0;
  double mid_max_voltage = 0.0;
  double lo_min_time = 0.0;
  double lo_max_time = 0.0;
  double lo_min_voltage = 0.0;
  double lo_max_voltage = 0.0;

  if (waveform_at_ramp < lower_threshold) {
    lo_max_time = driver_ramp;
    lo_max_voltage = waveform_at_ramp;
    double candidate_time = upper_threshold * driver_ramp;
    double candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    if (candidate_voltage < mid_threshold) {
      hi_max_time = candidate_time;
      mid_max_time = candidate_time;
      lo_min_time = candidate_time;
      hi_max_voltage = candidate_voltage;
      mid_max_voltage = candidate_voltage;
      lo_min_voltage = candidate_voltage;
      candidate_time = mid_threshold * driver_ramp;
      candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
      if (candidate_voltage > upper_threshold) {
        hi_min_time = candidate_time;
        mid_min_time = candidate_time;
        lo_min_time = candidate_time;
        hi_min_voltage = candidate_voltage;
        mid_min_voltage = candidate_voltage;
        lo_min_voltage = candidate_voltage;
        if (candidate_voltage < mid_threshold) {
          mid_max_time = candidate_time;
          mid_max_voltage = candidate_voltage;
        } else {
          mid_min_time = candidate_time;
          mid_min_voltage = candidate_voltage;
        }
      } else {
        hi_max_time = candidate_time;
        mid_min_time = candidate_time;
        hi_max_voltage = candidate_voltage;
        mid_min_voltage = candidate_voltage;
        candidate_time = lower_threshold * driver_ramp;
        candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
        hi_min_time = candidate_time;
        hi_min_voltage = candidate_voltage;
      }
    } else {
      mid_min_time = candidate_time;
      lo_min_time = candidate_time;
      mid_min_voltage = candidate_voltage;
      lo_min_voltage = candidate_voltage;
      mid_max_time = lo_max_time;
      mid_max_voltage = lo_max_voltage;
      if (candidate_voltage > upper_threshold) {
        hi_min_time = mid_min_time;
        hi_min_voltage = mid_min_voltage;
        hi_max_time = mid_max_time;
        hi_max_voltage = mid_max_voltage;
      } else {
        hi_max_time = mid_min_time;
        hi_max_voltage = mid_min_voltage;
        candidate_time = lower_threshold * driver_ramp;
        candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
        hi_min_time = candidate_time;
        hi_min_voltage = candidate_voltage;
      }
    }
  } else if (waveform_at_ramp < mid_threshold) {
    hi_max_time = driver_ramp;
    mid_max_time = driver_ramp;
    lo_min_time = driver_ramp;
    hi_max_voltage = waveform_at_ramp;
    mid_max_voltage = waveform_at_ramp;
    lo_min_voltage = waveform_at_ramp;
    double candidate_time = driver_ramp + 1.6 / search_pole;
    double candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    while (candidate_voltage > lower_threshold) {
      if (++bracket_steps > kMaxBracketSteps
          && !narrowMonotoneTail(driver_ramp, order, pole_list, residue_list, lower_threshold,
                                 1.0 / search_pole, candidate_time, candidate_voltage))
        return false;
      lo_min_time = candidate_time;
      lo_min_voltage = candidate_voltage;
      const double next_time = candidate_time + 1.0 / search_pole;
      if (!std::isfinite(next_time) || !(next_time > candidate_time))
        return false;
      candidate_time = next_time;
      candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    }
    lo_max_time = candidate_time;
    lo_max_voltage = candidate_voltage;
    candidate_time = mid_threshold * driver_ramp;
    candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    mid_min_time = candidate_time;
    mid_min_voltage = candidate_voltage;
    if (candidate_voltage > upper_threshold) {
      hi_min_time = candidate_time;
      hi_min_voltage = candidate_voltage;
    } else {
      hi_max_time = candidate_time;
      hi_max_voltage = candidate_voltage;
      candidate_time = lower_threshold * driver_ramp;
      candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
      hi_min_time = candidate_time;
      hi_min_voltage = candidate_voltage;
    }
  } else if (waveform_at_ramp < upper_threshold) {
    hi_max_time = driver_ramp;
    mid_min_time = driver_ramp;
    lo_min_time = driver_ramp;
    hi_max_voltage = waveform_at_ramp;
    mid_min_voltage = waveform_at_ramp;
    lo_min_voltage = waveform_at_ramp;
    double candidate_time = lower_threshold * driver_ramp;
    double candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    hi_min_time = candidate_time;
    hi_min_voltage = candidate_voltage;
    candidate_time = driver_ramp + 0.7 / search_pole;
    candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    while (candidate_voltage > mid_threshold) {
      if (++bracket_steps > kMaxBracketSteps
          && !narrowMonotoneTail(driver_ramp, order, pole_list, residue_list, mid_threshold,
                                 0.7 / search_pole, candidate_time, candidate_voltage))
        return false;
      mid_min_time = candidate_time;
      lo_min_time = candidate_time;
      mid_min_voltage = candidate_voltage;
      lo_min_voltage = candidate_voltage;
      const double next_time = candidate_time + 0.7 / search_pole;
      if (!std::isfinite(next_time) || !(next_time > candidate_time))
        return false;
      candidate_time = next_time;
      candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    }
    mid_max_time = candidate_time;
    mid_max_voltage = candidate_voltage;
    if (candidate_voltage < lower_threshold) {
      lo_max_time = candidate_time;
      lo_max_voltage = candidate_voltage;
    } else {
      lo_min_time = candidate_time;
      lo_min_voltage = candidate_voltage;
      const double next_time = candidate_time + 1.0 / search_pole;
      if (!std::isfinite(next_time) || !(next_time > candidate_time))
        return false;
      candidate_time = next_time;
      candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
      while (candidate_voltage > lower_threshold) {
        if (++bracket_steps > kMaxBracketSteps
            && !narrowMonotoneTail(driver_ramp, order, pole_list, residue_list, lower_threshold,
                                   1.0 / search_pole, candidate_time, candidate_voltage))
          return false;
        lo_min_time = candidate_time;
        lo_min_voltage = candidate_voltage;
        const double next_time = candidate_time + 1.0 / search_pole;
        if (!std::isfinite(next_time) || !(next_time > candidate_time))
          return false;
        candidate_time = next_time;
        candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
      }
      lo_max_time = candidate_time;
      lo_max_voltage = candidate_voltage;
    }
  } else {
    double candidate_time = driver_ramp;
    double candidate_voltage = waveform_at_ramp;
    hi_min_time = candidate_time;
    mid_min_time = candidate_time;
    lo_min_time = candidate_time;
    hi_min_voltage = candidate_voltage;
    mid_min_voltage = candidate_voltage;
    lo_min_voltage = candidate_voltage;
    while (candidate_voltage > upper_threshold) {
      if (++bracket_steps > kMaxBracketSteps
          && !narrowMonotoneTail(driver_ramp, order, pole_list, residue_list, upper_threshold,
                                 1.0 / search_pole, candidate_time, candidate_voltage))
        return false;
      hi_min_time = candidate_time;
      mid_min_time = candidate_time;
      lo_min_time = candidate_time;
      hi_min_voltage = candidate_voltage;
      mid_min_voltage = candidate_voltage;
      lo_min_voltage = candidate_voltage;
      const double next_time = candidate_time + 1.0 / search_pole;
      if (!std::isfinite(next_time) || !(next_time > candidate_time))
        return false;
      candidate_time = next_time;
      candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    }
    hi_max_time = candidate_time;
    hi_max_voltage = candidate_voltage;
    while (candidate_voltage > mid_threshold) {
      if (++bracket_steps > kMaxBracketSteps
          && !narrowMonotoneTail(driver_ramp, order, pole_list, residue_list, mid_threshold,
                                 1.0 / search_pole, candidate_time, candidate_voltage))
        return false;
      mid_min_time = candidate_time;
      lo_min_time = candidate_time;
      mid_min_voltage = candidate_voltage;
      lo_min_voltage = candidate_voltage;
      const double next_time = candidate_time + 1.0 / search_pole;
      if (!std::isfinite(next_time) || !(next_time > candidate_time))
        return false;
      candidate_time = next_time;
      candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    }
    mid_max_time = candidate_time;
    mid_max_voltage = candidate_voltage;
    while (candidate_voltage > lower_threshold) {
      if (++bracket_steps > kMaxBracketSteps
          && !narrowMonotoneTail(driver_ramp, order, pole_list, residue_list, lower_threshold,
                                 1.0 / search_pole, candidate_time, candidate_voltage))
        return false;
      lo_min_time = candidate_time;
      lo_min_voltage = candidate_voltage;
      const double next_time = candidate_time + 1.0 / search_pole;
      if (!std::isfinite(next_time) || !(next_time > candidate_time))
        return false;
      candidate_time = next_time;
      candidate_voltage = voltage(candidate_time, driver_ramp, order, pole_list, residue_list);
    }
    lo_max_time = candidate_time;
    lo_max_voltage = candidate_voltage;
  }

  upper_time = bracketedTime(driver_ramp, order, pole_list, residue_list, upper_threshold, hi_min_time, hi_max_time, hi_min_voltage,
                                                  hi_max_voltage);
  mid_time = bracketedTime(driver_ramp, order, pole_list, residue_list, mid_threshold, mid_min_time, mid_max_time, mid_min_voltage,
                                                mid_max_voltage);
  lower_time = bracketedTime(driver_ramp, order, pole_list, residue_list, lower_threshold, lo_min_time, lo_max_time, lo_min_voltage,
                                                  lo_max_voltage);
  return std::isfinite(upper_time) && std::isfinite(mid_time) && std::isfinite(lower_time);
}

ISTA_RC_HD inline double effectiveCapacitance(double driver_ramp, double drive_resistance, int order, const double* pole_list, const double* residue_list, double effective_capacitance_time)
{
  double integrated_current = 0.0;
  for (int pole_idx = 0; pole_idx < order; pole_idx++) {
    double pole = pole_list[pole_idx];
    double pole_ramp = pole * driver_ramp;
    double pole_time = pole * effective_capacitance_time;
    double charge = 0.0;
    if (effective_capacitance_time <= driver_ramp) {
      double exp_time = pole_time > 40.0 ? 0.0 : std::exp(-pole_time);
      charge = exp_time - 1.0 + pole_time;
    } else {
      double exp_time = (pole_time - pole_ramp) > 40.0 ? 0.0 : std::exp(-(pole_time - pole_ramp));
      double exp_ramp = pole_ramp > 40.0 ? 0.0 : std::exp(-pole_ramp);
      charge = pole_ramp - (1.0 - exp_ramp) * exp_time;
    }
    charge /= pole_ramp * pole;
    integrated_current += residue_list[pole_idx] * charge;
  }
  integrated_current /= drive_resistance;

  double result_voltage = voltage(effective_capacitance_time, driver_ramp, order, pole_list, residue_list);
  if (std::abs(1.0 - result_voltage) < 1e-6) {
    return 0.0;
  }
  return integrated_current / (1.0 - result_voltage);
}

}  // namespace arnoldi_detail

struct CsrArnoldiThresholds
{
  double lower{0.1};
  double upper{0.9};
  double slew_derate{0.8};
  double voltage_log{0.0};
  double min_slew_factor{0.0};
};

ISTA_RC_HD inline CsrArnoldiThresholds csrArnoldiThresholds(double lower, double upper, double slew_derate)
{
  if (lower > 1.0) lower *= 0.01;
  if (upper > 1.0) upper *= 0.01;
  if (!(lower > 0.01 && upper < 0.99 && upper > lower && slew_derate > 0.0)) {
    lower = 0.1;
    upper = 0.9;
    slew_derate = 0.8;
  }
  const double upper_log = std::log(1.0 / upper);
  return {lower, upper, slew_derate, std::log(upper / lower),
          upper_log + arnoldi_detail::hInverse((1.0 - upper) / upper - upper_log)};
}

// Table slew is the raw-load NLDM result at the current Ceff, evaluated
// by the selected backend, never a native CPU-computed timing answer.
ISTA_RC_HD inline double csrArnoldiDriverRamp(double table_slew, double drive_resistance, double capacitance,
                                             const CsrArnoldiThresholds& thresholds)
{
  if (!(drive_resistance > 0.0 && capacitance > 0.0 && thresholds.voltage_log > 0.0))
    return 0.0;
  const double transition_time = thresholds.slew_derate * table_slew;
  const double min_slew = drive_resistance * capacitance * thresholds.min_slew_factor;
  const double natural_transition = thresholds.voltage_log * drive_resistance * capacitance;
  // R may be derived from this very slew. Roundoff in the inverse products
  // must not send their mathematical equality into the ill-conditioned solve.
  if (natural_transition >= transition_time
      || (std::isfinite(transition_time)
          && transition_time - natural_transition <= 8.0 * DBL_EPSILON * std::abs(transition_time)))
    return min_slew;
  double ramp = min_slew + 0.3 * transition_time;
  arnoldi_detail::solveRamp(1.0 / (drive_resistance * capacitance), transition_time,
                          thresholds.lower, thresholds.upper, ramp);
  return ramp;
}

struct CsrArnoldiCrossings
{
  double upper_time{0.0};
  double middle_time{0.0};
  double lower_time{0.0};
};

ISTA_RC_HD inline CsrRcEvaluationStatus evaluateCsrArnoldiCrossings(
    double driver_ramp, int order, const double* poles, const double* residues,
    const CsrArnoldiThresholds& thresholds, CsrArnoldiCrossings& output)
{
  output = {};
  if (order <= 0 || order > 5 || !poles || !residues)
    return CsrRcEvaluationStatus::kUnavailable;
  if (!std::isfinite(driver_ramp) || driver_ramp <= 0.0
      || !std::isfinite(thresholds.lower) || !std::isfinite(thresholds.upper)
      || !(thresholds.lower > 0.01 && thresholds.upper < 0.99
           && thresholds.upper > thresholds.lower))
    return CsrRcEvaluationStatus::kNumericFailure;
  for (int i = 0; i < order; ++i)
    if (!std::isfinite(poles[i]) || poles[i] <= 0.0 || !std::isfinite(residues[i]))
      return CsrRcEvaluationStatus::kNumericFailure;
  while (order > 1 && residues[order - 1] < 1e-8 && residues[order - 1] > -1e-8)
    --order;
  CsrArnoldiCrossings pending;
  if (!arnoldi_detail::thresholdTimes(driver_ramp, order, poles, residues,
                                     thresholds.upper, pending.upper_time, 0.5, pending.middle_time,
                                     thresholds.lower, pending.lower_time))
    return CsrRcEvaluationStatus::kNumericFailure;
  output = pending;
  return CsrRcEvaluationStatus::kSuccess;
}

}  // namespace ista::csr
#undef ISTA_RC_HD
