// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <initializer_list>
#include <cmath>
#include <optional>

namespace ista::timing_edit {
struct BufferInsertionGain {
  double delta_capacitance_pf, benefit_ns, penalty_ns, benefit_minus_penalty_ns, gain_ns;
};
// Preserve the existing selector's operation order and positive-gain policy.
inline std::optional<BufferInsertionGain> bufferInsertionGain(
    double upstream_resistance_kohm, double candidate_load_pf, double buffer_capacitance_pf,
    double buffer_resistance_kohm, double buffer_delay_ns)
{
  for (const double value : {upstream_resistance_kohm, candidate_load_pf, buffer_capacitance_pf,
                             buffer_resistance_kohm, buffer_delay_ns})
    if (!std::isfinite(value) || value < 0.0) return std::nullopt;
  BufferInsertionGain result;
  result.delta_capacitance_pf = candidate_load_pf - buffer_capacitance_pf;
  result.benefit_ns = upstream_resistance_kohm * result.delta_capacitance_pf;
  result.penalty_ns = buffer_resistance_kohm * buffer_capacitance_pf;
  result.benefit_minus_penalty_ns = result.benefit_ns - result.penalty_ns;
  result.gain_ns = result.benefit_minus_penalty_ns - buffer_delay_ns;
  for (const double value : {result.delta_capacitance_pf, result.benefit_ns, result.penalty_ns,
                             result.benefit_minus_penalty_ns, result.gain_ns})
    if (!std::isfinite(value)) return std::nullopt;
  return result;
}
}  // namespace ista::timing_edit
