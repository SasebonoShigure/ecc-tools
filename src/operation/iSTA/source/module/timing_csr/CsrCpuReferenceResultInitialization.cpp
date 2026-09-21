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
#include <map>
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

}  // namespace

CsrCpuReferenceStageStatus collectCsrInitialTimingSeeds(const TimingPayloadView& payloads, CsrInitialTimingSeeds& seeds)
{
  const auto node_count = payloads.nodeCount();
  std::size_t rows = 0;
  if (!checkedMultiply(node_count, kAnalysisCount * kPathSourceCount * kTransitionCount, rows)
      || rows == std::numeric_limits<std::size_t>::max())
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  seeds.nodes.clear();
  seeds.paths.clear();
  if (payloads.clockOwners().size() != node_count || payloads.startSeeds().size() != payloads.orderedStartIds().size())
    return CsrCpuReferenceStageStatus::kInvariantViolation;
  std::map<NodeId, CsrInitialNodeSeed> nodes;
  auto node_seed = [&](NodeId id) -> CsrInitialNodeSeed& {
    auto& value = nodes[id];
    value.node = id;
    return value;
  };
  for (std::size_t node = 0; node < node_count; ++node) {
    const auto& owner = payloads.clockOwners()[node];
    if (owner) {
      auto& value = node_seed(NodeId{node});
      value.clock_point = true;
      value.clock_name = *owner;
    }
  }
  for (const auto& clock : payloads.clocks())
    for (const auto& source : clock.ordered_sources) {
      if (!source.resolved_node)
        continue;
      if (source.resolved_node->value() >= node_count)
        return CsrCpuReferenceStageStatus::kInvariantViolation;
      auto& value = node_seed(*source.resolved_node);
      if (!value.clock_point)
        value.clock_name = clock.map_key;
      value.clock_point = true;
      value.physical_clock_source = true;
    }
  for (std::size_t index = 0; index < payloads.startSeeds().size(); ++index) {
    const auto& seed = payloads.startSeeds()[index];
    const auto node = seed.seed_node.value();
    if (node >= node_count || payloads.orderedStartIds()[index] != seed.seed_node)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    if (seed.availability == StartSeedAvailability::kDeferredSequential)
      continue;
    auto& value = node_seed(seed.seed_node);
    const auto* direct_clock = findDirectClock(payloads, seed.seed_node);
    value.data_start = true;
    value.arrival = 0.0;
    value.launch = direct_clock == nullptr ? 0.0 : direct_clock->edges[csrTransitionIndex(Transition::kRise)];
    value.clock_name = seed.clock_tag;
    for (AnalysisMode analysis : kAnalyses)
      for (Transition transition : kTransitions) {
        const auto a = csrAnalysisIndex(analysis), t = csrTransitionIndex(transition);
        if (!seed.slew[a][t])
          return CsrCpuReferenceStageStatus::kInvariantViolation;
        value.slew[a * 2 + t] = *seed.slew[a][t];
      }
    if (!seed.path_source)
      continue;
    if (*seed.path_source != PathSource::kInput || seed.availability != StartSeedAvailability::kMaterialized)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    const auto clock_tag = findClockTag(payloads, seed.clock_tag);
    if (!clock_tag)
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    if (!seed.arrival[0][0])
      return CsrCpuReferenceStageStatus::kInvariantViolation;
    value.arrival = *seed.arrival[0][0];
    for (AnalysisMode analysis : kAnalyses)
      for (Transition transition : kTransitions) {
        const auto a = csrAnalysisIndex(analysis), t = csrTransitionIndex(transition);
        if (!seed.arrival[a][t])
          return CsrCpuReferenceStageStatus::kInvariantViolation;
        CsrCpuReferencePathState path;
        path.clock_tag = *clock_tag;
        path.arrival = *seed.arrival[a][t];
        path.slew = value.slew[a * 2 + t];
        path.launch = direct_clock == nullptr ? 0.0
            : direct_clock->edges[analysis == AnalysisMode::kMax && transition == Transition::kFall ? 1 : 0];
        path.start_node = seed.path_start_node;
        path.crpr_node = seed.crpr_node;
        path.crpr_transition = seed.crpr_transition;
        seeds.paths.push_back({csrPathRow(node, analysis, PathSource::kInput, transition), std::move(path)});
      }
  }
  seeds.nodes.reserve(nodes.size());
  for (auto& [id, value] : nodes) {
    static_cast<void>(id);
    seeds.nodes.push_back(std::move(value));
  }
  return CsrCpuReferenceStageStatus::kSuccess;
}


CsrCpuReferenceStageStatus initializeCsrCpuReferenceSeeds(const TimingPayloadView& payloads, CsrCpuReferenceWorkState& work)
{
  work.node_count = payloads.nodeCount();
  work.clock_tag_count = payloads.clockTags().size();
  if (!checkedMultiply(work.node_count, kAnalysisCount * kTransitionCount, work.node_lane_count)
      || !checkedMultiply(work.node_count, kAnalysisCount * kPathSourceCount * kTransitionCount, work.path_row_count)
      || work.path_row_count == std::numeric_limits<std::size_t>::max())
    return CsrCpuReferenceStageStatus::kInvariantViolation;

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

  CsrInitialTimingSeeds seeds;
  const auto status = collectCsrInitialTimingSeeds(payloads, seeds);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return status;
  for (const auto& seed : seeds.nodes) {
    const auto node = seed.node.value();
    work.result.nodes.clock_names.presence[node] = 1;
    work.result.nodes.clock_names.values[node] = seed.clock_name;
    work.result.nodes.is_clock_point.values[node] = seed.clock_point;
    work.result.nodes.arrivals.values[node] = seed.arrival;
    work.result.nodes.launches.values[node] = seed.launch;
    for (std::size_t lane = 0; lane < 4; ++lane) {
      const auto flat = node * 4 + lane;
      if (seed.clock_point)
        initializeClockSourceLane(work.result.effective_clock, flat);
      if (seed.physical_clock_source)
        initializeClockSourceLane(work.result.physical_clock, flat);
      if (seed.data_start) {
        work.result.data_slews.presence[flat] = 1;
        work.result.data_slews.values[flat] = seed.slew[lane];
      }
    }
  }
  for (auto& seed : seeds.paths)
    work.path_rows[seed.row].push_back(std::move(seed.path));
  return CsrCpuReferenceStageStatus::kSuccess;
}

CsrCpuReferenceStageStatus initializeCsrCpuReferenceResult(const TimingPayloadView& payloads, CsrFifoLevelization replay, CsrCpuReferenceWorkState& work)
{
  work.result.level_offsets = std::move(replay.level_offsets);
  work.result.level_nodes = std::move(replay.level_nodes);
  work.result.node_to_level = std::move(replay.node_to_level);
  return initializeCsrCpuReferenceSeeds(payloads, work);
}

}  // namespace ista::csr
