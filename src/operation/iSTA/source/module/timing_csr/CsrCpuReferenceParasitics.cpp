// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrCpuReferenceParasitics.hpp"
#include "CsrTableLookup.hpp"
#include "CsrDmpLoad.hpp"
#include "CsrParasiticThreshold.hpp"

#include <cmath>
#include <limits>

namespace ista::csr {
namespace {
struct RawTables
{
  const ChildTimingArcModel& child;
  std::size_t output;
  double input_slew;

  double lookup(const TimingTableModel& table, double load, double absent) const
  {
    if (!table.present) return absent;
    return lookupCsrTimingTableRawLoad(table, input_slew, load, child.time_unit_scale,
        child.capacitance_unit_scale).value_or(std::numeric_limits<double>::quiet_NaN());
  }
  double delay(double load) const { return lookup(child.delay_tables[output], load, 0.0); }
  double slew(double load) const { return lookup(child.slew_tables[output], load, input_slew); }
};

CsrRcEvaluationStatus values(const CsrRcTimingRecord& record, double& delay, double& slew)
{
  if (record.family == CsrRcDriverFamily::arnoldi) {
    delay = record.arnoldi.gate_delay;
    slew = record.arnoldi.driver_slew;
  } else if (record.family == CsrRcDriverFamily::dmp) {
    delay = record.dmp.gate_delay;
    slew = record.dmp.driver_slew;
  } else {
    return CsrRcEvaluationStatus::kUnavailable;
  }
  return CsrRcEvaluationStatus::kSuccess;
}
}  // namespace

CsrRcEvaluationStatus evaluateCsrCpuRcChild(
    const CsrParasiticBinding& binding, const CsrCpuRcChildKey& key,
    const ChildTimingArcModel& child, double input_slew, CsrCpuRcCache& cache,
    double& delay, double& slew)
{
  if (!binding.tree) return CsrRcEvaluationStatus::kUnavailable;
  const auto [driver, instance_copy, ordinal, child_index, analysis, transition] = key;
  (void) instance_copy;
  (void) ordinal;
  (void) child_index;
  if (binding.terminal != 0 || binding.tree->terminals.empty()
      || binding.tree->terminals.front() != driver
      || !std::isfinite(input_slew) || input_slew < 0.0
      || (transition != Transition::kRise && transition != Transition::kFall))
    return CsrRcEvaluationStatus::kNumericFailure;
  const bool negative_zero = input_slew == 0.0 && std::signbit(input_slew);
  const CsrCpuRcTimingKey timing_key{key, input_slew, negative_zero};
  auto found = cache.timing.find(timing_key);
  // Native Arnoldi's double key equates signed zero; native DMP's string key
  // distinguishes them. A shared Arnoldi hit is read-only.
  if (found == cache.timing.end() && input_slew == 0.0) {
    const auto other = cache.timing.find({key, input_slew, !negative_zero});
    if (other != cache.timing.end() && other->second.timing.family == CsrRcDriverFamily::arnoldi)
      found = other;
  }
  if (found != cache.timing.end()) {
    if (found->second.tree != binding.tree) return CsrRcEvaluationStatus::kNumericFailure;
    return values(found->second.timing, delay, slew);
  }
  const auto output = transition == Transition::kRise ? 0U : 1U;
  const auto lane = (analysis == AnalysisMode::kMax ? 0U : 2U) + output;
  const auto& model_owner = binding.tree->lanes[lane];
  if (!model_owner) return CsrRcEvaluationStatus::kNumericFailure;
  const auto& model = *model_owner;
  const auto& arnoldi = model.arnoldi;
  const RawTables tables{child, output, input_slew};
  CsrCpuRcCachedTiming pending;
  pending.child = &child;
  pending.tree = binding.tree;
  pending.timing.input_slew = input_slew;
  const CsrArnoldiModelView view{
      arnoldi.order, arnoldi.diagonal.data(), arnoldi.off_diagonal.data(),
      arnoldi.projection.empty() ? nullptr : arnoldi.projection.front().data(),
      arnoldi.total_capacitance, arnoldi.sqrt_total_capacitance};
  auto status = evaluateCsrArnoldiDriver(view,
      csrArnoldiThresholds(child.slew_lower_threshold_pct[output],
          child.slew_upper_threshold_pct[output], child.slew_derate),
      tables, pending.timing.arnoldi);
  if (status == CsrRcEvaluationStatus::kNumericFailure) return status;
  if (status == CsrRcEvaluationStatus::kSuccess) {
    pending.timing.family = CsrRcDriverFamily::arnoldi;
  } else if (model.dmp.valid && child.delay_tables[output].present && child.slew_tables[output].present) {
    status = evaluateCsrDmpDriver(model.dmp.driver_capacitance, model.dmp.resistance,
        model.dmp.load_capacitance,
        csrDmpThresholds(child.output_threshold_pct[output], child.slew_lower_threshold_pct[output],
            child.slew_upper_threshold_pct[output], child.slew_derate),
        tables, pending.timing.dmp);
    if (status == CsrRcEvaluationStatus::kNumericFailure) return status;
    if (status == CsrRcEvaluationStatus::kSuccess) pending.timing.family = CsrRcDriverFamily::dmp;
  }
  if (pending.timing.family != CsrRcDriverFamily::unavailable) {
    status = values(pending.timing, delay, slew);
    if (!std::isfinite(delay) || !std::isfinite(slew) || slew < 0.0)
      return CsrRcEvaluationStatus::kNumericFailure;
  }
  const auto& record = cache.timing.emplace(timing_key, std::move(pending)).first->second;
  // No per-sink evaluation or load-map copies here. The net worker evaluates
  // its terminal from this completed immutable driver response.
  if (record.timing.family == CsrRcDriverFamily::arnoldi)
    cache.arnoldi_drivers.insert_or_assign({driver, analysis, transition, slew, false}, &record);
  else if (record.timing.family == CsrRcDriverFamily::dmp)
    cache.dmp_drivers.insert_or_assign(
        {driver, analysis, transition, slew, slew == 0.0 && std::signbit(slew)}, &record);
  return values(record.timing, delay, slew);
}
CsrRcEvaluationStatus evaluateCsrCpuCellTiming(
    const CsrParasiticBinding& binding, const CsrCpuRcChildKey& key,
    const ChildTimingArcModel& child, double input_slew, double no_rc_load,
    bool slew, CsrCpuRcCache& cache, double& result)
{
  const auto output = std::get<5>(key) == Transition::kRise ? 0U : 1U;
  double load = no_rc_load;
  if (binding.tree) {
    double delay_value = 0.0, slew_value = 0.0;
    const auto status = evaluateCsrCpuRcChild(binding, key, child, input_slew, cache, delay_value, slew_value);
    if (status == CsrRcEvaluationStatus::kNumericFailure) return status;
    if (status == CsrRcEvaluationStatus::kSuccess) {
      result = slew ? slew_value : delay_value;
      return status;
    }
    const auto lane = (std::get<4>(key) == AnalysisMode::kMax ? 0U : 2U) + output;
    load = binding.tree->lanes[lane]->root_load;
  }
  const auto& table = slew ? child.slew_tables[output] : child.delay_tables[output];
  const auto value = table.present
      ? lookupCsrTimingTable(table, input_slew, load, child.time_unit_scale, child.capacitance_unit_scale)
      : std::optional<double>{slew ? input_slew : 0.0};
  if (!value || !std::isfinite(*value) || (slew && *value < 0.0))
    return CsrRcEvaluationStatus::kNumericFailure;
  result = *value;
  return CsrRcEvaluationStatus::kSuccess;
}
CsrRcEvaluationStatus evaluateCsrCpuRcLoad(
    const CsrParasiticBinding& binding, AnalysisMode analysis, Transition transition,
    double input_slew, const ReferencedLibraryCellModel* load_cell,
    const CsrCpuRcCache& cache, double& delay, double& slew)
{
  if (!binding.tree) return CsrRcEvaluationStatus::kUnavailable;
  if (binding.terminal >= binding.tree->terminals.size() || !std::isfinite(input_slew) || input_slew < 0.0
      || (transition != Transition::kRise && transition != Transition::kFall))
    return CsrRcEvaluationStatus::kNumericFailure;
  const auto output = transition == Transition::kRise ? 0U : 1U;
  const auto lane = (analysis == AnalysisMode::kMax ? 0U : 2U) + output;
  const auto& model_owner = binding.tree->lanes[lane];
  if (!model_owner) return CsrRcEvaluationStatus::kNumericFailure;
  const auto& model = *model_owner;
  CsrCpuRcDriverKey key{binding.tree->terminals.front(), analysis, transition, input_slew, false};
  const CsrCpuRcCachedTiming* selected = nullptr;
  const auto arnoldi = cache.arnoldi_drivers.find(key);
  const auto status_for = [&](const CsrCpuRcCachedTiming& record, bool use_arnoldi) {
    if (record.tree != binding.tree || record.child == nullptr)
      return CsrRcEvaluationStatus::kNumericFailure;
    const auto& child = *record.child;
    if (use_arnoldi) {
      if (binding.terminal >= model.arnoldi.projection.size())
        return CsrRcEvaluationStatus::kNumericFailure;
      return evaluateCsrArnoldiLoad(record.timing.arnoldi, model.arnoldi.sqrt_total_capacitance,
          model.arnoldi.projection[binding.terminal].data(),
          csrArnoldiThresholds(child.slew_lower_threshold_pct[output], child.slew_upper_threshold_pct[output], child.slew_derate),
          delay, slew);
    }
    if (binding.terminal == 0) {
      delay = 0.0;
      slew = record.timing.dmp.driver_slew;
      return CsrRcEvaluationStatus::kSuccess;
    }
    if (binding.terminal >= model.dmp.loads.size()) return CsrRcEvaluationStatus::kNumericFailure;
    const auto& load = model.dmp.loads[binding.terminal];
    return evaluateCsrDmpLoad(load.order, load.poles.data(), load.residues.data(), input_slew,
        csrDmpThresholds(child.output_threshold_pct[output], child.slew_lower_threshold_pct[output],
            child.slew_upper_threshold_pct[output], child.slew_derate), delay, slew);
  };
  auto status = CsrRcEvaluationStatus::kUnavailable;
  if (arnoldi != cache.arnoldi_drivers.end()) {
    selected = arnoldi->second;
    status = status_for(*selected, true);
  }
  if (status == CsrRcEvaluationStatus::kUnavailable) {
    std::get<4>(key) = input_slew == 0.0 && std::signbit(input_slew);
    const auto dmp = cache.dmp_drivers.find(key);
    if (dmp != cache.dmp_drivers.end()) {
      selected = dmp->second;
      status = status_for(*selected, false);
    }
  }
  if (status != CsrRcEvaluationStatus::kSuccess) return status;
  if (binding.terminal != 0 && load_cell && load_cell->present
      && load_cell->library_name != selected->child->signature.library_name) {
    const auto& child = *selected->child;
    const auto normalize = [](double value) { return value > 1.0 ? value * 0.01 : value; };
    status = adjustCsrParasiticLoadThreshold(
        {normalize(child.slew_lower_threshold_pct[output]), normalize(child.slew_upper_threshold_pct[output]),
         normalize(child.output_threshold_pct[output]), child.slew_derate},
        {normalize(load_cell->slew_lower_threshold_pct[output]), normalize(load_cell->slew_upper_threshold_pct[output]),
         normalize(load_cell->input_threshold_pct[output]), load_cell->slew_derate},
        transition == Transition::kFall, delay, slew);
  }
  return status;
}
CsrRcEvaluationStatus evaluateCsrCpuNetTiming(
    const CsrParasiticBinding& binding, AnalysisMode analysis, Transition transition,
    double input_slew, bool input_port, const ReferencedLibraryCellModel* load_cell,
    const InputPortRcThresholds& thresholds, const CsrCpuRcCache* cache,
    double& delay, double& slew)
{
  delay = 0.0;
  slew = input_slew;
  if (!binding.tree) return CsrRcEvaluationStatus::kSuccess;
  if (binding.terminal >= binding.tree->terminals.size() || !std::isfinite(input_slew)
      || (transition != Transition::kRise && transition != Transition::kFall))
    return CsrRcEvaluationStatus::kNumericFailure;
  if (cache) {
    const auto status = evaluateCsrCpuRcLoad(binding, analysis, transition, input_slew,
        load_cell, *cache, delay, slew);
    if (status != CsrRcEvaluationStatus::kUnavailable) return status;
  }
  const auto output = transition == Transition::kRise ? 0U : 1U;
  const auto lane = (analysis == AnalysisMode::kMax ? 0U : 2U) + output;
  if (!binding.tree->lanes[lane]) return CsrRcEvaluationStatus::kNumericFailure;
  const auto& model = *binding.tree->lanes[lane];
  if (input_port && model.arnoldi.order > 0) {
    const auto& arnoldi = model.arnoldi;
    if (binding.terminal >= arnoldi.projection.size())
      return CsrRcEvaluationStatus::kNumericFailure;
    const CsrArnoldiModelView view{
        arnoldi.order, arnoldi.diagonal.data(), arnoldi.off_diagonal.data(),
        arnoldi.projection.front().data(), arnoldi.total_capacitance, arnoldi.sqrt_total_capacitance};
    const auto status = evaluateCsrArnoldiInputPort(view, arnoldi.projection[binding.terminal].data(),
        csrArnoldiThresholds(thresholds.lower[output], thresholds.upper[output], thresholds.slew_derate),
        input_slew, delay, slew);
    if (status != CsrRcEvaluationStatus::kUnavailable) return status;
  }
  if (binding.terminal >= model.fallback.size()) return CsrRcEvaluationStatus::kNumericFailure;
  const auto& fallback = model.fallback[binding.terminal];
  if (!fallback.valid) return CsrRcEvaluationStatus::kNumericFailure;
  delay = fallback.delay;
  slew = std::sqrt(input_slew * input_slew + fallback.impulse);
  if (input_slew < 0.0) slew = -slew;
  return std::isfinite(delay) && std::isfinite(slew)
      ? CsrRcEvaluationStatus::kSuccess : CsrRcEvaluationStatus::kNumericFailure;
}
}  // namespace ista::csr
