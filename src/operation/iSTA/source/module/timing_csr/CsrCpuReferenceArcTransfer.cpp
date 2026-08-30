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
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "CsrCpuReferenceBackendInternal.hpp"
#include "CsrTableLookup.hpp"

namespace ista::csr {
namespace {

constexpr double kWinnerEpsilon = 1.0e-6;
constexpr std::array<AnalysisMode, kAnalysisCount> kAnalyses{AnalysisMode::kMax, AnalysisMode::kMin};
constexpr std::array<Transition, kTransitionCount> kTransitions{Transition::kRise, Transition::kFall};

[[nodiscard]] bool checkedMultiply(std::size_t lhs, std::size_t rhs, std::size_t& product)
{
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs)
    return false;
  product = lhs * rhs;
  return true;
}

template <typename T>
void resetOptionalSoa(OptionalSoa<T>& soa, std::size_t size, const T& absent_value)
{
  soa.presence.assign(size, 0);
  soa.values.assign(size, absent_value);
}

template <typename T>
void resetRequiredSoa(OptionalSoa<T>& soa, std::size_t size, const T& value)
{
  soa.presence.assign(size, 1);
  soa.values.assign(size, value);
}

[[nodiscard]] bool rawBetter(AnalysisMode analysis, double candidate, double current)
{
  return analysis == AnalysisMode::kMax ? candidate > current : candidate < current;
}

[[nodiscard]] CsrCpuReferenceStageStatus epsilonBetter(AnalysisMode analysis, double candidate, double current, bool& better)
{
  const double threshold = analysis == AnalysisMode::kMax ? current + kWinnerEpsilon : current - kWinnerEpsilon;
  if (!std::isfinite(threshold))
    return CsrCpuReferenceStageStatus::kNumericFailure;
  better = analysis == AnalysisMode::kMax ? candidate > threshold : candidate < threshold;
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] bool hasOutputTable(const ChildTimingArcModel& child, Transition output_transition)
{
  const std::size_t output = csrTransitionIndex(output_transition);
  return child.delay_tables[output].present || child.slew_tables[output].present || child.check_tables[output].present;
}

[[nodiscard]] bool childSelected(const ChildTimingArcModel& child, Transition input_transition, Transition output_transition)
{
  const bool flipped = input_transition != output_transition;
  switch (child.signature.sense) {
    case TimingSense::kNegativeUnate:
      return flipped;
    case TimingSense::kNonUnate:
      return true;
    case TimingSense::kNoneUsesPositiveFallback:
    case TimingSense::kPositiveUnate:
      return !flipped;
  }
  return false;
}

[[nodiscard]] bool usesBothOutputs(const CellTransferModel& transfer)
{
  bool has_negative = false;
  bool has_positive_or_none = false;
  bool has_non_unate = false;
  for (const auto& child : transfer.children) {
    has_negative = has_negative || child.signature.sense == TimingSense::kNegativeUnate;
    has_positive_or_none
        = has_positive_or_none || child.signature.sense == TimingSense::kPositiveUnate || child.signature.sense == TimingSense::kNoneUsesPositiveFallback;
    has_non_unate = has_non_unate || child.signature.sense == TimingSense::kNonUnate;
  }
  return transfer.is_clock_arc || has_non_unate || (has_negative && has_positive_or_none);
}

[[nodiscard]] bool allNegative(const CellTransferModel& transfer)
{
  for (const auto& child : transfer.children)
    if (child.signature.sense != TimingSense::kNegativeUnate)
      return false;
  return true;
}

[[nodiscard]] bool triggerMatches(const CellTransferModel& transfer, Transition input_transition)
{
  if (!transfer.is_clock_arc)
    return true;
  const Transition trigger = transfer.children.front().signature.trigger_transition;
  return input_transition == (trigger == Transition::kFall ? Transition::kFall : Transition::kRise);
}

[[nodiscard]] bool relationExists(const CellTransferModel& transfer, Transition input_transition, Transition output_transition)
{
  if (!triggerMatches(transfer, input_transition))
    return false;
  if (!usesBothOutputs(transfer)) {
    const Transition expected = allNegative(transfer) ? (input_transition == Transition::kRise ? Transition::kFall : Transition::kRise) : input_transition;
    if (output_transition != expected)
      return false;
  }
  for (const auto& child : transfer.children)
    if (hasOutputTable(child, output_transition))
      return true;
  return false;
}

[[nodiscard]] CsrCpuReferenceStageStatus lookupTable(const TimingTableModel& table, double input_slew, double output_load, const ChildTimingArcModel& child,
                                                     double& result)
{
  const auto value = lookupCsrTimingTable(table, input_slew, output_load, child.time_unit_scale, child.capacitance_unit_scale);
  if (!value || !std::isfinite(*value))
    return CsrCpuReferenceStageStatus::kNumericFailure;
  result = *value;
  return CsrCpuReferenceStageStatus::kSuccess;
}

void initializeNet(std::size_t arc, ArcResultSoa& result)
{
  for (AnalysisMode analysis : kAnalyses) {
    for (Transition input_transition : kTransitions) {
      const std::size_t transition_lane = csrArcTransitionLane(arc, analysis, input_transition);
      result.transition_delays.values[transition_lane] = 0.0;
      const std::size_t io_lane = csrArcIoLane(arc, analysis, input_transition, input_transition);
      result.input_output_delays.presence[io_lane] = 1;
      result.input_output_delays.values[io_lane] = 0.0;
      const std::size_t type_lane = csrArcTypeLane(arc, input_transition);
      result.transition_types.presence[type_lane] = 1;
      result.transition_types.values[type_lane] = input_transition;
    }
  }
}

void initializeEmptyCell(std::size_t arc, const CellTransferModel& transfer, ArcResultSoa& result)
{
  for (AnalysisMode analysis : kAnalyses) {
    const double delay = analysis == AnalysisMode::kMax ? transfer.delay_max : transfer.delay_min;
    for (Transition input_transition : kTransitions) {
      const std::size_t transition_lane = csrArcTransitionLane(arc, analysis, input_transition);
      result.transition_delays.values[transition_lane] = delay;
      const std::size_t io_lane = csrArcIoLane(arc, analysis, input_transition, input_transition);
      result.input_output_delays.presence[io_lane] = 1;
      result.input_output_delays.values[io_lane] = delay;
      const std::size_t type_lane = csrArcTypeLane(arc, input_transition);
      result.transition_types.presence[type_lane] = 1;
      result.transition_types.values[type_lane] = input_transition;
    }
  }
}

void writeInitializationCache(std::size_t lane, double candidate, AnalysisMode analysis, ArcResultSoa& result)
{
  if (result.timing_arc_delays.presence[lane] == 0) {
    result.timing_arc_delays.presence[lane] = 1;
    result.timing_arc_delays.values[lane] = candidate;
    result.timing_arc_fallbacks.presence[lane] = 1;
    result.timing_arc_fallbacks.values[lane] = 1;
    return;
  }
  if (rawBetter(analysis, candidate, result.timing_arc_delays.values[lane]))
    result.timing_arc_delays.values[lane] = candidate;
}

[[nodiscard]] CsrCpuReferenceStageStatus initializeNonemptyCell(std::size_t arc_index, const ArcRecord& arc, std::size_t child_begin, ArcResultSoa& result)
{
  if (!arc.output_load)
    return CsrCpuReferenceStageStatus::kInvariantViolation;

  for (AnalysisMode analysis : kAnalyses) {
    for (Transition input_transition : kTransitions) {
      bool transition_winner = false;
      double winning_delay = 0.0;
      Transition winning_output = Transition::kNone;
      for (Transition output_transition : kTransitions) {
        if (!relationExists(arc.transfer, input_transition, output_transition))
          continue;

        bool delay_candidate_seen = false;
        double aggregate_delay = 0.0;
        for (std::size_t child_index = 0; child_index < arc.transfer.children.size(); ++child_index) {
          const auto& child = arc.transfer.children[child_index];
          if (!childSelected(child, input_transition, output_transition))
            continue;
          const TimingTableModel& table = child.delay_tables[csrTransitionIndex(output_transition)];
          if (!table.present)
            continue;

          double candidate = 0.0;
          CsrCpuReferenceStageStatus status
              = lookupTable(table, 0.0, (*arc.output_load)[csrAnalysisIndex(analysis)][csrTransitionIndex(output_transition)], child, candidate);
          if (status != CsrCpuReferenceStageStatus::kSuccess)
            return status;

          const std::size_t cache_lane = csrTimingArcLane(child_begin + child_index, analysis, input_transition, output_transition);
          writeInitializationCache(cache_lane, candidate, analysis, result);
          if (!delay_candidate_seen || rawBetter(analysis, candidate, aggregate_delay)) {
            aggregate_delay = candidate;
            delay_candidate_seen = true;
          }
        }

        if (!delay_candidate_seen)
          aggregate_delay = arc.transfer.delay;
        const std::size_t io_lane = csrArcIoLane(arc_index, analysis, input_transition, output_transition);
        result.input_output_delays.presence[io_lane] = 1;
        result.input_output_delays.values[io_lane] = aggregate_delay;

        if (!transition_winner || rawBetter(analysis, aggregate_delay, winning_delay)) {
          winning_delay = aggregate_delay;
          winning_output = output_transition;
          transition_winner = true;
        }
      }

      if (transition_winner) {
        result.transition_delays.values[csrArcTransitionLane(arc_index, analysis, input_transition)] = winning_delay;
        const std::size_t type_lane = csrArcTypeLane(arc_index, input_transition);
        result.transition_types.presence[type_lane] = 1;
        result.transition_types.values[type_lane] = winning_output;
      }
    }
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

void finalizeScalar(std::size_t arc, ArcResultSoa& result)
{
  double max_delay = result.transition_delays.values[csrArcTransitionLane(arc, AnalysisMode::kMax, Transition::kRise)];
  const double max_fall = result.transition_delays.values[csrArcTransitionLane(arc, AnalysisMode::kMax, Transition::kFall)];
  if (max_fall > max_delay)
    max_delay = max_fall;

  double min_delay = result.transition_delays.values[csrArcTransitionLane(arc, AnalysisMode::kMin, Transition::kRise)];
  const double min_fall = result.transition_delays.values[csrArcTransitionLane(arc, AnalysisMode::kMin, Transition::kFall)];
  if (min_fall < min_delay)
    min_delay = min_fall;

  result.max_delays.values[arc] = max_delay;
  result.min_delays.values[arc] = min_delay;
  result.delays.values[arc] = max_delay;
}

[[nodiscard]] CsrCpuReferenceStageStatus updateDynamicCache(std::size_t lane, double candidate, AnalysisMode analysis, ArcResultSoa& result)
{
  if (result.timing_arc_delays.presence[lane] == 0 || result.timing_arc_fallbacks.presence[lane] == 0)
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  if (result.timing_arc_fallbacks.values[lane] != 0) {
    result.timing_arc_delays.values[lane] = candidate;
    result.timing_arc_fallbacks.values[lane] = 0;
  } else if (rawBetter(analysis, candidate, result.timing_arc_delays.values[lane])) {
    result.timing_arc_delays.values[lane] = candidate;
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus updateGraphDelay(std::size_t lane, AnalysisMode analysis, double candidate, ArcResultSoa& result)
{
  if (result.graph_delays.presence[lane] == 0) {
    result.graph_delays.presence[lane] = 1;
    result.graph_delays.values[lane] = candidate;
    return CsrCpuReferenceStageStatus::kSuccess;
  }

  bool better = false;
  CsrCpuReferenceStageStatus status = epsilonBetter(analysis, candidate, result.graph_delays.values[lane], better);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  if (better)
    result.graph_delays.values[lane] = candidate;
  return CsrCpuReferenceStageStatus::kSuccess;
}

}  // namespace

CsrCpuReferenceStageStatus initializeCsrCpuReferenceArcs(const CsrReferenceInput& input, CsrCpuReferenceWorkState& work)
{
  work.arc_count = input.arcs.size();
  if (work.arc_count == std::numeric_limits<std::size_t>::max()
      || !checkedMultiply(work.arc_count, kAnalysisCount * kTransitionCount, work.arc_transition_lane_count)
      || !checkedMultiply(work.arc_transition_lane_count, kTransitionCount, work.arc_io_lane_count))
    return CsrCpuReferenceStageStatus::kInvariantViolation;

  work.timing_arc_count = 0;
  for (const auto& arc : input.arcs) {
    if (arc.transfer.children.size() > std::numeric_limits<std::size_t>::max() - work.timing_arc_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    work.timing_arc_count += arc.transfer.children.size();
  }
  if (!checkedMultiply(work.timing_arc_count, kAnalysisCount * kTransitionCount * kTransitionCount, work.timing_arc_lane_count))
    return CsrCpuReferenceStageStatus::kInvariantViolation;

  auto& result = work.result.arcs;
  resetRequiredSoa(result.delays, work.arc_count, 0.0);
  resetRequiredSoa(result.min_delays, work.arc_count, 0.0);
  resetRequiredSoa(result.max_delays, work.arc_count, 0.0);
  resetRequiredSoa(result.transition_delays, work.arc_transition_lane_count, 0.0);
  resetOptionalSoa(result.input_output_delays, work.arc_io_lane_count, 0.0);
  resetOptionalSoa(result.graph_delays, work.arc_io_lane_count, 0.0);
  resetOptionalSoa(result.transition_types, work.arc_count * kTransitionCount, Transition::kNone);
  result.timing_arc_row_offsets.assign(work.arc_count + 1, 0);
  result.timing_arc_indices.clear();
  result.timing_arc_indices.reserve(work.timing_arc_count);
  resetOptionalSoa(result.timing_arc_delays, work.timing_arc_lane_count, 0.0);
  resetOptionalSoa(result.timing_arc_fallbacks, work.timing_arc_lane_count, std::uint8_t{0});

  std::size_t child_offset = 0;
  for (std::size_t arc_index = 0; arc_index < input.arcs.size(); ++arc_index) {
    const auto& arc = input.arcs[arc_index];
    result.timing_arc_row_offsets[arc_index] = child_offset;
    for (const auto& child : arc.transfer.children)
      result.timing_arc_indices.push_back(child.signature.arc_index);

    CsrCpuReferenceStageStatus status = CsrCpuReferenceStageStatus::kSuccess;
    if (arc.kind == ArcKind::kNet) {
      initializeNet(arc_index, result);
    } else if (!arc.output_load) {
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    } else if (arc.transfer.children.empty()) {
      initializeEmptyCell(arc_index, arc.transfer, result);
    } else {
      status = initializeNonemptyCell(arc_index, arc, child_offset, result);
    }
    if (status != CsrCpuReferenceStageStatus::kSuccess)
      return status;
    finalizeScalar(arc_index, result);
    child_offset += arc.transfer.children.size();
  }
  result.timing_arc_row_offsets[work.arc_count] = child_offset;
  return child_offset == work.timing_arc_count ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kInvariantViolation;
}

CsrCpuReferenceStageStatus evaluateCsrDynamicArc(const CsrReferenceInput& input, ArcId arc_id, AnalysisMode analysis, Transition input_transition,
                                                 Transition output_transition, double source_slew, CsrCpuReferenceWorkState& work, CsrDynamicArcValue& value)
{
  value = {};
  if (!std::isfinite(source_slew) || source_slew < 0.0)
    return CsrCpuReferenceStageStatus::kNumericFailure;
  if (arc_id.value() >= work.arc_count || arc_id.value() >= input.arcs.size())
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  const std::size_t arc_index = static_cast<std::size_t>(arc_id.value());
  const auto& arc = input.arcs[arc_index];
  const std::size_t io_lane = csrArcIoLane(arc_index, analysis, input_transition, output_transition);
  if (work.result.arcs.input_output_delays.presence[io_lane] == 0)
    return CsrCpuReferenceStageStatus::kSuccess;

  value.present = true;
  value.delay = work.result.arcs.input_output_delays.values[io_lane];
  value.slew = source_slew;
  if (!arc.transfer.children.empty()) {
    if (!arc.output_load)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    bool delay_seen = false;
    bool slew_seen = false;
    double aggregate_delay = 0.0;
    double aggregate_slew = 0.0;
    const std::size_t child_begin = work.result.arcs.timing_arc_row_offsets[arc_index];
    for (std::size_t child_index = 0; child_index < arc.transfer.children.size(); ++child_index) {
      const auto& child = arc.transfer.children[child_index];
      if (!childSelected(child, input_transition, output_transition))
        continue;
      const double output_load = (*arc.output_load)[csrAnalysisIndex(analysis)][csrTransitionIndex(output_transition)];
      const TimingTableModel& delay_table = child.delay_tables[csrTransitionIndex(output_transition)];
      if (delay_table.present) {
        double candidate = 0.0;
        CsrCpuReferenceStageStatus status = lookupTable(delay_table, source_slew, output_load, child, candidate);
        if (status != CsrCpuReferenceStageStatus::kSuccess)
          return status;
        status = updateDynamicCache(csrTimingArcLane(child_begin + child_index, analysis, input_transition, output_transition), candidate, analysis,
                                    work.result.arcs);
        if (status != CsrCpuReferenceStageStatus::kSuccess)
          return status;
        if (!delay_seen || rawBetter(analysis, candidate, aggregate_delay)) {
          aggregate_delay = candidate;
          delay_seen = true;
        }
      }

      const TimingTableModel& slew_table = child.slew_tables[csrTransitionIndex(output_transition)];
      if (slew_table.present) {
        double candidate = 0.0;
        CsrCpuReferenceStageStatus status = lookupTable(slew_table, source_slew, output_load, child, candidate);
        if (status != CsrCpuReferenceStageStatus::kSuccess || candidate < 0.0)
          return CsrCpuReferenceStageStatus::kNumericFailure;
        if (!slew_seen || rawBetter(analysis, candidate, aggregate_slew)) {
          aggregate_slew = candidate;
          slew_seen = true;
        }
      }
    }
    value.delay = delay_seen ? aggregate_delay : arc.transfer.delay;
    value.slew = slew_seen ? aggregate_slew : source_slew;
  }

  if (!std::isfinite(value.delay) || !std::isfinite(value.slew) || value.slew < 0.0)
    return CsrCpuReferenceStageStatus::kNumericFailure;
  CsrCpuReferenceStageStatus status = updateGraphDelay(io_lane, analysis, value.delay, work.result.arcs);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  value.delay = work.result.arcs.graph_delays.values[io_lane];
  return CsrCpuReferenceStageStatus::kSuccess;
}

}  // namespace ista::csr
