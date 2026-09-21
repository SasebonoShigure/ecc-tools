// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrFunctionalProjectionInternal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr {
namespace {

template <typename Input, typename Output, typename Transform>
void appendLane(const csr::OptionalSoa<Input>& source, std::size_t lane,
                csr::OptionalSoa<Output>& target, Transform transform)
{
  const std::uint8_t presence = source.presence.at(lane);
  target.presence.push_back(presence);
  if (presence == 0)
    target.values.emplace_back();
  else
    target.values.push_back(transform(source.values.at(lane)));
}

template <typename T>
void appendLane(const csr::OptionalSoa<T>& source, std::size_t lane,
                csr::OptionalSoa<T>& target)
{
  appendLane(source, lane, target,
             [](const T& value) { return value; });
}

template <typename T>
csr::OptionalSoa<T> projectNodeSoa(
    const csr::OptionalSoa<T>& source,
    const detail::CanonicalSemanticIndex& index, std::size_t width)
{
  csr::OptionalSoa<T> result;
  result.presence.reserve(index.node_ids.size() * width);
  result.values.reserve(index.node_ids.size() * width);
  for (const auto id : index.node_ids)
    for (std::size_t lane = 0; lane < width; ++lane)
      appendLane(source, id * width + lane, result);
  return result;
}

template <typename Input, typename Output, typename Transform>
csr::OptionalSoa<Output> projectNodeSoa(
    const csr::OptionalSoa<Input>& source,
    const detail::CanonicalSemanticIndex& index, std::size_t width,
    Transform transform)
{
  csr::OptionalSoa<Output> result;
  result.presence.reserve(index.node_ids.size() * width);
  result.values.reserve(index.node_ids.size() * width);
  for (const auto id : index.node_ids)
    for (std::size_t lane = 0; lane < width; ++lane)
      appendLane(source, id * width + lane, result, transform);
  return result;
}

template <typename T>
csr::OptionalSoa<T> projectActiveArcSoa(
    const csr::OptionalSoa<T>& source,
    const detail::CanonicalSemanticIndex& index, std::size_t width)
{
  csr::OptionalSoa<T> result;
  result.presence.reserve(index.active_arc_ids.size() * width);
  result.values.reserve(index.active_arc_ids.size() * width);
  for (const auto id : index.active_arc_ids)
    for (std::size_t lane = 0; lane < width; ++lane)
      appendLane(source, id * width + lane, result);
  return result;
}

DualCsrProjectedNodeTiming projectNodeTiming(
    const DualCsrState& state, const csr::NodeResultSoa& source,
    const detail::CanonicalSemanticIndex& index)
{
  DualCsrProjectedNodeTiming result;
  result.is_clock_point = projectNodeSoa(source.is_clock_point, index, 1);
  result.arrivals = projectNodeSoa(source.arrivals, index, 1);
  result.requireds = projectNodeSoa(source.requireds, index, 1);
  result.slacks = projectNodeSoa(source.slacks, index, 1);
  result.launches = projectNodeSoa(source.launches, index, 1);
  result.clock_names = projectNodeSoa(source.clock_names, index, 1);
  result.predecessor_nodes
      = projectNodeSoa<csr::NodeId, csr::NodeKey>(
          source.predecessor_nodes, index, 1,
          [&](csr::NodeId id) { return detail::semanticNodeKey(state, id); });
  result.predecessor_arcs
      = projectNodeSoa<csr::ArcId, csr::ArcKey>(
          source.predecessor_arcs, index, 1,
          [&](csr::ArcId id) { return detail::semanticArcKey(state, id); });
  return result;
}

DualCsrProjectedClockTiming projectClockTiming(
    const DualCsrState& state, const csr::ClockResultSoa& source,
    const detail::CanonicalSemanticIndex& index)
{
  constexpr std::size_t kNodeLaneWidth
      = csr::kAnalysisCount * csr::kTransitionCount;
  DualCsrProjectedClockTiming result;
  result.arrivals = projectNodeSoa(source.arrivals, index, kNodeLaneWidth);
  result.slews = projectNodeSoa(source.slews, index, kNodeLaneWidth);
  result.predecessor_nodes
      = projectNodeSoa<csr::NodeId, csr::NodeKey>(
          source.predecessor_nodes, index, kNodeLaneWidth,
          [&](csr::NodeId id) { return detail::semanticNodeKey(state, id); });
  result.predecessor_arc_delays = projectNodeSoa(
      source.predecessor_arc_delays, index, kNodeLaneWidth);
  result.predecessor_transitions = projectNodeSoa(
      source.predecessor_transitions, index, kNodeLaneWidth);
  return result;
}

DualCsrProjectedPathTiming projectPathTiming(
    const DualCsrState& state, const csr::PathResultSoa& source,
    const detail::CanonicalSemanticIndex& index)
{
  constexpr std::size_t kRowsPerNode
      = csr::kAnalysisCount * csr::kPathSourceCount
        * csr::kTransitionCount;
  DualCsrProjectedPathTiming result;
  result.row_offsets.push_back(0);
  for (const auto node_id : index.node_ids) {
    for (std::size_t local_row = 0; local_row < kRowsPerNode;
         ++local_row) {
      const std::size_t old_row = node_id * kRowsPerNode + local_row;
      const std::size_t begin
          = static_cast<std::size_t>(source.row_offsets.at(old_row));
      const std::size_t end
          = static_cast<std::size_t>(source.row_offsets.at(old_row + 1));
      for (std::size_t entry = begin; entry < end; ++entry) {
        appendLane(
            source.clock_tags, entry, result.clock_tags,
            [&](csr::ClockTagId id) {
              return state.reference_payload.clock_tags.at(id.value());
            });
        appendLane(source.arrivals, entry, result.arrivals);
        appendLane(source.slews, entry, result.slews);
        appendLane(source.launches, entry, result.launches);
        appendLane(
            source.start_nodes, entry, result.start_nodes,
            [&](csr::NodeId id) {
              return detail::semanticNodeKey(state, id);
            });
        appendLane(
            source.predecessor_nodes, entry, result.predecessor_nodes,
            [&](csr::NodeId id) {
              return detail::semanticNodeKey(state, id);
            });
        appendLane(
            source.predecessor_arcs, entry, result.predecessor_arcs,
            [&](csr::ArcId id) {
              return detail::semanticArcKey(state, id);
            });
        appendLane(source.predecessor_arc_delays, entry,
                   result.predecessor_arc_delays);
        appendLane(source.predecessor_transitions, entry,
                   result.predecessor_transitions);
        appendLane(
            source.crpr_nodes, entry, result.crpr_nodes,
            [&](csr::NodeId id) {
              return detail::semanticNodeKey(state, id);
            });
        appendLane(source.crpr_transitions, entry,
                   result.crpr_transitions);
      }
      result.row_offsets.push_back(result.clock_tags.values.size());
    }
  }
  return result;
}

DualCsrProjectedArcTiming projectArcTiming(
    const csr::ArcResultSoa& source,
    const detail::CanonicalSemanticIndex& index)
{
  constexpr std::size_t kTransitionWidth
      = csr::kAnalysisCount * csr::kTransitionCount;
  constexpr std::size_t kGraphWidth
      = kTransitionWidth * csr::kTransitionCount;
  constexpr std::size_t kTypeWidth = csr::kTransitionCount;
  DualCsrProjectedArcTiming result;
  result.delays = projectActiveArcSoa(source.delays, index, 1);
  result.min_delays = projectActiveArcSoa(source.min_delays, index, 1);
  result.max_delays = projectActiveArcSoa(source.max_delays, index, 1);
  result.transition_delays = projectActiveArcSoa(
      source.transition_delays, index, kTransitionWidth);
  result.input_output_delays = projectActiveArcSoa(
      source.input_output_delays, index, kGraphWidth);
  result.graph_delays = projectActiveArcSoa(
      source.graph_delays, index, kGraphWidth);
  result.transition_types = projectActiveArcSoa(
      source.transition_types, index, kTypeWidth);

  result.timing_arc_row_offsets.push_back(0);
  for (const auto arc_id : index.active_arc_ids) {
    const std::size_t begin = static_cast<std::size_t>(
        source.timing_arc_row_offsets.at(arc_id));
    const std::size_t end = static_cast<std::size_t>(
        source.timing_arc_row_offsets.at(arc_id + 1));
    for (std::size_t entry = begin; entry < end; ++entry) {
      result.timing_arc_indices.push_back(
          source.timing_arc_indices.at(entry));
      for (std::size_t lane = 0; lane < kGraphWidth; ++lane) {
        appendLane(source.timing_arc_delays,
                   entry * kGraphWidth + lane,
                   result.timing_arc_delays);
        appendLane(source.timing_arc_fallbacks,
                   entry * kGraphWidth + lane,
                   result.timing_arc_fallbacks);
      }
    }
    result.timing_arc_row_offsets.push_back(
        result.timing_arc_indices.size());
  }
  return result;
}

void projectLevelization(
    const DualCsrState& state, const csr::CsrTimingResult& source,
    const detail::CanonicalSemanticIndex& index,
    DualCsrTimingSemanticProjection& result)
{
  result.level_offsets.push_back(0);
  for (std::size_t level = 0; level + 1 < source.level_offsets.size();
       ++level) {
    std::vector<csr::NodeKey> members;
    const std::size_t begin
        = static_cast<std::size_t>(source.level_offsets[level]);
    const std::size_t end
        = static_cast<std::size_t>(source.level_offsets[level + 1]);
    members.reserve(end - begin);
    for (std::size_t position = begin; position < end; ++position)
      members.push_back(detail::semanticNodeKey(
          state, source.level_nodes.at(position)));
    std::ranges::sort(members, csr::nodeKeyLess);
    result.level_nodes.insert(result.level_nodes.end(),
                              members.begin(), members.end());
    result.level_offsets.push_back(result.level_nodes.size());
  }
  result.node_to_level.reserve(index.node_ids.size());
  for (const auto id : index.node_ids)
    result.node_to_level.push_back(source.node_to_level.at(id));
}

}  // namespace

namespace detail {

DualCsrTimingSemanticProjection projectValidatedDualCsrTimingResult(
    const DualCsrState& state, const DualCsrTimingResult& timing,
    const CanonicalSemanticIndex& index)
{
  const auto& source = timing.values;
  DualCsrTimingSemanticProjection result;
  result.schema_version = source.schema_version;
  projectLevelization(state, source, index, result);
  result.nodes = projectNodeTiming(state, source.nodes, index);
  result.effective_clock = projectClockTiming(
      state, source.effective_clock, index);
  result.physical_clock = projectClockTiming(
      state, source.physical_clock, index);
  constexpr std::size_t kNodeLaneWidth
      = csr::kAnalysisCount * csr::kTransitionCount;
  result.data_slews = projectNodeSoa(
      source.data_slews, index, kNodeLaneWidth);
  result.paths = projectPathTiming(state, source.paths, index);
  result.arcs = projectArcTiming(source.arcs, index);
  return result;
}

}  // namespace detail

FunctionalSemanticReceiptAttempt projectDualCsrFunctionalSemanticReceipt(
    const DualCsrState& state, const DualCsrTimingResult& timing)
{
  FunctionalSemanticReceiptAttempt attempt;
  attempt.violations = validateDualCsrTimingResult(state, timing);
  if (!attempt.violations.empty())
    return attempt;
  const auto index = detail::makeCanonicalSemanticIndex(state);
  attempt.receipt = FunctionalSemanticReceipt{
      .graph = detail::projectValidatedDualCsrState(state, index),
      .timing = detail::projectValidatedDualCsrTimingResult(
          state, timing, index)};
  return attempt;
}

}  // namespace ista::timing_dual_csr
