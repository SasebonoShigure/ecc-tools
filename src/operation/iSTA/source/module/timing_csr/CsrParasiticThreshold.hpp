// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include "CsrArnoldiDriver.hpp"

#if defined(__CUDACC__)
#define ISTA_RC_HD __host__ __device__
#else
#define ISTA_RC_HD
#endif

namespace ista::csr {

// The caller skips absent load cells and identical driver/load libraries.
// Percentages are normalized at the input boundary, preserving native *0.01.
struct CsrRcPinThresholds
{
  double lower{0.3}, upper{0.7}, threshold{0.5}, slew_derate{1.0};
};

ISTA_RC_HD inline CsrRcEvaluationStatus adjustCsrParasiticLoadThreshold(
    const CsrRcPinThresholds& driver, const CsrRcPinThresholds& load,
    bool falling, double& wire_delay, double& load_slew)
{
  const double driver_span = driver.upper - driver.lower;
  const double load_span = load.upper - load.lower;
  if (!(driver_span > 1e-6 && load_span > 1e-6
        && driver.slew_derate > 1e-6 && load.slew_derate > 1e-6))
    return CsrRcEvaluationStatus::kSuccess;
  const double adjustment = load_slew * ((load.threshold - driver.threshold) / driver_span);
  const double delay = falling ? wire_delay - adjustment : wire_delay + adjustment;
  const double slew = load_slew * ((load_span / load.slew_derate) / (driver_span / driver.slew_derate));
  if (!std::isfinite(delay) || !std::isfinite(slew))
    return CsrRcEvaluationStatus::kNumericFailure;
  wire_delay = delay;
  load_slew = slew;
  return CsrRcEvaluationStatus::kSuccess;
}

// Native input ports use the global TimingLibrary thresholds, with no cell
// driver waveform. This is the native reduced-model first moment, not a
// replacement for cell-driven Arnoldi/DMP evaluation.
ISTA_RC_HD inline CsrRcEvaluationStatus evaluateCsrArnoldiInputPort(
    const CsrArnoldiModelView& model, const double* terminal_projection,
    const CsrArnoldiThresholds& thresholds, double input_slew,
    double& wire_delay, double& load_slew)
{
  if (model.order <= 0 || model.order > 5 || !model.diagonal
      || !model.source_projection || !terminal_projection)
    return CsrRcEvaluationStatus::kUnavailable;
  double moment = model.diagonal[0];
  if (model.order > 1 && model.off_diagonal && std::abs(model.source_projection[0]) >= 1e-6)
    moment += model.off_diagonal[0] * terminal_projection[1] / model.source_projection[0];
  const double delay = std::log(2.0) * moment;
  double slew = std::abs(input_slew) + (thresholds.voltage_log / thresholds.slew_derate) * moment;
  if (input_slew < 0.0) slew = -slew;
  if (!std::isfinite(delay) || !std::isfinite(slew))
    return CsrRcEvaluationStatus::kNumericFailure;
  wire_delay = delay;
  load_slew = slew;
  return CsrRcEvaluationStatus::kSuccess;
}

}  // namespace ista::csr
#undef ISTA_RC_HD
