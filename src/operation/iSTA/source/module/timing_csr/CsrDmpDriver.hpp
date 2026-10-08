// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include "CsrArnoldiSpectrum.hpp"
#include "CsrDmpNumerics.hpp"
#include <cstdint>

#if defined(__CUDACC__)
#define ISTA_RC_HD __host__ __device__
#else
#define ISTA_RC_HD
#endif

namespace ista::csr {

struct CsrDmpThresholds
{
  double threshold{0.5};
  double lower{0.3};
  double upper{0.7};
  double slew_derate{1.0};
};

ISTA_RC_HD inline CsrDmpThresholds csrDmpThresholds(double threshold, double lower, double upper, double derate)
{
  return {threshold > 1.0 ? threshold * 0.01 : threshold,
          lower > 1.0 ? lower * 0.01 : lower,
          upper > 1.0 ? upper * 0.01 : upper, derate};
}

struct CsrDmpDriverResult
{
  double gate_delay{0.0};
  double driver_slew{0.0};
  double effective_capacitance{0.0};
};

namespace dmp_detail {
// Native DMP arithmetic, with fixed scratch owned by one driver worker.
// The native oracle retains its independent implementation.
template <class Tables>
class DriverSolver
{
 public:
  ISTA_RC_HD DriverSolver(double driver_capacitance, double resistance, double load_capacitance,
                         const CsrDmpThresholds& thresholds, const Tables& tables)
      : _tables(tables)
  {
    _driver_capacitance = driver_capacitance;
    _pi_resistance = resistance;
    _load_capacitance = load_capacitance;
    _threshold = thresholds.threshold;
    _lower_threshold = thresholds.lower;
    _upper_threshold = thresholds.upper;
    _slew_derate = thresholds.slew_derate;
  }

  ISTA_RC_HD bool run(double& gate_delay, double& driver_slew, double& effective_capacitance)
  {
    _driver_resistance = calcGateResistance();
    if (!std::isfinite(_driver_resistance) || _driver_resistance < 0.0) {
      _numeric_failure = !std::isfinite(_driver_resistance);
      return false;
    }
    if (_driver_resistance < kCapacitiveDriverResistance || _pi_resistance < _driver_resistance * 1E-3 || _load_capacitance == 0.0
        || _load_capacitance < _driver_capacitance * 1E-3 || _pi_resistance == 0.0) {
      return calcCap(gate_delay, driver_slew, effective_capacitance);
    }
    if (_driver_capacitance < _load_capacitance * 1E-3) {
      return calcZeroC2(gate_delay, driver_slew, effective_capacitance);
    }
    return calcPi(gate_delay, driver_slew, effective_capacitance);
  }

  ISTA_RC_HD bool numericFailure() const { return _numeric_failure; }
 private:
  const Tables& _tables;
  bool _numeric_failure{false};
  static constexpr int32_t kStartTimeIndex = 0;
  static constexpr int32_t kTransitionTimeIndex = 1;
  static constexpr int32_t kEffectiveCapacitanceIndex = 2;
  static constexpr int32_t kLowerVoltageIndex = 0;
  static constexpr int32_t kThresholdVoltageIndex = 1;
  static constexpr int32_t kCurrentIndex = 2;
  static constexpr int32_t kMaxNewtonIteration = 100;
  static constexpr int32_t kMaxRootIteration = 20;
  static constexpr double kDriverParameterTolerance = 0.01;
  static constexpr double kThresholdTimeTolerance = 0.01;
  static constexpr double kTinyNumber = 1E-20;
  static constexpr double kGateResistanceCapacitanceStep = 1E-3;
  static constexpr double kCapacitiveDriverResistance = 1E-5;
  double _driver_capacitance = 0.0;
  double _pi_resistance = 0.0;
  double _load_capacitance = 0.0;
  double _driver_resistance = 0.0;
  double _threshold = 0.5;
  double _lower_threshold = 0.3;
  double _upper_threshold = 0.7;
  double _slew_derate = 1.0;
  bool _is_pi = false;
  bool _is_zero_c2 = false;
  int32_t _newton_order = 0;
  double _start_time = 0.0;
  double _transition_time = 0.0;
  double _pi_pole1 = 0.0;
  double _pi_pole2 = 0.0;
  double _pi_zero = 0.0;
  double _pi_scale = 0.0;
  double _pi_constant1 = 0.0;
  double _pi_constant2 = 0.0;
  double _pi_residue1 = 0.0;
  double _pi_residue2 = 0.0;
  double _pi_current_constant = 0.0;
  double _pi_current_residue1 = 0.0;
  double _pi_current_residue2 = 0.0;
  double _zero_pole = 0.0;
  double _zero_zero = 0.0;
  double _zero_scale = 0.0;
  double _zero_constant1 = 0.0;
  double _zero_constant2 = 0.0;
  double _zero_residue = 0.0;
  double _parameter_list[3]{};
  double _function_list[3]{};
  double _jacobian[3][3]{};
  double _delta_list[3]{};

  ISTA_RC_HD static double minimum(double a, double b) { return b < a ? b : a; }
  ISTA_RC_HD static void swap(double& a, double& b) { double t = a; a = b; b = t; }

  ISTA_RC_HD double calcGateResistance()
  {
    double capacitance1 = _driver_capacitance + _load_capacitance;
    double capacitance2 = capacitance1 + kGateResistanceCapacitanceStep;
    double delay1 = 0.0;
    double slew1 = 0.0;
    double delay2 = 0.0;
    double slew2 = 0.0;
    if (!getGateDelaySlew(capacitance1, delay1, slew1) || !getGateDelaySlew(capacitance2, delay2, slew2)) {
      return 0.0;
    }
    return -std::log(_threshold) * std::abs(delay1 - delay2) / (capacitance2 - capacitance1);
  }

  ISTA_RC_HD bool getGateDelaySlew(double capacitance, double& gate_delay, double& gate_slew)
  {
    gate_delay = _tables.delay(capacitance);
    gate_slew = _tables.slew(capacitance);
    _numeric_failure |= !std::isfinite(gate_delay) || !std::isfinite(gate_slew);
    return !_numeric_failure && gate_slew >= 0.0;
  }

  ISTA_RC_HD bool calcCap(double& gate_delay, double& driver_slew, double& effective_capacitance)
  {
    effective_capacitance = _driver_capacitance + _load_capacitance;
    return getGateDelaySlew(effective_capacitance, gate_delay, driver_slew);
  }

  ISTA_RC_HD bool calcPi(double& gate_delay, double& driver_slew, double& effective_capacitance)
  {
    if (!initPi()) {
      return calcCap(gate_delay, driver_slew, effective_capacitance);
    }

    if (!findDriverParams(_driver_capacitance + _load_capacitance) && !findDriverParams(_driver_capacitance)) {
      return calcCap(gate_delay, driver_slew, effective_capacitance);
    }

    effective_capacitance = _parameter_list[kEffectiveCapacitanceIndex];
    double table_slew = 0.0;
    if (!getGateDelaySlew(effective_capacitance, gate_delay, table_slew)) {
      return false;
    }
    double waveform_delay = 0.0;
    if (!findDriverDelaySlew(waveform_delay, driver_slew)) {
      driver_slew = table_slew;
    }
    return std::isfinite(gate_delay) && std::isfinite(driver_slew);
  }

  ISTA_RC_HD bool initPi()
  {
    _is_pi = true;
    _is_zero_c2 = false;
    _newton_order = 3;
    double denominator = _pi_resistance * _driver_resistance * _load_capacitance * _driver_capacitance;
    double coefficient = _driver_resistance * (_load_capacitance + _driver_capacitance) + _pi_resistance * _load_capacitance;
    double discriminant = coefficient * coefficient - 4.0 * denominator;
    if (!(denominator > 0.0) || discriminant < 0.0) {
      return false;
    }

    _pi_zero = 1.0 / (_pi_resistance * _load_capacitance);
    _pi_scale = 1.0 / (_driver_resistance * _driver_capacitance);
    double root = std::sqrt(discriminant);
    _pi_pole1 = (coefficient + root) / (2.0 * denominator);
    _pi_pole2 = (coefficient - root) / (2.0 * denominator);
    double pole_product = _pi_pole1 * _pi_pole2;
    if (!(_pi_pole1 > 0.0) || !(_pi_pole2 > 0.0) || !(pole_product > 0.0)
        || std::abs(_pi_pole2 - _pi_pole1) < kTinyNumber) {
      return false;
    }

    _pi_constant2 = _pi_zero / pole_product;
    _pi_constant1 = (1.0 - _pi_constant2 * (_pi_pole1 + _pi_pole2)) / pole_product;
    _pi_residue2 = (_pi_constant1 * _pi_pole1 + _pi_constant2) / (_pi_pole2 - _pi_pole1);
    _pi_residue1 = -_pi_constant1 - _pi_residue2;
    double current_zero = (_load_capacitance + _driver_capacitance)
                          / (_pi_resistance * _load_capacitance * _driver_capacitance);
    _pi_current_constant = current_zero / pole_product;
    _pi_current_residue1 = (current_zero - _pi_pole1) / (_pi_pole1 * (_pi_pole1 - _pi_pole2));
    _pi_current_residue2 = (current_zero - _pi_pole2) / (_pi_pole2 * (_pi_pole2 - _pi_pole1));
    return std::isfinite(_pi_residue1) && std::isfinite(_pi_residue2) && std::isfinite(_pi_current_constant)
           && std::isfinite(_pi_current_residue1) && std::isfinite(_pi_current_residue2);
  }

  ISTA_RC_HD bool calcZeroC2(double& gate_delay, double& driver_slew, double& effective_capacitance)
  {
    if (!initZeroC2()) {
      return calcCap(gate_delay, driver_slew, effective_capacitance);
    }

    effective_capacitance = _load_capacitance;
    if (findDriverParams(effective_capacitance) && findDriverDelaySlew(gate_delay, driver_slew)) {
      return true;
    }
    return getGateDelaySlew(effective_capacitance, gate_delay, driver_slew);
  }

  ISTA_RC_HD bool initZeroC2()
  {
    _is_pi = false;
    _is_zero_c2 = true;
    _newton_order = 2;
    if (!(_pi_resistance > 0.0) || !(_load_capacitance > 0.0) || !(_driver_resistance > 0.0)) {
      return false;
    }

    _zero_zero = 1.0 / (_pi_resistance * _load_capacitance);
    _zero_pole = 1.0 / (_load_capacitance * (_driver_resistance + _pi_resistance));
    _zero_scale = _zero_pole / _zero_zero;
    if (!(_zero_scale > 0.0) || !(_zero_pole > 0.0)) {
      return false;
    }
    _zero_constant2 = 1.0 / _zero_scale;
    _zero_constant1 = (_zero_pole - _zero_zero) / (_zero_pole * _zero_pole);
    _zero_residue = -_zero_constant1;
    return std::isfinite(_zero_constant1) && std::isfinite(_zero_constant2) && std::isfinite(_zero_residue);
  }

  ISTA_RC_HD bool findDriverParams(double effective_capacitance)
  {
    if (_newton_order == 3) {
      _parameter_list[kEffectiveCapacitanceIndex] = effective_capacitance;
    }
    double threshold_delay = 0.0;
    double lower_delay = 0.0;
    double measured_slew = 0.0;
    if (!getGateDelays(effective_capacitance, threshold_delay, lower_delay, measured_slew)) {
      return false;
    }

    double threshold_span = _upper_threshold - _lower_threshold;
    double transition_time = measured_slew / threshold_span;
    double start_time = threshold_delay + std::log(1.0 - _threshold) * _driver_resistance * effective_capacitance
                        - _threshold * transition_time;
    _parameter_list[kTransitionTimeIndex] = transition_time;
    _parameter_list[kStartTimeIndex] = start_time;
    if (!newtonRaphson()) {
      return false;
    }
    _start_time = _parameter_list[kStartTimeIndex];
    _transition_time = _parameter_list[kTransitionTimeIndex];
    return std::isfinite(_start_time) && std::isfinite(_transition_time) && _transition_time > 0.0;
  }

  ISTA_RC_HD bool getGateDelays(double effective_capacitance, double& threshold_delay, double& lower_delay, double& measured_slew)
  {
    double table_slew = 0.0;
    if (!getGateDelaySlew(effective_capacitance, threshold_delay, table_slew)) {
      return false;
    }
    measured_slew = table_slew * _slew_derate;
    double threshold_span = _upper_threshold - _lower_threshold;
    if (!(threshold_span > 0.0)) {
      return false;
    }
    lower_delay = threshold_delay - measured_slew * (_threshold - _lower_threshold) / threshold_span;
    return measured_slew > 0.0;
  }

  ISTA_RC_HD bool newtonRaphson()
  {
    for (int32_t iteration = 0; iteration < kMaxNewtonIteration; iteration++) {
      if (!evalDmpEqns()) {
        return false;
      }
      for (int32_t index = 0; index < _newton_order; index++) {
        _delta_list[index] = -_function_list[index];
      }
      if (!dmp_detail::solveJacobian(_newton_order, _jacobian, _delta_list)) {
        return false;
      }

      bool is_converged = true;
      for (int32_t index = 0; index < _newton_order; index++) {
        if (!std::isfinite(_delta_list[index])
            || std::abs(_delta_list[index]) > std::abs(_parameter_list[index]) * kDriverParameterTolerance) {
          is_converged = false;
        }
        _parameter_list[index] += _delta_list[index];
      }
      if (is_converged) {
        return evalDmpEqns();
      }
    }
    return false;
  }

  ISTA_RC_HD bool evalDmpEqns()
  {
    if (_is_pi) {
      return evalPiEqns();
    }
    if (_is_zero_c2) {
      return evalOnePoleEqns();
    }
    return false;
  }

  ISTA_RC_HD bool evalPiEqns()
  {
    double start_time = _parameter_list[kStartTimeIndex];
    double transition_time = _parameter_list[kTransitionTimeIndex];
    double effective_capacitance = _parameter_list[kEffectiveCapacitanceIndex];
    double total_capacitance = _load_capacitance + _driver_capacitance;
    if (!(effective_capacitance > 0.0) || effective_capacitance > total_capacitance || !(transition_time > 0.0)) {
      return false;
    }

    double threshold_delay = 0.0;
    double lower_delay = 0.0;
    double measured_slew = 0.0;
    if (!getGateDelays(effective_capacitance, threshold_delay, lower_delay, measured_slew)) {
      return false;
    }
    double effective_capacitance_time = measured_slew / (_upper_threshold - _lower_threshold);
    effective_capacitance_time = minimum(effective_capacitance_time, 1.4 * transition_time);

    double threshold_voltage = 0.0;
    double lower_voltage = 0.0;
    calcCapacitiveWaveform(threshold_delay, start_time, transition_time, effective_capacitance, threshold_voltage);
    calcCapacitiveWaveform(lower_delay, start_time, transition_time, effective_capacitance, lower_voltage);
    _function_list[kCurrentIndex] = calcPiCurrentDifference(transition_time, effective_capacitance_time, effective_capacitance);
    _function_list[kThresholdVoltageIndex] = threshold_voltage - _threshold;
    _function_list[kLowerVoltageIndex] = lower_voltage - _lower_threshold;

    double exp_pole1 = calcDmpExp(-_pi_pole1 * transition_time);
    double exp_pole2 = calcDmpExp(-_pi_pole2 * transition_time);
    double exp_effective = calcDmpExp(-transition_time / (_driver_resistance * effective_capacitance));
    _jacobian[kCurrentIndex][kStartTimeIndex] = 0.0;
    _jacobian[kCurrentIndex][kTransitionTimeIndex]
        = (-_pi_current_constant * transition_time + _pi_current_residue1 * transition_time * exp_pole1
           - (2.0 * _pi_current_residue1 / _pi_pole1) * (1.0 - exp_pole1)
           + _pi_current_residue2 * transition_time * exp_pole2
           - (2.0 * _pi_current_residue2 / _pi_pole2) * (1.0 - exp_pole2)
           + _driver_resistance * effective_capacitance
                 * (transition_time + transition_time * exp_effective
                    - 2.0 * _driver_resistance * effective_capacitance * (1.0 - exp_effective)))
          / (_driver_resistance * transition_time * transition_time * transition_time);
    _jacobian[kCurrentIndex][kEffectiveCapacitanceIndex]
        = (2.0 * _driver_resistance * effective_capacitance - transition_time
           - (2.0 * _driver_resistance * effective_capacitance + transition_time) * exp_effective)
          / (transition_time * transition_time);

    calcCapacitiveWaveformDerivative(lower_delay, start_time, transition_time, effective_capacitance,
                                     _jacobian[kLowerVoltageIndex][kStartTimeIndex],
                                     _jacobian[kLowerVoltageIndex][kTransitionTimeIndex],
                                     _jacobian[kLowerVoltageIndex][kEffectiveCapacitanceIndex]);
    calcCapacitiveWaveformDerivative(threshold_delay, start_time, transition_time, effective_capacitance,
                                     _jacobian[kThresholdVoltageIndex][kStartTimeIndex],
                                     _jacobian[kThresholdVoltageIndex][kTransitionTimeIndex],
                                     _jacobian[kThresholdVoltageIndex][kEffectiveCapacitanceIndex]);
    return std::isfinite(_function_list[kCurrentIndex]) && std::isfinite(_function_list[kThresholdVoltageIndex])
           && std::isfinite(_function_list[kLowerVoltageIndex]);
  }

  ISTA_RC_HD double calcPiCurrentDifference(double transition_time, double effective_capacitance_time, double effective_capacitance)
  {
    double exp_pole1 = calcDmpExp(-_pi_pole1 * effective_capacitance_time);
    double exp_pole2 = calcDmpExp(-_pi_pole2 * effective_capacitance_time);
    double exp_effective = calcDmpExp(-effective_capacitance_time / (_driver_resistance * effective_capacitance));
    double pi_current = (_pi_current_constant * effective_capacitance_time
                         + (_pi_current_residue1 / _pi_pole1) * (1.0 - exp_pole1)
                         + (_pi_current_residue2 / _pi_pole2) * (1.0 - exp_pole2))
                        / (_driver_resistance * effective_capacitance_time * transition_time);
    double effective_current
        = (_driver_resistance * effective_capacitance * effective_capacitance_time
           - std::pow(_driver_resistance * effective_capacitance, 2) * (1.0 - exp_effective))
          / (_driver_resistance * effective_capacitance_time * transition_time);
    return pi_current - effective_current;
  }

  ISTA_RC_HD bool evalOnePoleEqns()
  {
    double start_time = _parameter_list[kStartTimeIndex];
    double transition_time = _parameter_list[kTransitionTimeIndex];
    double threshold_delay = 0.0;
    double lower_delay = 0.0;
    double measured_slew = 0.0;
    if (!getGateDelays(_load_capacitance, threshold_delay, lower_delay, measured_slew) || !(transition_time > 0.0)) {
      return false;
    }

    double threshold_voltage = 0.0;
    double lower_voltage = 0.0;
    calcCapacitiveWaveform(threshold_delay, start_time, transition_time, _load_capacitance, threshold_voltage);
    calcCapacitiveWaveform(lower_delay, start_time, transition_time, _load_capacitance, lower_voltage);
    _function_list[kThresholdVoltageIndex] = threshold_voltage - _threshold;
    _function_list[kLowerVoltageIndex] = lower_voltage - _lower_threshold;

    double ignored_capacitance_derivative = 0.0;
    calcCapacitiveWaveformDerivative(lower_delay, start_time, transition_time, _load_capacitance,
                                     _jacobian[kLowerVoltageIndex][kStartTimeIndex],
                                     _jacobian[kLowerVoltageIndex][kTransitionTimeIndex], ignored_capacitance_derivative);
    calcCapacitiveWaveformDerivative(threshold_delay, start_time, transition_time, _load_capacitance,
                                     _jacobian[kThresholdVoltageIndex][kStartTimeIndex],
                                     _jacobian[kThresholdVoltageIndex][kTransitionTimeIndex], ignored_capacitance_derivative);
    return std::isfinite(_function_list[kThresholdVoltageIndex]) && std::isfinite(_function_list[kLowerVoltageIndex]);
  }

  ISTA_RC_HD void calcCapacitiveWaveform(double time, double start_time, double transition_time, double capacitance, double& voltage)
  {
    double shifted_time = time - start_time;
    if (shifted_time <= 0.0) {
      voltage = 0.0;
    } else if (shifted_time <= transition_time) {
      voltage = calcCapacitiveUnitRamp(shifted_time, capacitance) / transition_time;
    } else {
      voltage = (calcCapacitiveUnitRamp(shifted_time, capacitance)
                 - calcCapacitiveUnitRamp(shifted_time - transition_time, capacitance))
                / transition_time;
    }
  }

  ISTA_RC_HD double calcCapacitiveUnitRamp(double time, double capacitance)
  {
    double time_constant = _driver_resistance * capacitance;
    if (!(time_constant > 0.0)) {
      return time;
    }
    return time - time_constant * (1.0 - calcDmpExp(-time / time_constant));
  }

  ISTA_RC_HD void calcCapacitiveWaveformDerivative(double time, double start_time, double transition_time, double capacitance,
                                                           double& start_derivative, double& transition_derivative,
                                                           double& capacitance_derivative)
  {
    double shifted_time = time - start_time;
    if (shifted_time <= 0.0) {
      start_derivative = 0.0;
      transition_derivative = 0.0;
      capacitance_derivative = 0.0;
    } else if (shifted_time <= transition_time) {
      start_derivative = -calcCapacitiveUnitRampTimeDerivative(shifted_time, capacitance) / transition_time;
      transition_derivative = -calcCapacitiveUnitRamp(shifted_time, capacitance) / (transition_time * transition_time);
      capacitance_derivative = calcCapacitiveUnitRampCapDerivative(shifted_time, capacitance) / transition_time;
    } else {
      start_derivative = -(calcCapacitiveUnitRampTimeDerivative(shifted_time, capacitance)
                           - calcCapacitiveUnitRampTimeDerivative(shifted_time - transition_time, capacitance))
                         / transition_time;
      transition_derivative
          = -(calcCapacitiveUnitRamp(shifted_time, capacitance) + calcCapacitiveUnitRamp(shifted_time - transition_time, capacitance))
                / (transition_time * transition_time)
            + calcCapacitiveUnitRampTimeDerivative(shifted_time - transition_time, capacitance) / transition_time;
      capacitance_derivative = (calcCapacitiveUnitRampCapDerivative(shifted_time, capacitance)
                                - calcCapacitiveUnitRampCapDerivative(shifted_time - transition_time, capacitance))
                               / transition_time;
    }
  }

  ISTA_RC_HD double calcCapacitiveUnitRampTimeDerivative(double time, double capacitance)
  {
    double time_constant = _driver_resistance * capacitance;
    if (!(time_constant > 0.0)) {
      return 1.0;
    }
    return 1.0 - calcDmpExp(-time / time_constant);
  }

  ISTA_RC_HD double calcCapacitiveUnitRampCapDerivative(double time, double capacitance)
  {
    double time_constant = _driver_resistance * capacitance;
    if (!(time_constant > 0.0)) {
      return 0.0;
    }
    return _driver_resistance * ((1.0 + time / time_constant) * calcDmpExp(-time / time_constant) - 1.0);
  }

  ISTA_RC_HD bool findDriverDelaySlew(double& driver_delay, double& driver_slew)
  {
    double upper_bound = getOutputCrossingUpperBound();
    double lower_crossing = 0.0;
    double upper_crossing = 0.0;
    if (!(upper_bound > _start_time) || !findOutputCrossing(_threshold, _start_time, upper_bound, driver_delay)
        || !findOutputCrossing(_lower_threshold, _start_time, driver_delay, lower_crossing)
        || !findOutputCrossing(_upper_threshold, driver_delay, upper_bound, upper_crossing)) {
      return false;
    }
    driver_slew = (upper_crossing - lower_crossing) / _slew_derate;
    return std::isfinite(driver_delay) && std::isfinite(driver_slew) && driver_slew >= 0.0;
  }

  ISTA_RC_HD bool findOutputCrossing(double threshold, double lower_time, double upper_time, double& crossing_time)
  {
    return findRoot(threshold, lower_time, upper_time, crossing_time);
  }

  ISTA_RC_HD bool findRoot(double threshold, double lower_time, double upper_time, double& root)
  {
    double lower_value = 0.0;
    double upper_value = 0.0;
    double ignored_derivative = 0.0;
    calcOutputWaveform(lower_time, lower_value, ignored_derivative);
    lower_value -= threshold;
    calcOutputWaveform(upper_time, upper_value, ignored_derivative);
    upper_value -= threshold;
    if (!std::isfinite(lower_value) || !std::isfinite(upper_value) || lower_value * upper_value > 0.0) {
      return false;
    }
    if (lower_value == 0.0) {
      root = lower_time;
      return true;
    }
    if (upper_value == 0.0) {
      root = upper_time;
      return true;
    }
    if (lower_value > 0.0) {
      swap(lower_time, upper_time);
    }

    root = 0.5 * (lower_time + upper_time);
    double previous_delta = std::abs(upper_time - lower_time);
    double delta = previous_delta;
    double value = 0.0;
    double derivative = 0.0;
    calcOutputWaveform(root, value, derivative);
    value -= threshold;
    for (int32_t iteration = 0; iteration < kMaxRootIteration; iteration++) {
      bool use_bisection = derivative == 0.0
                            || (((root - upper_time) * derivative - value) * ((root - lower_time) * derivative - value) > 0.0)
                            || std::abs(2.0 * value) > std::abs(previous_delta * derivative);
      if (use_bisection) {
        previous_delta = delta;
        delta = 0.5 * (upper_time - lower_time);
        root = lower_time + delta;
      } else {
        previous_delta = delta;
        delta = value / derivative;
        root -= delta;
      }
      if (std::abs(delta) <= kThresholdTimeTolerance * std::abs(root)) {
        return true;
      }

      calcOutputWaveform(root, value, derivative);
      value -= threshold;
      if (!std::isfinite(value) || !std::isfinite(derivative)) {
        return false;
      }
      if (value < 0.0) {
        lower_time = root;
      } else {
        upper_time = root;
      }
    }
    return false;
  }

  ISTA_RC_HD void calcOutputWaveform(double time, double& voltage, double& derivative)
  {
    double shifted_time = time - _start_time;
    if (shifted_time <= 0.0) {
      voltage = 0.0;
      derivative = 0.0;
      return;
    }

    double unit_voltage = 0.0;
    double unit_derivative = 0.0;
    if (shifted_time <= _transition_time) {
      if (_is_pi) {
        calcPiUnitRamp(shifted_time, unit_voltage, unit_derivative);
      } else {
        calcZeroC2UnitRamp(shifted_time, unit_voltage, unit_derivative);
      }
      voltage = unit_voltage / _transition_time;
      derivative = unit_derivative / _transition_time;
      return;
    }

    double delayed_voltage = 0.0;
    double delayed_derivative = 0.0;
    if (_is_pi) {
      calcPiUnitRamp(shifted_time, unit_voltage, unit_derivative);
      calcPiUnitRamp(shifted_time - _transition_time, delayed_voltage, delayed_derivative);
    } else {
      calcZeroC2UnitRamp(shifted_time, unit_voltage, unit_derivative);
      calcZeroC2UnitRamp(shifted_time - _transition_time, delayed_voltage, delayed_derivative);
    }
    voltage = (unit_voltage - delayed_voltage) / _transition_time;
    derivative = (unit_derivative - delayed_derivative) / _transition_time;
  }

  ISTA_RC_HD void calcPiUnitRamp(double time, double& voltage, double& derivative)
  {
    double exp_pole1 = calcDmpExp(-_pi_pole1 * time);
    double exp_pole2 = calcDmpExp(-_pi_pole2 * time);
    voltage = _pi_scale * (_pi_constant1 + _pi_constant2 * time + _pi_residue1 * exp_pole1 + _pi_residue2 * exp_pole2);
    derivative = _pi_scale
                 * (_pi_constant2 - _pi_residue1 * _pi_pole1 * exp_pole1 - _pi_residue2 * _pi_pole2 * exp_pole2);
  }

  ISTA_RC_HD void calcZeroC2UnitRamp(double time, double& voltage, double& derivative)
  {
    double exp_pole = calcDmpExp(-_zero_pole * time);
    voltage = _zero_scale * (_zero_constant1 + _zero_constant2 * time + _zero_residue * exp_pole);
    derivative = _zero_scale * (_zero_constant2 - _zero_residue * _zero_pole * exp_pole);
  }

  ISTA_RC_HD double getOutputCrossingUpperBound()
  {
    if (_is_pi) {
      return _start_time + _transition_time
             + (_load_capacitance + _driver_capacitance) * (_driver_resistance + _pi_resistance) * 2.0;
    }
    if (_is_zero_c2) {
      return _start_time + _transition_time + _load_capacitance * (_driver_resistance + _pi_resistance) * 2.0;
    }
    return _start_time + _transition_time;
  }

  ISTA_RC_HD double calcDmpExp(double value)
  {
    if (value < -12.0) {
      return 0.0;
    }
    double result = 1.0 + value / 4096.0;
    for (int32_t iteration = 0; iteration < 12; iteration++) {
      result *= result;
    }
    return result;
  }
};
}  // namespace dmp_detail

// Tables supplies raw-load NLDM delay/slew, including unit conversion.
// An absent required table must return NaN; the caller retains native fallback.
template <class Tables>
ISTA_RC_HD CsrRcEvaluationStatus evaluateCsrDmpDriver(
    double driver_capacitance, double resistance, double load_capacitance,
    const CsrDmpThresholds& thresholds, const Tables& tables, CsrDmpDriverResult& output)
{
  output = {};
  if (!std::isfinite(driver_capacitance) || !std::isfinite(resistance) || !std::isfinite(load_capacitance)
      || driver_capacitance < 0.0 || resistance < 0.0 || load_capacitance < 0.0
      || !std::isfinite(thresholds.threshold) || !std::isfinite(thresholds.lower)
      || !std::isfinite(thresholds.upper) || !std::isfinite(thresholds.slew_derate))
    return CsrRcEvaluationStatus::kNumericFailure;
  if (!(thresholds.threshold > 0.0 && thresholds.threshold < 1.0)
      || !(thresholds.lower >= 0.0 && thresholds.upper <= 1.0 && thresholds.upper > thresholds.lower)
      || thresholds.slew_derate <= 0.0)
    return CsrRcEvaluationStatus::kUnavailable;
  dmp_detail::DriverSolver<Tables> solver(driver_capacitance, resistance, load_capacitance, thresholds, tables);
  CsrDmpDriverResult pending;
  const bool completed = solver.run(pending.gate_delay, pending.driver_slew, pending.effective_capacitance);
  if (solver.numericFailure()) return CsrRcEvaluationStatus::kNumericFailure;
  if (!completed) return CsrRcEvaluationStatus::kUnavailable;
  if (!std::isfinite(pending.gate_delay) || !std::isfinite(pending.driver_slew)
      || !std::isfinite(pending.effective_capacitance) || pending.driver_slew < 0.0)
    return CsrRcEvaluationStatus::kNumericFailure;
  output = pending;
  return CsrRcEvaluationStatus::kSuccess;
}

}  // namespace ista::csr
#undef ISTA_RC_HD
