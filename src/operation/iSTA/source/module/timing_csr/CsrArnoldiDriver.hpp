// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include "CsrArnoldiWaveform.hpp"

#if defined(__CUDACC__)
#define ISTA_RC_HD __host__ __device__
#else
#define ISTA_RC_HD
#endif

namespace ista::csr {

// Borrow immutable structure data from the host model or its packed device
// arrays. The owner retains it until the backend finishes this evaluation.
struct CsrArnoldiModelView
{
  int order{0};
  const double* diagonal{nullptr};
  const double* off_diagonal{nullptr};
  const double* source_projection{nullptr};
  double total_capacitance{0.0};
  double sqrt_total_capacitance{0.0};
};

struct CsrArnoldiDriverResult
{
  CsrArnoldiSpectrum spectrum;
  double drive_resistance{0.0};
  double driver_ramp{0.0};
  double effective_capacitance{0.0};
  double table_delay{0.0};
  double reference_time{0.0};
  double gate_delay{0.0};
  double driver_slew{0.0};
};

// The CPU and CUDA table accessors provide delay(C) and slew(C), including
// their native missing-table defaults. All calls use raw load, without ceil.
// This is one driver's live arithmetic; no shared or native answer cache.
template <typename TableLookup>
ISTA_RC_HD CsrRcEvaluationStatus evaluateCsrArnoldiDriver(
    const CsrArnoldiModelView& model, const CsrArnoldiThresholds& thresholds,
    const TableLookup& table, CsrArnoldiDriverResult& output)
{
  output = {};
  if (model.order <= 0 || model.order > 5 || !model.diagonal || !model.source_projection
      || (model.order > 1 && !model.off_diagonal) || model.total_capacitance <= 0.0)
    return CsrRcEvaluationStatus::kUnavailable;
  if (!std::isfinite(model.total_capacitance) || !std::isfinite(model.sqrt_total_capacitance)
      || model.sqrt_total_capacitance <= 0.0 || !std::isfinite(thresholds.slew_derate)
      || thresholds.slew_derate <= 0.0)
    return CsrRcEvaluationStatus::kNumericFailure;
  const double capacitance = model.total_capacitance;
  const double delay_total = table.delay(capacitance);
  const double delay_half = table.delay(0.5 * capacitance);
  const double slew_total = table.slew(capacitance);
  if (!std::isfinite(delay_total) || !std::isfinite(delay_half) || !std::isfinite(slew_total))
    return CsrRcEvaluationStatus::kNumericFailure;
  double delay_resistance = capacitance > 1e-6 ? (delay_total - delay_half) / (0.5 * capacitance) : 0.0;
  if (!(delay_resistance > 0.0)) delay_resistance = 1.0;

  CsrArnoldiDriverResult pending;
  pending.drive_resistance = thresholds.slew_derate * slew_total / (thresholds.voltage_log * capacitance);
  if (delay_resistance > 0.0 && pending.drive_resistance > delay_resistance)
    pending.drive_resistance = delay_resistance;
  if (!(pending.drive_resistance > 0.0 && pending.drive_resistance < 100.0))
    pending.drive_resistance = 1.0;
  const bool bad_resistance = pending.drive_resistance < delay_resistance;
  pending.driver_ramp = csrArnoldiDriverRamp(slew_total, pending.drive_resistance, capacitance, thresholds);
  if (!(pending.driver_ramp > 0.0 && pending.driver_ramp < 100.0))
    pending.driver_ramp = 0.5;
  auto status = evaluateCsrArnoldiSpectrum(model.order, model.diagonal, model.off_diagonal,
      capacitance, pending.drive_resistance, pending.spectrum);
  if (status != CsrRcEvaluationStatus::kSuccess) return status;
  double source_residues[5]{};
  status = evaluateCsrArnoldiResidues(pending.spectrum, model.sqrt_total_capacitance,
                                    model.source_projection, source_residues);
  if (status != CsrRcEvaluationStatus::kSuccess) return status;

  pending.effective_capacitance = capacitance;
  if (!bad_resistance) {
    for (int i = 0; i < 3; ++i) {
      pending.effective_capacitance = arnoldi_detail::effectiveCapacitance(pending.driver_ramp,
          pending.drive_resistance, model.order, pending.spectrum.poles, source_residues, pending.driver_ramp);
      if (!std::isfinite(pending.effective_capacitance) || pending.effective_capacitance < 1e-8)
        pending.effective_capacitance = capacitance;
      const double slew = table.slew(pending.effective_capacitance);
      if (!std::isfinite(slew)) return CsrRcEvaluationStatus::kNumericFailure;
      pending.driver_ramp = csrArnoldiDriverRamp(slew, pending.drive_resistance, pending.effective_capacitance, thresholds);
      if (!(pending.driver_ramp > 0.0 && pending.driver_ramp < 100.0))
        pending.driver_ramp = 0.5;
    }
  }
  pending.table_delay = table.delay(pending.effective_capacitance);
  pending.reference_time = arnoldi_detail::rampTime(
      1.0 / (pending.drive_resistance * pending.effective_capacitance), pending.driver_ramp, 0.5);
  CsrArnoldiCrossings source;
  status = evaluateCsrArnoldiCrossings(pending.driver_ramp, model.order, pending.spectrum.poles,
                                     source_residues, thresholds, source);
  if (status != CsrRcEvaluationStatus::kSuccess) return status;
  pending.gate_delay = source.middle_time + pending.table_delay - pending.reference_time;
  pending.driver_slew = (source.lower_time - source.upper_time) / thresholds.slew_derate;
  if (!std::isfinite(pending.gate_delay) || !std::isfinite(pending.driver_slew))
    return CsrRcEvaluationStatus::kNumericFailure;
  output = pending;
  return CsrRcEvaluationStatus::kSuccess;
}

// A sink consumes the completed driver result; it never overwrites another
// sink or a driver cache. Cross-library threshold adjustment follows this.
ISTA_RC_HD inline CsrRcEvaluationStatus evaluateCsrArnoldiLoad(
    const CsrArnoldiDriverResult& driver, double sqrt_total_capacitance,
    const double* projection, const CsrArnoldiThresholds& thresholds,
    double& wire_delay, double& load_slew)
{
  wire_delay = 0.0;
  load_slew = 0.0;
  double residues[5]{};
  auto status = evaluateCsrArnoldiResidues(driver.spectrum, sqrt_total_capacitance, projection, residues);
  if (status != CsrRcEvaluationStatus::kSuccess) return status;
  CsrArnoldiCrossings times;
  status = evaluateCsrArnoldiCrossings(driver.driver_ramp, driver.spectrum.order,
                                     driver.spectrum.poles, residues, thresholds, times);
  if (status != CsrRcEvaluationStatus::kSuccess) return status;
  const double delay = (times.middle_time + driver.table_delay - driver.reference_time) - driver.gate_delay;
  const double slew = (times.lower_time - times.upper_time) / thresholds.slew_derate;
  if (!std::isfinite(delay) || !std::isfinite(slew))
    return CsrRcEvaluationStatus::kNumericFailure;
  wire_delay = delay;
  load_slew = slew;
  return CsrRcEvaluationStatus::kSuccess;
}

}  // namespace ista::csr
#undef ISTA_RC_HD
