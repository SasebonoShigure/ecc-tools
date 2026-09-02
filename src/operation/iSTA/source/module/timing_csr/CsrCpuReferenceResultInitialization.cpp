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
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "CsrCpuReferenceBackendInternal.hpp"

namespace ista::csr {
namespace {

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

void resetClockSoa(ClockResultSoa& clock, std::size_t lane_count)
{
  resetOptionalSoa(clock.arrivals, lane_count, 0.0);
  resetOptionalSoa(clock.slews, lane_count, 0.0);
  resetOptionalSoa(clock.predecessor_nodes, lane_count, NodeId{});
  resetOptionalSoa(clock.predecessor_arc_delays, lane_count, 0.0);
  resetOptionalSoa(clock.predecessor_transitions, lane_count, Transition::kNone);
}

[[nodiscard]] const ClockConstraintModel* findDirectClock(const TimingPayloadView& payloads, NodeId node)
{
  for (const auto& clock : payloads.clocks())
    for (const auto& source : clock.ordered_sources)
      if (source.resolved_node == node)
        return &clock;
  return nullptr;
}

[[nodiscard]] std::optional<ClockTagId> findClockTag(const TimingPayloadView& payloads, const std::string& tag)
{
  const auto found = std::find(payloads.clockTags().begin(), payloads.clockTags().end(), tag);
  if (found == payloads.clockTags().end())
    return std::nullopt;
  return ClockTagId{static_cast<std::uint64_t>(found - payloads.clockTags().begin())};
}

void initializeClockSourceLane(ClockResultSoa& clock, std::size_t lane)
{
  clock.arrivals.presence[lane] = 1;
  clock.arrivals.values[lane] = 0.0;
  clock.slews.presence[lane] = 1;
  clock.slews.values[lane] = 0.0;
}

[[nodiscard]] CsrCpuReferenceStageStatus initializeIdealClocks(const TimingPayloadView& payloads, CsrCpuReferenceWorkState& work)
{
  for (const auto& clock : payloads.clocks()) {
    for (const auto& source : clock.ordered_sources) {
      if (!source.resolved_node)
        continue;
      const std::uint64_t node = source.resolved_node->value();
      if (node >= work.node_count)
        return CsrCpuReferenceStageStatus::kInvariantViolation;

      work.result.nodes.is_clock_point.values[node] = 1;
      if (work.result.nodes.clock_names.presence[node] == 0) {
        work.result.nodes.clock_names.presence[node] = 1;
        work.result.nodes.clock_names.values[node] = clock.map_key;
      }
      for (AnalysisMode analysis : kAnalyses) {
        for (Transition transition : kTransitions) {
          const std::size_t lane = csrNodeLane(node, analysis, transition);
          initializeClockSourceLane(work.result.effective_clock, lane);
          initializeClockSourceLane(work.result.physical_clock, lane);
        }
      }
    }
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

[[nodiscard]] CsrCpuReferenceStageStatus initializeStarts(const TimingPayloadView& payloads, CsrCpuReferenceWorkState& work)
{
  if (payloads.startSeeds().size() != payloads.orderedStartIds().size())
    return CsrCpuReferenceStageStatus::kInvariantViolation;

  for (std::size_t index = 0; index < payloads.startSeeds().size(); ++index) {
    const auto& seed = payloads.startSeeds()[index];
    const std::uint64_t node = seed.seed_node.value();
    if (node >= work.node_count || payloads.orderedStartIds()[index] != seed.seed_node)
      return CsrCpuReferenceStageStatus::kInvariantViolation;

    const ClockConstraintModel* direct_clock = findDirectClock(payloads, seed.seed_node);
    for (AnalysisMode analysis : kAnalyses) {
      for (Transition transition : kTransitions) {
        const std::size_t lane = csrNodeLane(node, analysis, transition);
        const auto& slew = seed.slew[csrAnalysisIndex(analysis)][csrTransitionIndex(transition)];
        if (!slew)
          return CsrCpuReferenceStageStatus::kInvariantViolation;
        work.result.data_slews.presence[lane] = 1;
        work.result.data_slews.values[lane] = *slew;
      }
    }

    work.result.nodes.arrivals.values[node] = 0.0;
    if (seed.path_source) {
      const auto& scalar_arrival = seed.arrival[csrAnalysisIndex(AnalysisMode::kMax)][csrTransitionIndex(Transition::kRise)];
      if (!scalar_arrival)
        return CsrCpuReferenceStageStatus::kInvariantViolation;
      work.result.nodes.arrivals.values[node] = *scalar_arrival;
    }
    work.result.nodes.launches.values[node] = direct_clock == nullptr ? 0.0 : direct_clock->edges[csrTransitionIndex(Transition::kRise)];
    work.result.nodes.clock_names.presence[node] = 1;
    work.result.nodes.clock_names.values[node] = seed.clock_tag;

    if (!seed.path_source)
      continue;
    if (*seed.path_source != PathSource::kInput || seed.availability != StartSeedAvailability::kMaterialized)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const std::optional<ClockTagId> clock_tag = findClockTag(payloads, seed.clock_tag);
    if (!clock_tag)
      return CsrCpuReferenceStageStatus::kInvariantViolation;

    for (AnalysisMode analysis : kAnalyses) {
      for (Transition transition : kTransitions) {
        const auto& arrival = seed.arrival[csrAnalysisIndex(analysis)][csrTransitionIndex(transition)];
        const auto& slew = seed.slew[csrAnalysisIndex(analysis)][csrTransitionIndex(transition)];
        if (!arrival || !slew)
          return CsrCpuReferenceStageStatus::kInvariantViolation;

        CsrCpuReferencePathState path;
        path.clock_tag = *clock_tag;
        path.arrival = *arrival;
        path.slew = *slew;
        path.launch = direct_clock == nullptr
                          ? 0.0
                          : direct_clock->edges[analysis == AnalysisMode::kMax && transition == Transition::kFall ? csrTransitionIndex(Transition::kFall)
                                                                                                                  : csrTransitionIndex(Transition::kRise)];
        path.start_node = seed.path_start_node;
        path.crpr_node = seed.crpr_node;
        path.crpr_transition = seed.crpr_transition;
        work.path_rows[csrPathRow(node, analysis, PathSource::kInput, transition)].push_back(path);
      }
    }
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}

}  // namespace

CsrCpuReferenceStageStatus initializeCsrCpuReferenceResult(const TimingPayloadView& payloads, const CsrFifoLevelization& replay, CsrCpuReferenceWorkState& work)
{
  work.node_count = payloads.nodeCount();
  work.clock_tag_count = payloads.clockTags().size();
  if (!checkedMultiply(work.node_count, kAnalysisCount * kTransitionCount, work.node_lane_count)
      || !checkedMultiply(work.node_count, kAnalysisCount * kPathSourceCount * kTransitionCount, work.path_row_count)
      || work.path_row_count == std::numeric_limits<std::size_t>::max())
    return CsrCpuReferenceStageStatus::kInvariantViolation;

  work.result.level_offsets = replay.level_offsets;
  work.result.level_nodes = replay.level_nodes;
  work.result.node_to_level = replay.node_to_level;
  work.result.timing_order = replay.level_nodes;

  resetRequiredSoa(work.result.nodes.is_clock_point, work.node_count, std::uint8_t{0});
  resetRequiredSoa(work.result.nodes.arrivals, work.node_count, -std::numeric_limits<double>::infinity());
  resetRequiredSoa(work.result.nodes.requireds, work.node_count, std::numeric_limits<double>::infinity());
  resetRequiredSoa(work.result.nodes.slacks, work.node_count, 0.0);
  resetRequiredSoa(work.result.nodes.launches, work.node_count, 0.0);
  resetOptionalSoa(work.result.nodes.clock_names, work.node_count, std::string{});
  resetOptionalSoa(work.result.nodes.predecessor_nodes, work.node_count, NodeId{});
  resetOptionalSoa(work.result.nodes.predecessor_arcs, work.node_count, ArcId{});

  resetClockSoa(work.result.effective_clock, work.node_lane_count);
  resetClockSoa(work.result.physical_clock, work.node_lane_count);
  resetOptionalSoa(work.result.data_slews, work.node_lane_count, 0.0);

  work.path_rows.assign(work.path_row_count, {});
  work.result.paths.row_offsets.assign(work.path_row_count + 1, 0);

  CsrCpuReferenceStageStatus status = initializeIdealClocks(payloads, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  return initializeStarts(payloads, work);
}

}  // namespace ista::csr
