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

CsrCpuReferenceStageStatus outputLoad(const ActiveCsrEdgeView& edges, NodeId node, ArcOutputLoadMatrix& load)
{
  // Every outgoing net arc carries the canonical load of the entire Q net.
  const auto visit = edges.forEachOutgoing(node, [&](const EdgeEntry& entry) {
    const auto arc = edges.arc(entry.arc_id);
    if (!arc || arc.value().source != node || arc.value().sink.value() >= edges.nodeCount())
      return EdgeVisitResult::fail(CsrViewError::kCallbackError);
    if (arc.value().kind != ArcKind::kNet)
      return EdgeVisitResult::continueVisit();
    if (!arc.value().output_load)
      return EdgeVisitResult::fail(CsrViewError::kCallbackError);
    load = *arc.value().output_load;
    return EdgeVisitResult::stopVisit();
  });
  return visit.control == EdgeVisitControl::kError ? CsrCpuReferenceStageStatus::kInvariantViolation : CsrCpuReferenceStageStatus::kSuccess;
}

CsrCpuReferenceStageStatus clockToQ(const CellTransferModel* transfer, AnalysisMode analysis, Transition clock_transition,
                                   Transition output_transition, double clock_slew, double load, CsrDynamicArcValue& value)
{
  // Native Instance's absent clock-to-Q arc has zero delay, unlike a default
  // CSR graph arc. This standalone seed calculation must not mutate arc caches.
  value = {true, 0.0, clock_slew};
  if (transfer == nullptr)
    return CsrCpuReferenceStageStatus::kSuccess;
  value.delay = transfer->children.empty() ? (analysis == AnalysisMode::kMax ? transfer->delay_max : transfer->delay_min) : transfer->delay;
  bool delay_seen = false;
  bool slew_seen = false;
  const auto output = csrTransitionIndex(output_transition);
  for (const auto& child : transfer->children) {
    if (!csrChildSelected(child, clock_transition, output_transition))
      continue;
    const auto accumulate = [&](const TimingTableModel& table, double& result, bool& seen) {
      if (!table.present)
        return true;
      const auto candidate = lookupCsrTimingTable(table, clock_slew, load, child.time_unit_scale, child.capacitance_unit_scale);
      if (!candidate || !std::isfinite(*candidate))
        return false;
      if (!seen || (analysis == AnalysisMode::kMax ? *candidate > result : *candidate < result))
        result = *candidate;
      seen = true;
      return true;
    };
    if (!accumulate(child.delay_tables[output], value.delay, delay_seen) || !accumulate(child.slew_tables[output], value.slew, slew_seen))
      return CsrCpuReferenceStageStatus::kNumericFailure;
  }
  return std::isfinite(value.delay) && std::isfinite(value.slew) && value.slew >= 0.0
             ? CsrCpuReferenceStageStatus::kSuccess : CsrCpuReferenceStageStatus::kNumericFailure;
}

CsrCpuReferenceStageStatus initializeSeed(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads, const StartSeedModel& seed,
                                         const ReferencedLibraryCellModel& cell, std::optional<NodeId> clock_node, CsrCpuReferenceWorkState& work)
{
  const auto node = seed.seed_node.value();
  const auto& record = payloads.nodeRecords()[node];
  const auto selected = std::ranges::find_if(cell.cell_arcs, [](const LibraryCellArcModel& arc) { return arc.transfer.is_clock_arc; });
  const CellTransferModel* transfer = selected == cell.cell_arcs.end() ? nullptr : &selected->transfer;
  const Transition trigger = transfer != nullptr && !transfer->children.empty()
                                 && transfer->children.front().signature.trigger_transition == Transition::kFall ? Transition::kFall : Transition::kRise;
  const bool is_clock = clock_node == seed.seed_node;
  const bool has_output = cell.present && (transfer != nullptr || std::ranges::any_of(cell.ports, [](const LibraryPortModel& port) {
    return port.is_output && !port.is_clock;
  }));
  const bool is_output = has_output && record.key.pin_name == cell.output_pin_name;
  ArcOutputLoadMatrix load{};
  if (is_output) {
    const auto status = outputLoad(edges, seed.seed_node, load);
    if (status != CsrCpuReferenceStageStatus::kSuccess)
      return status;
  }
  std::optional<ClockTagId> tag;
  if (seed.path_source) {
    const auto found = std::ranges::find(payloads.clockTags(), seed.clock_tag);
    if (*seed.path_source != PathSource::kRegister || found == payloads.clockTags().end() || (!is_clock && !is_output)
        || seed.path_start_node.value() >= work.node_count || !clock_node || seed.crpr_node != clock_node || seed.crpr_transition != trigger)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    tag = ClockTagId{static_cast<std::uint64_t>(found - payloads.clockTags().begin())};
  }
  work.result.nodes.clock_names.presence[node] = 1;
  work.result.nodes.clock_names.values[node] = seed.clock_tag;
  for (AnalysisMode analysis : {AnalysisMode::kMax, AnalysisMode::kMin}) {
    double clock_arrival = 0.0;
    double clock_slew = 0.0;
    if (clock_node && (is_clock || is_output)) {
      const auto lane = csrNodeLane(clock_node->value(), analysis, trigger);
      const auto& clock = work.result.effective_clock;
      if (clock.arrivals.presence[lane] != 0)
        clock_arrival = clock.arrivals.values[lane];
      if (clock.slews.presence[lane] != 0)
        clock_slew = clock.slews.values[lane];
    }
    for (Transition transition : {Transition::kRise, Transition::kFall}) {
      CsrDynamicArcValue value{true, 0.0, clock_slew};
      if (is_output) {
        const auto status = clockToQ(transfer, analysis, trigger, transition, clock_slew,
                                     load[csrAnalysisIndex(analysis)][csrTransitionIndex(transition)], value);
        if (status != CsrCpuReferenceStageStatus::kSuccess)
          return status;
      }
      const double arrival = clock_arrival + value.delay;
      if (!std::isfinite(arrival) || !std::isfinite(clock_arrival) || !std::isfinite(value.slew) || value.slew < 0.0)
        return CsrCpuReferenceStageStatus::kNumericFailure;
      const auto lane = csrNodeLane(node, analysis, transition);
      work.result.data_slews.presence[lane] = 1;
      work.result.data_slews.values[lane] = value.slew;
      if (analysis == AnalysisMode::kMax && transition == Transition::kRise) {
        work.result.nodes.arrivals.values[node] = arrival;
        work.result.nodes.launches.values[node] = clock_arrival;
      }
      if (tag) {
        CsrCpuReferencePathState path;
        path.clock_tag = *tag;
        path.arrival = arrival;
        path.slew = value.slew;
        path.launch = clock_arrival;
        path.start_node = seed.path_start_node;
        path.crpr_node = seed.crpr_node;
        path.crpr_transition = seed.crpr_transition;
        work.path_rows[csrPathRow(node, analysis, PathSource::kRegister, transition)].push_back(path);
      }
    }
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

}  // namespace

CsrCpuReferenceStageStatus initializeCsrCpuSequentialSeeds(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                         CsrCpuReferenceWorkState& work)
{
  if (std::ranges::none_of(payloads.startSeeds(), [](const StartSeedModel& seed) { return seed.availability == StartSeedAvailability::kDeferredSequential; }))
    return CsrCpuReferenceStageStatus::kSuccess;
  if (payloads.nodeRecords().size() != work.node_count)
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  std::map<std::string, const ReferencedLibraryCellModel*, std::less<>> cells;
  for (const auto& cell : payloads.referencedLibraryCells())
    cells.emplace(cell.cell_name, &cell);
  std::map<std::string, NodeId, std::less<>> nodes;
  for (const auto& node : payloads.nodeRecords())
    nodes.emplace(node.key.full_name, node.id);
  for (const auto& seed : payloads.startSeeds()) {
    if (seed.availability != StartSeedAvailability::kDeferredSequential)
      continue;
    if (seed.seed_node.value() >= work.node_count)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const auto& node = payloads.nodeRecords()[seed.seed_node.value()];
    const auto cell = cells.find(node.instance_cell);
    if (cell == cells.end() || node.is_port || !node.is_sequential)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    std::optional<NodeId> clock_node;
    if (cell->second->present && !cell->second->timing_checks.empty()) {
      const auto found = nodes.find(node.key.instance_name + ":" + cell->second->clock_pin_name);
      if (found != nodes.end()) {
        if (found->second.value() >= work.node_count)
          return CsrCpuReferenceStageStatus::kInvariantViolation;
        clock_node = found->second;
      }
    }
    const auto status = initializeSeed(edges, payloads, seed, *cell->second, clock_node, work);
    if (status != CsrCpuReferenceStageStatus::kSuccess)
      return status;
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

}  // namespace ista::csr
