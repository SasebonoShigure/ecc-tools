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
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>

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

[[nodiscard]] bool usesBothOutputs(const CellTransferModel& transfer)
{
  bool has_negative = false;
  bool has_positive_or_none = false;
  bool has_non_unate = false;
  for (const auto& child : transfer.children()) {
    has_negative = has_negative || child.signature.sense == TimingSense::kNegativeUnate;
    has_positive_or_none
        = has_positive_or_none || child.signature.sense == TimingSense::kPositiveUnate || child.signature.sense == TimingSense::kNoneUsesPositiveFallback;
    has_non_unate = has_non_unate || child.signature.sense == TimingSense::kNonUnate;
  }
  return transfer.is_clock_arc || has_non_unate || (has_negative && has_positive_or_none);
}

[[nodiscard]] bool allNegative(const CellTransferModel& transfer)
{
  for (const auto& child : transfer.children())
    if (child.signature.sense != TimingSense::kNegativeUnate)
      return false;
  return true;
}

[[nodiscard]] bool triggerMatches(const CellTransferModel& transfer, Transition input_transition)
{
  if (!transfer.is_clock_arc)
    return true;
  const Transition trigger = transfer.children().front().signature.trigger_transition;
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
  for (const auto& child : transfer.children())
    if (hasOutputTable(child, output_transition))
      return true;
  return false;
}

[[nodiscard]] CsrCpuReferenceStageStatus lookupChild(
    const ArcSemanticPayload& arc, std::size_t child_index, AnalysisMode analysis,
    Transition output, double input_slew, bool slew, CsrCpuReferenceWorkState& work, double& result)
{
  if ((!arc.output_load && !arc.parasitic.tree) || (arc.parasitic.tree && !arc.native_cell_arc_ordinal))
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  const auto& child = arc.transfer.children()[child_index];
  const double load = arc.output_load ? (*arc.output_load)[csrAnalysisIndex(analysis)][csrTransitionIndex(output)] : 0.0;
  const CsrCpuRcChildKey key{arc.sink, false, arc.native_cell_arc_ordinal.value_or(0), child_index, analysis, output};
  return evaluateCsrCpuCellTiming(arc.parasitic, key, child, input_slew, load, slew, work.rc, result)
             == CsrRcEvaluationStatus::kSuccess
         ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kNumericFailure;
}

[[nodiscard]] CsrCpuReferenceStageStatus netTiming(
    const ArcSemanticPayload& arc, AnalysisMode analysis, Transition transition,
    double source_slew, bool initializing, const CsrCpuReferenceWorkState& work,
    double& delay, double& slew)
{
  delay = 0.0;
  slew = source_slew;
  if (!arc.parasitic.tree) return CsrCpuReferenceStageStatus::kSuccess;
  if (arc.source.value() >= work.rc_nodes.size() || arc.sink.value() >= work.rc_nodes.size())
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  const auto& sink = work.rc_nodes[arc.sink.value()];
  const ReferencedLibraryCellModel* load_cell = nullptr;
  if (!sink.is_port) {
    const auto found = work.rc_cells.find(sink.instance_cell);
    if (found != work.rc_cells.end()) load_cell = found->second;
  }
  return evaluateCsrCpuNetTiming(arc.parasitic, analysis, transition, source_slew,
      work.rc_nodes[arc.source.value()].is_port, load_cell, work.input_port_thresholds,
      initializing ? nullptr : &work.rc, delay, slew) == CsrRcEvaluationStatus::kSuccess
      ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kNumericFailure;
}

[[nodiscard]] CsrCpuReferenceStageStatus initializeNet(
    std::size_t arc, const ArcSemanticPayload& payload, CsrCpuReferenceWorkState& work)
{
  auto& result = work.result.arcs;
  for (AnalysisMode analysis : kAnalyses) {
    for (Transition input_transition : kTransitions) {
      double delay = 0.0, unused_slew = 0.0;
      const auto status = netTiming(payload, analysis, input_transition, 0.0, true, work, delay, unused_slew);
      if (status != CsrCpuReferenceStageStatus::kSuccess) return status;
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
  return CsrCpuReferenceStageStatus::kSuccess;
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

[[nodiscard]] CsrCpuReferenceStageStatus initializeNonemptyCell(std::size_t arc_index, const ArcSemanticPayload& arc, std::size_t child_begin, CsrCpuReferenceWorkState& work)
{
  auto& result = work.result.arcs;
  if (!arc.output_load && !arc.parasitic.tree)
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
        for (std::size_t child_index = 0; child_index < arc.transfer.children().size(); ++child_index) {
          const auto& child = arc.transfer.children()[child_index];
          if (!csrChildSelected(child, input_transition, output_transition))
            continue;
          const TimingTableModel& table = child.delay_tables[csrTransitionIndex(output_transition)];
          if (!table.present)
            continue;

          double candidate = 0.0;
          CsrCpuReferenceStageStatus status
              = lookupChild(arc, child_index, analysis, output_transition, 0.0, false, work, candidate);
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

CsrCpuReferenceStageStatus initializeCsrCpuReferenceArcs(const ActiveCsrEdgeView& edges, CsrCpuReferenceWorkState& work)
{
  work.arc_count = edges.arcSlotCount();
  if (work.arc_count == std::numeric_limits<std::size_t>::max()
      || !checkedMultiply(work.arc_count, kAnalysisCount * kTransitionCount, work.arc_transition_lane_count)
      || !checkedMultiply(work.arc_transition_lane_count, kTransitionCount, work.arc_io_lane_count))
    return CsrCpuReferenceStageStatus::kInvariantViolation;

  struct RcInitialization {
    NodeId driver;
    std::uint64_t ordinal;
    ArcId arc;
  };
  std::vector<RcInitialization> rc_initialization;
  std::vector<std::size_t> child_counts(work.arc_count, 0);
  std::optional<CsrCpuReferenceStageStatus> callback_status;
  EdgeVisitResult visit = edges.forEachActiveArc([&](const EdgeEntry& entry) {
    const auto payload = edges.arc(entry.arc_id);
    if (!payload || entry.arc_id.value() >= work.arc_count) {
      callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
      return EdgeVisitResult::fail(CsrViewError::kCallbackError);
    }
    const std::size_t count = payload.value().transfer.children().size();
    const auto& arc = payload.value();
    if (arc.kind == ArcKind::kCell && arc.parasitic.tree && count != 0) {
      if (!arc.native_cell_arc_ordinal) {
        callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
        return EdgeVisitResult::fail(CsrViewError::kCallbackError);
      }
      rc_initialization.push_back({arc.sink, *arc.native_cell_arc_ordinal, entry.arc_id});
    }
    if (count > std::numeric_limits<std::size_t>::max() - work.timing_arc_count) {
      callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
      return EdgeVisitResult::fail(CsrViewError::kCallbackError);
    }
    child_counts[entry.arc_id.value()] = count;
    work.timing_arc_count += count;
    return EdgeVisitResult::continueVisit();
  });
  if (visit.control == EdgeVisitControl::kError)
    return callback_status.value_or(CsrCpuReferenceStageStatus::kInvariantViolation);
  if (!checkedMultiply(work.timing_arc_count, kAnalysisCount * kTransitionCount * kTransitionCount, work.timing_arc_lane_count))
    return CsrCpuReferenceStageStatus::kInvariantViolation;

  auto& result = work.result.arcs;
  resetOptionalSoa(result.delays, work.arc_count, 0.0);
  resetOptionalSoa(result.min_delays, work.arc_count, 0.0);
  resetOptionalSoa(result.max_delays, work.arc_count, 0.0);
  resetOptionalSoa(result.transition_delays, work.arc_transition_lane_count, 0.0);
  resetOptionalSoa(result.input_output_delays, work.arc_io_lane_count, 0.0);
  resetOptionalSoa(result.graph_delays, work.arc_io_lane_count, 0.0);
  resetOptionalSoa(result.transition_types, work.arc_count * kTransitionCount, Transition::kNone);
  result.timing_arc_row_offsets.assign(work.arc_count + 1, 0);
  for (std::size_t arc_index = 0; arc_index < work.arc_count; ++arc_index)
    result.timing_arc_row_offsets[arc_index + 1] = result.timing_arc_row_offsets[arc_index] + child_counts[arc_index];
  result.timing_arc_indices.assign(work.timing_arc_count, 0);
  resetOptionalSoa(result.timing_arc_delays, work.timing_arc_lane_count, 0.0);
  resetOptionalSoa(result.timing_arc_fallbacks, work.timing_arc_lane_count, std::uint8_t{0});

  callback_status.reset();
  visit = edges.forEachActiveArc([&](const EdgeEntry& entry) {
    recordStorageVisit(work.telemetry.arc_initialization, entry.storage_kind);
    const auto payload = edges.arc(entry.arc_id);
    if (!payload) {
      callback_status = CsrCpuReferenceStageStatus::kInvariantViolation;
      return EdgeVisitResult::fail(CsrViewError::kCallbackError);
    }
    const std::size_t arc_index = entry.arc_id.value();
    const ArcSemanticPayload& arc = payload.value();
    const std::size_t child_offset = result.timing_arc_row_offsets[arc_index];
    for (std::size_t child_index = 0; child_index < arc.transfer.children().size(); ++child_index)
      result.timing_arc_indices[child_offset + child_index] = arc.transfer.children()[child_index].signature.arc_index;
    result.delays.presence[arc_index] = 1;
    result.min_delays.presence[arc_index] = 1;
    result.max_delays.presence[arc_index] = 1;
    for (AnalysisMode analysis : kAnalyses)
      for (Transition transition : kTransitions)
        result.transition_delays.presence[csrArcTransitionLane(arc_index, analysis, transition)] = 1;
    CsrCpuReferenceStageStatus status = CsrCpuReferenceStageStatus::kSuccess;
    if (arc.kind == ArcKind::kNet) {
      status = initializeNet(arc_index, arc, work);
    } else if (!arc.output_load && !arc.parasitic.tree) {
      status = CsrCpuReferenceStageStatus::kInvariantViolation;
    } else if (arc.transfer.children().empty()) {
      initializeEmptyCell(arc_index, arc.transfer, result);
    } else if (arc.parasitic.tree) {
      // Complete below in native Liberty order within each driver. Physical
      // slots and propagation candidate order remain unchanged.
      return EdgeVisitResult::continueVisit();
    } else {
      status = initializeNonemptyCell(arc_index, arc, child_offset, work);
    }
    if (status != CsrCpuReferenceStageStatus::kSuccess) {
      callback_status = status;
      return EdgeVisitResult::fail(CsrViewError::kCallbackError);
    }
    finalizeScalar(arc_index, result);
    return EdgeVisitResult::continueVisit();
  });
  if (visit.control == EdgeVisitControl::kError)
    return callback_status.value_or(CsrCpuReferenceStageStatus::kInvariantViolation);
  std::ranges::sort(rc_initialization, [](const RcInitialization& lhs, const RcInitialization& rhs) {
    return std::tie(lhs.driver, lhs.ordinal, lhs.arc) < std::tie(rhs.driver, rhs.ordinal, rhs.arc);
  });
  for (const auto& entry : rc_initialization) {
    const auto payload = edges.arc(entry.arc);
    if (!payload) return CsrCpuReferenceStageStatus::kInvariantViolation;
    const auto id = entry.arc.value();
    const auto status = initializeNonemptyCell(id, payload.value(), result.timing_arc_row_offsets[id], work);
    if (status != CsrCpuReferenceStageStatus::kSuccess) return status;
    finalizeScalar(id, result);
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

CsrCpuReferenceStageStatus evaluateCsrDynamicArc(const ActiveCsrEdgeView& edges, ArcId arc_id, AnalysisMode analysis, Transition input_transition,
                                                 Transition output_transition, double source_slew, CsrCpuReferenceWorkState& work, CsrDynamicArcValue& value)
{
  value = {};
  if (!std::isfinite(source_slew) || source_slew < 0.0)
    return CsrCpuReferenceStageStatus::kNumericFailure;
  if (arc_id.value() >= work.arc_count)
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  const std::size_t arc_index = static_cast<std::size_t>(arc_id.value());
  const auto payload = edges.arc(arc_id);
  if (!payload)
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  const ArcSemanticPayload& arc = payload.value();
  const std::size_t io_lane = csrArcIoLane(arc_index, analysis, input_transition, output_transition);
  if (work.result.arcs.input_output_delays.presence[io_lane] == 0)
    return CsrCpuReferenceStageStatus::kSuccess;

  value.present = true;
  value.delay = work.result.arcs.input_output_delays.values[io_lane];
  value.slew = source_slew;
  if (arc.kind == ArcKind::kNet) {
    const auto status = netTiming(arc, analysis, output_transition, source_slew, false, work, value.delay, value.slew);
    if (status != CsrCpuReferenceStageStatus::kSuccess) return status;
  } else if (!arc.transfer.children().empty()) {
    if (!arc.output_load && !arc.parasitic.tree)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    bool delay_seen = false;
    bool slew_seen = false;
    double aggregate_delay = 0.0;
    double aggregate_slew = 0.0;
    const std::size_t child_begin = work.result.arcs.timing_arc_row_offsets[arc_index];
    for (std::size_t child_index = 0; child_index < arc.transfer.children().size(); ++child_index) {
      const auto& child = arc.transfer.children()[child_index];
      if (!csrChildSelected(child, input_transition, output_transition))
        continue;
      const TimingTableModel& delay_table = child.delay_tables[csrTransitionIndex(output_transition)];
      if (delay_table.present) {
        double candidate = 0.0;
        CsrCpuReferenceStageStatus status = lookupChild(arc, child_index, analysis, output_transition, source_slew, false, work, candidate);
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

    }
    // Native calculateArc completes all delay children before slew children.
    // RC driver cache misses make this order observable.
    for (std::size_t child_index = 0; child_index < arc.transfer.children().size(); ++child_index) {
      const auto& child = arc.transfer.children()[child_index];
      if (!csrChildSelected(child, input_transition, output_transition))
        continue;
      const TimingTableModel& slew_table = child.slew_tables[csrTransitionIndex(output_transition)];
      if (slew_table.present) {
        double candidate = 0.0;
        CsrCpuReferenceStageStatus status = lookupChild(arc, child_index, analysis, output_transition, source_slew, true, work, candidate);
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
