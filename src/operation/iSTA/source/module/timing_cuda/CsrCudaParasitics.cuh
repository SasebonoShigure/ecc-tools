#pragma once

#include "CsrCudaTableLookup.cuh"
#include "CsrRcDriverCache.hpp"
#include "CsrDmpLoad.hpp"
#include "CsrParasiticThreshold.hpp"

namespace ista::csr::cuda_detail {

struct RcRawTables
{
  DeviceView view;
  const Child& child;
  unsigned output;
  double input_slew;

  __device__ double delay(double load) const
  {
    return child.delay[output].present
        ? lookupTableRawLoad(view, child.delay[output], child, input_slew, load) : 0.0;
  }
  __device__ double slew(double load) const
  {
    return child.slew[output].present
        ? lookupTableRawLoad(view, child.slew[output], child, input_slew, load) : input_slew;
  }
};

__device__ inline CsrArnoldiModelView arnoldiView(RcModelView source)
{
  const auto& model = *source.model;
  return {model.order, model.diagonal, model.off_diagonal,
          source.terminals[model.terminal_begin].projection,
          model.total_capacitance, model.sqrt_total_capacitance};
}

// Only the driver owner calls this on a miss. It returns a complete local
// record; the caller publishes it after status/capacity checks.
__device__ inline CsrRcEvaluationStatus evaluateRcDriver(
    DeviceView view, RcModelView source, Index child_index,
    unsigned output, double input_slew, CsrRcTimingRecord& pending)
{
  pending = {};
  if (!source.model || !source.terminals || !source.model->terminal_count
      || child_index >= view.child_count || output >= 2
      || !isfinite(input_slew) || input_slew < 0.0)
    return CsrRcEvaluationStatus::kNumericFailure;
  const auto& model = *source.model;
  const auto& child = view.children[child_index];
  pending.input_slew = input_slew;
  pending.child_model = child_index;  // Payload only; this is not cache identity.
  const RcRawTables tables{view, child, output, input_slew};
  auto status = evaluateCsrArnoldiDriver(arnoldiView(source),
      csrArnoldiThresholds(child.lower[output], child.upper[output], child.slew_derate),
      tables, pending.arnoldi);
  if (*view.status != Status::success || status == CsrRcEvaluationStatus::kNumericFailure)
    return CsrRcEvaluationStatus::kNumericFailure;
  if (status == CsrRcEvaluationStatus::kSuccess) {
    pending.family = CsrRcDriverFamily::arnoldi;
  } else if (model.dmp_valid && child.delay[output].present && child.slew[output].present) {
    status = evaluateCsrDmpDriver(model.driver_capacitance, model.resistance, model.load_capacitance,
        csrDmpThresholds(child.output_threshold[output], child.lower[output], child.upper[output], child.slew_derate),
        tables, pending.dmp);
    if (*view.status != Status::success || status == CsrRcEvaluationStatus::kNumericFailure)
      return CsrRcEvaluationStatus::kNumericFailure;
    if (status == CsrRcEvaluationStatus::kSuccess)
      pending.family = CsrRcDriverFamily::dmp;
  }
  if (pending.family == CsrRcDriverFamily::unavailable)
    return CsrRcEvaluationStatus::kUnavailable;
  const double delay = pending.family == CsrRcDriverFamily::arnoldi ? pending.arnoldi.gate_delay : pending.dmp.gate_delay;
  const double slew = pending.family == CsrRcDriverFamily::arnoldi ? pending.arnoldi.driver_slew : pending.dmp.driver_slew;
  return isfinite(delay) && isfinite(slew) && slew >= 0.0
      ? CsrRcEvaluationStatus::kSuccess : CsrRcEvaluationStatus::kNumericFailure;
}

// Row identity is prepared from driver/native-child/analysis/output, separately
// from immutable child_model. One driver worker owns all its rows and sequence.
__device__ inline const CsrRcTimingRecord* cachedRcDriver(
    DeviceView view, RcModelView source, Index child_index, unsigned output, double input_slew,
    CsrRcTimingRow& row, CsrRcDriverLane& driver, CsrRcTimingRecord* records)
{
  if (!isfinite(input_slew) || input_slew < 0.0) {
    numeric(view);
    return nullptr;
  }
  if (row.size > row.capacity) {
    *view.status = Status::invariant;
    return nullptr;
  }
  auto index = findCsrRcTimingRecord(records, row, input_slew);
  if (index != kCsrRcAbsent) {
    if (records[index].child_model != child_index) {
      *view.status = Status::invariant;
      return nullptr;
    }
    return records + index;
  }
  CsrRcTimingRecord pending;
  const auto status = evaluateRcDriver(view, source, child_index, output, input_slew, pending);
  if (status == CsrRcEvaluationStatus::kNumericFailure) {
    numeric(view);
    return nullptr;
  }
  if (!appendCsrRcTimingRecord(records, row, driver, pending, index)) {
    *view.status = Status::invariant;
    return nullptr;
  }
  return records + index;
}

__device__ inline double rcCellValue(
    DeviceView view, RcModelView source, const CsrRcTimingRecord& record,
    unsigned output, bool slew)
{
  if (record.family == CsrRcDriverFamily::arnoldi)
    return slew ? record.arnoldi.driver_slew : record.arnoldi.gate_delay;
  if (record.family == CsrRcDriverFamily::dmp)
    return slew ? record.dmp.driver_slew : record.dmp.gate_delay;
  // Model-unavailable keeps ordinary NLDM's DFS root load and ceil semantics.
  const auto& child = view.children[record.child_model];
  const auto& table = slew ? child.slew[output] : child.delay[output];
  return table.present
      ? lookupTable(view, table, child, record.input_slew, source.model->root_load)
      : (slew ? record.input_slew : 0.0);
}

// Sinks borrow a completed driver response. No cache mutation or fanout map.
__device__ inline CsrRcEvaluationStatus evaluateRcLoad(
    RcModelView source, Index terminal, const CsrRcTimingRecord& record,
    const Child& child, unsigned output, double input_slew, double& delay, double& slew)
{
  if (!source.model || !source.terminals || terminal >= source.model->terminal_count
      || output >= 2 || !isfinite(input_slew) || input_slew < 0.0)
    return CsrRcEvaluationStatus::kNumericFailure;
  const auto& model = *source.model;
  const auto& load = source.terminals[model.terminal_begin + terminal];
  if (record.family == CsrRcDriverFamily::arnoldi)
    return evaluateCsrArnoldiLoad(record.arnoldi, model.sqrt_total_capacitance, load.projection,
        csrArnoldiThresholds(child.lower[output], child.upper[output], child.slew_derate), delay, slew);
  if (record.family == CsrRcDriverFamily::dmp) {
    if (terminal == 0) {
      delay = 0.0;
      slew = record.dmp.driver_slew;
      return CsrRcEvaluationStatus::kSuccess;
    }
    return evaluateCsrDmpLoad(load.dmp_order, load.poles, load.residues, input_slew,
        csrDmpThresholds(child.output_threshold[output], child.lower[output], child.upper[output], child.slew_derate),
        delay, slew);
  }
  return CsrRcEvaluationStatus::kUnavailable;
}

__device__ inline CsrRcEvaluationStatus evaluateRcNetFallback(
    RcModelView source, Index terminal, bool input_port, const CsrArnoldiThresholds& thresholds,
    double input_slew, double& delay, double& slew)
{
  if (!source.model || !source.terminals || terminal >= source.model->terminal_count || !isfinite(input_slew))
    return CsrRcEvaluationStatus::kNumericFailure;
  const auto& model = *source.model;
  const auto& load = source.terminals[model.terminal_begin + terminal];
  if (input_port && model.order > 0) {
    const auto status = evaluateCsrArnoldiInputPort(arnoldiView(source), load.projection, thresholds,
        input_slew, delay, slew);
    if (status != CsrRcEvaluationStatus::kUnavailable)
      return status;
  }
  if (!load.fallback_valid)
    return CsrRcEvaluationStatus::kNumericFailure;
  delay = load.fallback_delay;
  slew = sqrt(input_slew * input_slew + load.fallback_impulse);
  if (input_slew < 0.0) slew = -slew;
  return isfinite(delay) && isfinite(slew)
      ? CsrRcEvaluationStatus::kSuccess : CsrRcEvaluationStatus::kNumericFailure;
}

}  // namespace ista::csr::cuda_detail
