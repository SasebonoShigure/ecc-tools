// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "CsrCpuReferenceBackendInternal.hpp"
#include "CsrTableLookup.hpp"

namespace ista::csr {
namespace {

const CsrCpuReferencePathState* worstEndpointPath(std::size_t node, const CsrCpuReferenceWorkState& work, Transition& selected_transition)
{
  // Preserve native's two-stage epsilon selection: each transition's tag
  // winner first, then the transition winner. Input paths take precedence.
  for (PathSource source : {PathSource::kInput, PathSource::kRegister}) {
    const CsrCpuReferencePathState* best = nullptr;
    for (Transition transition : {Transition::kRise, Transition::kFall}) {
      const CsrCpuReferencePathState* lane_best = nullptr;
      for (const auto& path : work.path_rows[csrPathRow(node, AnalysisMode::kMax, source, transition)]) {
        if (std::isfinite(path.arrival) && (lane_best == nullptr || path.arrival > lane_best->arrival + 1.0e-6))
          lane_best = &path;
      }
      if (lane_best != nullptr && (best == nullptr || lane_best->arrival > best->arrival + 1.0e-6)) {
        best = lane_best;
        selected_transition = transition;
      }
    }
    if (best != nullptr)
      return best;
  }
  return nullptr;
}

CsrCpuReferenceStageStatus applyCheck(const TimingPayloadView& payloads, std::size_t node, const LibraryTimingCheckArcModel& check,
                                     std::optional<NodeId> clock_node, CsrCpuReferenceWorkState& work)
{
  Transition transition = Transition::kRise;
  const auto* path = worstEndpointPath(node, work, transition);
  const auto output = csrTransitionIndex(transition);
  if (!check.children().empty() && std::ranges::none_of(check.children(), [&](const ChildTimingArcModel& child) { return child.check_tables[output].present; }))
    return CsrCpuReferenceStageStatus::kSuccess;
  const double data_slew = path == nullptr ? 0.0 : path->slew;
  double clock_arrival = 0.0;
  double clock_slew = 0.0;
  const ClockConstraintModel* clock = payloads.clocks().empty() ? nullptr : &payloads.clocks().front();
  if (clock_node) {
    const auto clock_index = clock_node->value();
    if (clock_index >= work.node_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    if (check.clock_transition != Transition::kNone) {
      const auto lane = csrNodeLane(clock_index, AnalysisMode::kMin, check.clock_transition);
      const auto& effective = work.result.effective_clock;
      if (effective.arrivals.presence[lane] != 0)
        clock_arrival = effective.arrivals.values[lane];
      if (effective.slews.presence[lane] != 0)
        clock_slew = effective.slews.values[lane];
    }
    if (work.result.nodes.clock_names.presence[clock_index] != 0) {
      const auto found = std::ranges::find(payloads.clocks(), work.result.nodes.clock_names.values[clock_index], &ClockConstraintModel::map_key);
      if (found != payloads.clocks().end())
        clock = &*found;
    }
  }
  double check_time = check.check_time;
  bool seen = false;
  for (const auto& child : check.children()) {
    if (!child.check_tables[output].present)
      continue;
    const auto value = lookupCsrTimingCheckTable(child.check_tables[output], clock_slew, data_slew, child.time_unit_scale);
    if (!value)
      return CsrCpuReferenceStageStatus::kNumericFailure;
    if (!seen || *value > check_time)
      check_time = *value;
    seen = true;
  }
  // Native scalar RAT uses getEndPointRequired(end, end), without a launch
  // path. Per-path CRPR is applied later by report analysis, not twice here.
  const double capture = (clock == nullptr ? 0.0 : clock->period) + clock_arrival;
  const double required = capture - check_time;
  const double scaled = required * 1.0e15;
  if (!std::isfinite(check_time) || !std::isfinite(capture) || !std::isfinite(required) || !std::isfinite(scaled))
    return CsrCpuReferenceStageStatus::kNumericFailure;
  work.result.nodes.requireds.values[node] = std::round(scaled) / 1.0e15;
  return CsrCpuReferenceStageStatus::kSuccess;
}

}  // namespace

CsrCpuReferenceStageStatus seedCsrCpuSequentialRequired(const TimingPayloadView& payloads, CsrCpuReferenceWorkState& work)
{
  if (payloads.timingCheckEndpoints().empty())
    return CsrCpuReferenceStageStatus::kSuccess;
  if (payloads.nodeRecords().size() != work.node_count)
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  std::map<std::string, const ReferencedLibraryCellModel*, std::less<>> cells;
  for (const auto& cell : payloads.referencedLibraryCells())
    cells.emplace(cell.cell_name, &cell);
  std::map<std::string, NodeId, std::less<>> nodes;
  for (const auto& node : payloads.nodeRecords())
    nodes.emplace(node.key.full_name, node.id);
  std::vector<bool> selected(work.node_count, false);
  for (const auto& endpoint : payloads.timingCheckEndpoints()) {
    if (!endpoint.resolved_data_node || (endpoint.check_kind != TimingCheckKind::kSetup && endpoint.check_kind != TimingCheckKind::kRecovery))
      continue;
    const auto node = endpoint.resolved_data_node->value();
    if (node >= work.node_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const auto& record = payloads.nodeRecords()[node];
    if (selected[node] || record.is_port || !record.is_sequential || !record.is_endpoint)
      continue;
    selected[node] = true;  // First matching check owns the endpoint even if its transition has no table.
    const auto cell = cells.find(record.instance_cell);
    if (cell == cells.end() || endpoint.referenced_check_ordinal >= cell->second->timing_checks.size())
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const auto& check = cell->second->timing_checks[endpoint.referenced_check_ordinal];
    std::optional<NodeId> clock_node;
    const auto found = nodes.find(record.key.instance_name + ":" + cell->second->clock_pin_name);
    if (found != nodes.end())
      clock_node = found->second;
    const auto status = applyCheck(payloads, node, check, clock_node, work);
    if (status != CsrCpuReferenceStageStatus::kSuccess)
      return status;
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

}  // namespace ista::csr
