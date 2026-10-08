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
#include <cmath>
#include <iterator>
#include <utility>

#include "CsrFifoLevelization.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrReferenceValidatorDetail.hpp"
#include "CsrTimingResultValidatorInternal.hpp"

namespace ista::csr {
namespace {

using namespace detail;
using ResultClass = CsrTimingResultViolationClass;
constexpr double kPathRoundScale = 1E15;

inline bool checkedFiniteAdd(double lhs, double rhs, double& result)
{
  if (!isFinite(lhs) || !isFinite(rhs))
    return false;
  result = lhs + rhs;
  return isFinite(result);
}

inline bool checkedRoundedPathArrival(double lhs, double rhs, double& result)
{
  double sum = 0.0;
  if (!checkedFiniteAdd(lhs, rhs, sum))
    return false;
  const double scaled = sum * kPathRoundScale;
  if (!isFinite(scaled))
    return false;
  const double rounded = std::round(scaled);
  if (!isFinite(rounded))
    return false;
  result = rounded / kPathRoundScale;
  return isFinite(result);
}

inline bool hasResolvedClockSource(const ClockConstraintModel& clock, NodeId node)
{
  return std::ranges::any_of(clock.ordered_sources, [&](const RawNodeReference& source) { return source.resolved_node && *source.resolved_node == node; });
}

// Deferred register roots bind to the selected current clock lane. Malformed
// clock arrays are diagnosed by validateClockSoa, without indexing them here.
inline std::optional<double> sequentialLaunch(const StartSeedModel& seed, std::size_t analysis,
                                             const ClockResultSoa& clock)
{
  if (!seed.crpr_node || !seed.crpr_transition)
    return 0.0;
  const auto lane = (seed.crpr_node->value() * kAnalysisCount + analysis) * kTransitionCount
                    + static_cast<std::size_t>(*seed.crpr_transition);
  if (lane >= clock.arrivals.presence.size() || lane >= clock.arrivals.values.size())
    return std::nullopt;
  return clock.arrivals.presence[lane] ? clock.arrivals.values[lane] : 0.0;
}

inline void validateNodeResultSoa(const NodeResultSoa& nodes, const ArcResultSoa& arcs, const CsrReferenceInput& input,
                                  const ClockResultSoa& effective, TimingResultViolations& errors)
{
  const std::size_t node_count = input.nodes.size();
  const bool clock_points_ok = validateOptionalSoa(nodes.is_clock_point, node_count, true, errors, ResultClass::kShape, "result.nodes.is_clock_point",
                                                   [&](std::uint8_t value, std::size_t) {
                                                     if (value > 1)
                                                       reject(errors, ResultClass::kShape, "result.nodes.is_clock_point", "value must be zero or one");
                                                   });
  const bool arrivals_ok
      = validateOptionalSoa(nodes.arrivals, node_count, true, errors, ResultClass::kShape, "result.nodes.arrivals", [&](double value, std::size_t) {
          if (!validArrival(value))
            reject(errors, ResultClass::kNumeric, "result.nodes.arrivals", "arrival must be finite or negative infinity");
        });
  const bool requireds_ok
      = validateOptionalSoa(nodes.requireds, node_count, true, errors, ResultClass::kShape, "result.nodes.requireds", [&](double value, std::size_t) {
          if (!validRequired(value))
            reject(errors, ResultClass::kNumeric, "result.nodes.requireds", "required must be finite or positive infinity");
        });
  const auto finite_node = [&](double value, std::size_t) {
    if (!isFinite(value))
      reject(errors, ResultClass::kNumeric, "result.nodes", "slack and launch values must be finite");
  };
  const bool slacks_ok = validateOptionalSoa(nodes.slacks, node_count, true, errors, ResultClass::kShape, "result.nodes.slacks", finite_node);
  const bool launches_ok = validateOptionalSoa(nodes.launches, node_count, true, errors, ResultClass::kShape, "result.nodes.launches", finite_node);
  const bool clock_names_ok = validateOptionalSoa(
      nodes.clock_names, node_count, false, errors, ResultClass::kShape, "result.nodes.clock_names", [&](const std::string& value, std::size_t) {
        if (value.empty()) {
          reject(errors, ResultClass::kIdMapping, "result.nodes.clock_names", "present clock name must be nonempty");
        } else if (!std::ranges::binary_search(input.clock_tags, value, byteStringLess)) {
          reject(errors, ResultClass::kIdMapping, "result.nodes.clock_names", "present clock name does not resolve");
        }
      });
  if (clock_points_ok) {
    for (std::size_t node = 0; node < node_count; ++node)
      if (nodes.is_clock_point.values[node] != static_cast<std::uint8_t>(input.clock_owner_by_node[node].has_value()))
        reject(errors, ResultClass::kIdMapping, "result.nodes.is_clock_point", "clock points must equal the replayed clock ownership");
  }
  const bool node_pred_shape = validateOptionalSoa(nodes.predecessor_nodes, node_count, false, errors, ResultClass::kShape, "result.nodes.predecessor_nodes",
                                                   [&](NodeId value, std::size_t) {
                                                     if (value.value() >= node_count)
                                                       reject(errors, ResultClass::kIdMapping, "result.nodes.predecessor_nodes", "NodeId is out of range");
                                                   });
  const bool arc_pred_shape = validateOptionalSoa(nodes.predecessor_arcs, node_count, false, errors, ResultClass::kShape, "result.nodes.predecessor_arcs",
                                                  [&](ArcId value, std::size_t) {
                                                    if (value.value() >= input.arcs.size())
                                                      reject(errors, ResultClass::kIdMapping, "result.nodes.predecessor_arcs", "ArcId is out of range");
                                                  });
  if (!(arrivals_ok && requireds_ok && slacks_ok && node_pred_shape && arc_pred_shape))
    return;
  const bool arc_delays_shape = arcs.delays.presence.size() == input.arcs.size() && arcs.delays.values.size() == input.arcs.size();
  std::vector<std::string> source_clock_names(node_count);
  for (std::size_t node = 0; node < node_count; ++node)
    source_clock_names[node] = input.clock_owner_by_node[node].value_or("");
  if (nodes.predecessor_nodes.presence != nodes.predecessor_arcs.presence) {
    reject(errors, ResultClass::kShape, "result.nodes.predecessor", "node and arc presence must match");
    return;
  }
  for (std::size_t node = 0; node < node_count; ++node) {
    const double arrival = nodes.arrivals.values[node];
    const double required = nodes.requireds.values[node];
    if (clock_names_ok && input.nodes[node].is_startpoint && nodes.clock_names.presence[node] != 1)
      reject(errors, ResultClass::kShape, "result.nodes.clock_names", "every startpoint requires a clock name");
    if (clock_names_ok && !input.nodes[node].is_startpoint && nodes.predecessor_nodes.presence[node] == 0 && !source_clock_names[node].empty()) {
      if (nodes.clock_names.presence[node] != 1)
        reject(errors, ResultClass::kShape, "result.nodes.clock_names", "clock-source node must carry its owning clock name");
      else if (nodes.clock_names.values[node] != source_clock_names[node])
        reject(errors, ResultClass::kIdMapping, "result.nodes.clock_names", "clock-source node must carry its owning clock name");
    }
    if (isFinite(arrival) && !input.nodes[node].is_startpoint && nodes.predecessor_nodes.presence[node] != 1)
      reject(errors, ResultClass::kPredecessor, "result.nodes.predecessor", "finite non-start arrival requires a predecessor");
    if (requireds_ok && slacks_ok) {
      const double expected_slack = isFinite(arrival) && isFinite(required) ? required - arrival : 0.0;
      if (nodes.slacks.values[node] != expected_slack)
        reject(errors, ResultClass::kNumeric, "result.nodes.slacks", "slack must equal required minus arrival, or canonical reset zero");
    }
    if (nodes.predecessor_nodes.presence[node] != 1) {
      if (input.nodes[node].is_startpoint) {
        const auto seed = std::ranges::find(input.start_seeds, NodeId{node}, &StartSeedModel::seed_node);
        if (seed != input.start_seeds.end()) {
          if (clock_names_ok && nodes.clock_names.presence[node] == 1 && nodes.clock_names.values[node] != seed->clock_tag)
            reject(errors, ResultClass::kIdMapping, "result.nodes.clock_names", "unreplaced startpoint clock name must equal its seed tag");
          if (seed->availability != StartSeedAvailability::kDeferredSequential) {
            const double expected_arrival = seed->path_source ? *seed->arrival[0][0] : 0.0;
            if (arrival != expected_arrival)
              reject(errors, ResultClass::kNumeric, "result.nodes.arrivals", "unreplaced startpoint arrival must equal its native max/rise seed");
          } else if (launches_ok) {
            const auto expected = sequentialLaunch(*seed, 0, effective);
            if (expected && nodes.launches.values[node] != *expected)
              reject(errors, ResultClass::kNumeric, "result.nodes.launches", "sequential launch differs from its effective clock lane");
          }
          if (launches_ok && seed->availability != StartSeedAvailability::kDeferredSequential) {
            double expected_launch = 0.0;
            const auto source_clock
                = std::ranges::find_if(input.clocks, [&](const ClockConstraintModel& clock) { return hasResolvedClockSource(clock, seed->seed_node); });
            if (source_clock != input.clocks.end())
              expected_launch = source_clock->edges[0];
            if (nodes.launches.values[node] != expected_launch)
              reject(errors, ResultClass::kNumeric, "result.nodes.launches", "unreplaced input start launch must equal its rise edge or zero fallback");
          }
        }
      } else {
        if (arrival != -std::numeric_limits<double>::infinity())
          reject(errors, ResultClass::kNumeric, "result.nodes.arrivals", "unreached non-start node must retain negative-infinity arrival");
        if (launches_ok && nodes.launches.values[node] != 0)
          reject(errors, ResultClass::kNumeric, "result.nodes.launches", "unreached non-start node must retain zero launch");
        if (clock_names_ok && source_clock_names[node].empty() && nodes.clock_names.presence[node] != 0)
          reject(errors, ResultClass::kShape, "result.nodes.clock_names", "unreached non-clock-source node must retain absent clock name");
      }
      continue;
    }
    if (nodes.arrivals.presence[node] != 1 || nodes.arrivals.values[node] == -std::numeric_limits<double>::infinity())
      reject(errors, ResultClass::kPredecessor, "result.nodes.predecessor", "present predecessor requires a computed arrival");
    const auto predecessor = nodes.predecessor_nodes.values[node].value();
    const auto arc = nodes.predecessor_arcs.values[node].value();
    if (predecessor >= node_count || arc >= input.arcs.size())
      continue;
    if (!input.arcs[arc].eligibility.data_arrival || input.arcs[arc].source.value() != predecessor || input.arcs[arc].sink.value() != node)
      reject(errors, ResultClass::kPredecessor, "result.nodes.predecessor", "predecessor Arc endpoints do not match the node");
    if (launches_ok && nodes.launches.values[node] != nodes.launches.values[predecessor])
      reject(errors, ResultClass::kPredecessor, "result.nodes.launches", "predecessor chain must preserve launch time");
    if (clock_names_ok
        && (nodes.clock_names.presence[node] != nodes.clock_names.presence[predecessor]
            || (nodes.clock_names.presence[node] == 1 && nodes.clock_names.values[node] != nodes.clock_names.values[predecessor])))
      reject(errors, ResultClass::kPredecessor, "result.nodes.clock_names", "predecessor chain must preserve clock name");
    if (!isFinite(nodes.arrivals.values[predecessor])) {
      reject(errors, ResultClass::kPredecessor, "result.nodes.arrivals", "present predecessor requires a finite upstream arrival");
    } else if (arc_delays_shape) {
      if (arcs.delays.presence[arc] != 1) {
        reject(errors, ResultClass::kPredecessor, "result.nodes.arrivals", "scalar arrival requires a selected Arc delay");
      } else {
        double expected_arrival = 0.0;
        if (!checkedFiniteAdd(nodes.arrivals.values[predecessor], arcs.delays.values[arc], expected_arrival))
          reject(errors, ResultClass::kNumeric, "result.nodes.arrivals", "scalar predecessor arrival plus selected Arc delay must remain finite");
        else if (nodes.arrivals.values[node] != expected_arrival)
          reject(errors, ResultClass::kPredecessor, "result.nodes.arrivals", "scalar arrival must equal upstream arrival plus selected Arc delay");
      }
    }
  }
}

inline void validateClockSoa(const ClockResultSoa& clock, const ArcResultSoa& arcs, const CsrReferenceInput& input, std::size_t lane_count, TimingResultViolations& errors,
                             const std::string& field, bool physical, const ClockResultSoa* physical_clock = nullptr)
{
  const bool arrivals_ok
      = validateOptionalSoa(clock.arrivals, lane_count, false, errors, ResultClass::kShape, field + ".arrivals", [&](double value, std::size_t) {
          if (!isFinite(value))
            reject(errors, ResultClass::kNumeric, field + ".arrivals", "present arrival must be finite");
        });
  const bool slews_ok = validateOptionalSoa(clock.slews, lane_count, false, errors, ResultClass::kShape, field + ".slews", [&](double value, std::size_t) {
    if (!isFinite(value) || value < 0)
      reject(errors, ResultClass::kNumeric, field + ".slews", "present slew must be finite and nonnegative");
  });
  const bool nodes_ok = validateOptionalSoa(clock.predecessor_nodes, lane_count, false, errors, ResultClass::kShape, field + ".predecessor_nodes",
                                            [&](NodeId value, std::size_t) {
                                              if (value.value() >= input.nodes.size())
                                                reject(errors, ResultClass::kIdMapping, field + ".predecessor_nodes", "NodeId is out of range");
                                            });
  const bool delays_ok = validateOptionalSoa(clock.predecessor_arc_delays, lane_count, false, errors, ResultClass::kShape, field + ".predecessor_arc_delays",
                                             [&](double value, std::size_t) {
                                               if (!isFinite(value))
                                                 reject(errors, ResultClass::kNumeric, field + ".predecessor_arc_delays", "delay must be finite");
                                             });
  const bool transitions_ok = validateOptionalSoa(
      clock.predecessor_transitions, lane_count, false, errors, ResultClass::kShape, field + ".predecessor_transitions", [&](Transition value, std::size_t) {
        if (!knownEnum(value) || value == Transition::kNone)
          reject(errors, ResultClass::kIdMapping, field + ".predecessor_transitions", "transition must be rise or fall");
      });
  if (!(arrivals_ok && slews_ok && nodes_ok && delays_ok && transitions_ok))
    return;
  if (clock.predecessor_nodes.presence != clock.predecessor_arc_delays.presence || clock.predecessor_nodes.presence != clock.predecessor_transitions.presence)
    reject(errors, ResultClass::kShape, field + ".predecessor", "clock predecessor presence arrays must match");
  std::size_t arc_lanes = 0;
  const bool arc_shape = checkedProduct(input.arcs.size(), kAnalysisCount * kTransitionCount * kTransitionCount,
                                        arc_lanes, errors, ResultClass::kShape, "result.arcs.input_output_delays")
                         && arcs.input_output_delays.presence.size() == arc_lanes;
  for (std::size_t lane = 0; lane < lane_count; ++lane) {
    const std::size_t node = lane / (kAnalysisCount * kTransitionCount);
    const bool owned = input.clock_owner_by_node[node].has_value();
    if (!physical) {
      const auto owner = owned ? std::ranges::find(input.clocks, *input.clock_owner_by_node[node], &ClockConstraintModel::map_key)
                               : input.clocks.end();
      if (owner != input.clocks.end() && owner->propagated) {
        const auto matches = [lane](const auto& actual, const auto& expected) {
          return lane < expected.presence.size() && lane < expected.values.size()
                 && actual.presence[lane] == expected.presence[lane]
                 && (!actual.presence[lane] || actual.values[lane] == expected.values[lane]);
        };
        if (physical_clock == nullptr || !matches(clock.arrivals, physical_clock->arrivals)
            || !matches(clock.slews, physical_clock->slews)
            || !matches(clock.predecessor_nodes, physical_clock->predecessor_nodes)
            || !matches(clock.predecessor_arc_delays, physical_clock->predecessor_arc_delays)
            || !matches(clock.predecessor_transitions, physical_clock->predecessor_transitions))
          reject(errors, ResultClass::kPredecessor, field, "propagated effective clock must preserve physical values and identity");
        continue;
      }
      const std::uint8_t expected_presence = owned ? 1 : 0;
      if (clock.arrivals.presence[lane] != expected_presence || clock.slews.presence[lane] != expected_presence)
        reject(errors, ResultClass::kShape, field, "effective ideal-clock lanes must exactly cover owned nodes");
      else if (owned && (clock.arrivals.values[lane] != 0 || clock.slews.values[lane] != 0))
        reject(errors, ResultClass::kNumeric, field, "effective ideal-clock arrival and slew must be zero");
      if (clock.predecessor_nodes.presence[lane] != 0)
        reject(errors, ResultClass::kPredecessor, field + ".predecessor", "effective ideal clocks must not have predecessors");
      continue;
    }
    const bool is_source = std::ranges::any_of(input.clocks, [&](const auto& constraint) { return hasResolvedClockSource(constraint, NodeId{node}); });
    if (is_source && (clock.arrivals.presence[lane] != 1 || clock.slews.presence[lane] != 1))
      reject(errors, ResultClass::kShape, field, "resolved clock source must retain physical arrival and slew in every lane");
    if (!owned && (clock.arrivals.presence[lane] || clock.slews.presence[lane] || clock.predecessor_nodes.presence[lane]))
      reject(errors, ResultClass::kShape, field, "physical clock state requires a replayed owner");
    if (clock.predecessor_nodes.presence[lane] == 0) {
      if (clock.arrivals.presence[lane]) {
        if (!is_source || clock.arrivals.values[lane] != 0.0)
          reject(errors, ResultClass::kPredecessor, field, "physical clock arrival without predecessor must be a zero source seed");
      }
      continue;
    }
    const auto predecessor = clock.predecessor_nodes.values[lane].value();
    const auto transition = static_cast<std::size_t>(clock.predecessor_transitions.values[lane]);
    if (predecessor >= input.nodes.size() || transition >= kTransitionCount)
      continue;
    const auto upstream = (predecessor * kAnalysisCount + (lane / kTransitionCount) % kAnalysisCount) * kTransitionCount + transition;
    bool edge_found = false;
    for (std::size_t index = input.in_row_offsets[node]; index < input.in_row_offsets[node + 1]; ++index) {
      const auto arc_id = input.in_arc_ids[index].value();
      const auto& arc = input.arcs[arc_id];
      const auto arc_lane = ((arc_id * kAnalysisCount + (lane / kTransitionCount) % kAnalysisCount) * kTransitionCount + transition)
                            * kTransitionCount + lane % kTransitionCount;
      edge_found |= arc.eligibility.clock && arc.source.value() == predecessor && arc_shape
                    && arcs.input_output_delays.presence[arc_lane] == 1;
    }
    double expected_arrival = 0.0;
    if (!owned || input.clock_owner_by_node[node] != input.clock_owner_by_node[predecessor] || !edge_found
        || clock.arrivals.presence[lane] != 1 || clock.arrivals.presence[upstream] != 1)
      reject(errors, ResultClass::kPredecessor, field, "physical predecessor must follow an owned clock edge with present arrivals");
    else if (!checkedRoundedPathArrival(clock.arrivals.values[upstream], clock.predecessor_arc_delays.values[lane], expected_arrival)
             || clock.arrivals.values[lane] != expected_arrival)
      reject(errors, ResultClass::kNumeric, field, "physical arrival differs from its predecessor plus recorded delay");
  }
}

template <typename T>
bool allPresent(const OptionalSoa<T>& values)
{
  return std::ranges::all_of(values.presence, [](std::uint8_t presence) { return presence == 1; });
}

struct PathLaneCoordinates
{
  std::size_t node;
  std::size_t analysis;
  std::size_t source;
  std::size_t transition;
};

inline PathLaneCoordinates decodePathLane(std::size_t row)
{
  PathLaneCoordinates lane;
  lane.transition = row % kTransitionCount;
  row /= kTransitionCount;
  lane.source = row % kPathSourceCount;
  row /= kPathSourceCount;
  lane.analysis = row % kAnalysisCount;
  lane.node = row / kAnalysisCount;
  return lane;
}

inline std::size_t encodePathLane(std::size_t node, std::size_t analysis, std::size_t source, std::size_t transition)
{
  return (((node * kAnalysisCount) + analysis) * kPathSourceCount + source) * kTransitionCount + transition;
}

inline std::optional<std::size_t> findPathEntry(const PathResultSoa& paths, std::size_t row, ClockTagId tag)
{
  const auto begin = paths.clock_tags.values.begin() + static_cast<std::size_t>(paths.row_offsets[row]);
  const auto end = paths.clock_tags.values.begin() + static_cast<std::size_t>(paths.row_offsets[row + 1]);
  const auto found = std::lower_bound(begin, end, tag.value(), [](ClockTagId candidate, std::uint64_t value) { return candidate.value() < value; });
  if (found == end || found->value() != tag.value())
    return std::nullopt;
  return static_cast<std::size_t>(std::distance(paths.clock_tags.values.begin(), found));
}

inline void validatePathRoot(const PathResultSoa& paths, const CsrReferenceInput& input, std::size_t entry, const PathLaneCoordinates& lane,
                             const ClockResultSoa& effective, TimingResultViolations& errors)
{
  const NodeId start = paths.start_nodes.values[entry];
  const ClockTagId tag_id = paths.clock_tags.values[entry];
  const std::string& tag = input.clock_tags[tag_id.value()];
  const PathSource source = input.path_source_order[lane.source];
  const StartSeedModel* matching_seed = nullptr;
  std::size_t matches = 0;
  for (const auto& seed : input.start_seeds) {
    if (seed.path_source && seed.seed_node.value() == lane.node && seed.path_start_node == start && *seed.path_source == source && seed.clock_tag == tag) {
      matching_seed = &seed;
      ++matches;
    }
  }
  if (matches != 1) {
    reject(errors, ResultClass::kPredecessor, "result.paths.predecessor", "root entry must resolve to exactly one start seed");
    return;
  }
  const bool result_has_crpr = paths.crpr_nodes.presence[entry] == 1;
  if (result_has_crpr != matching_seed->crpr_node.has_value()
      || (result_has_crpr
          && (paths.crpr_nodes.values[entry] != *matching_seed->crpr_node || paths.crpr_transitions.values[entry] != *matching_seed->crpr_transition)))
    reject(errors, ResultClass::kPredecessor, "result.paths.crpr", "root entry CRPR must exactly equal its start seed");
  if (matching_seed->availability == StartSeedAvailability::kDeferredSequential) {
    // The input stores raw CTQ tables, not computed seed values. Fresh native
    // comparison proves those values; this validator retains root/CRPR binding.
    const auto expected = sequentialLaunch(*matching_seed, lane.analysis, effective);
    if (expected && paths.launches.values[entry] != *expected)
      reject(errors, ResultClass::kPredecessor, "result.paths.launches", "register root launch differs from its effective clock lane");
  } else {
    const auto& seed_arrival = matching_seed->arrival[lane.analysis][lane.transition];
    const auto& seed_slew = matching_seed->slew[lane.analysis][lane.transition];
    if (!seed_arrival || !seed_slew || paths.arrivals.values[entry] != *seed_arrival || paths.slews.values[entry] != *seed_slew)
      reject(errors, ResultClass::kPredecessor, "result.paths.predecessor", "root entry arrival and slew must exactly equal its start seed lane");
  }
  if (source == PathSource::kInput) {
    const auto source_clock
        = std::ranges::find_if(input.clocks, [&](const ClockConstraintModel& clock) { return hasResolvedClockSource(clock, matching_seed->seed_node); });
    double expected_launch = 0.0;
    if (source_clock != input.clocks.end())
      expected_launch = lane.analysis == 0 && lane.transition == 1 ? source_clock->edges[1] : source_clock->edges[0];
    if (paths.launches.values[entry] != expected_launch)
      reject(errors, ResultClass::kPredecessor, "result.paths.launches", "input root launch must equal its native source-clock edge or zero fallback");
  }
}

inline void validateMaterializedSeedRoots(const PathResultSoa& paths, const CsrReferenceInput& input, TimingResultViolations& errors)
{
  for (const auto& seed : input.start_seeds) {
    if (!seed.path_source)
      continue;
    const auto source = std::ranges::find(input.path_source_order, *seed.path_source);
    const auto tag = std::ranges::lower_bound(input.clock_tags, seed.clock_tag, byteStringLess);
    if (source == input.path_source_order.end() || tag == input.clock_tags.end() || *tag != seed.clock_tag)
      continue;
    const std::size_t source_index = static_cast<std::size_t>(std::distance(input.path_source_order.begin(), source));
    const ClockTagId tag_id{static_cast<std::uint64_t>(std::distance(input.clock_tags.begin(), tag))};
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        if (seed.availability != StartSeedAvailability::kDeferredSequential && (!seed.arrival[analysis][transition] || !seed.slew[analysis][transition]))
          continue;
        const std::size_t row = encodePathLane(seed.seed_node.value(), analysis, source_index, transition);
        const auto entry = findPathEntry(paths, row, tag_id);
        if (!entry)
          reject(errors, ResultClass::kPredecessor, "result.paths", "every materialized seed lane requires its unique tag entry");
      }
    }
  }
}

inline void validatePathCausality(const PathResultSoa& paths, const ArcResultSoa& arc_results, const OptionalSoa<double>& data_slews,
                                  const CsrReferenceInput& input, const ClockResultSoa& effective, std::size_t lane_count, std::size_t entry_count, TimingResultViolations& errors)
{
  std::size_t graph_lanes = 0;
  const bool graph_shape = checkedProduct(input.arcs.size(), kAnalysisCount * kTransitionCount * kTransitionCount, graph_lanes, errors, ResultClass::kShape,
                                          "result.arcs.graph_delays")
                           && arc_results.graph_delays.presence.size() == graph_lanes && arc_results.graph_delays.values.size() == graph_lanes;
  std::size_t data_lanes = 0;
  const bool data_shape = checkedProduct(input.nodes.size(), kAnalysisCount * kTransitionCount, data_lanes, errors, ResultClass::kShape, "result.data_slews")
                          && data_slews.presence.size() == data_lanes && data_slews.values.size() == data_lanes;
  std::vector<std::size_t> entry_rows(entry_count, 0);
  for (std::size_t row = 0; row < lane_count; ++row)
    for (std::uint64_t entry = paths.row_offsets[row]; entry < paths.row_offsets[row + 1]; ++entry)
      entry_rows[entry] = row;

  std::vector<std::uint8_t> state(entry_count, 0);
  for (std::size_t origin = 0; origin < entry_count; ++origin) {
    if (state[origin] != 0)
      continue;
    std::vector<std::size_t> chain;
    std::size_t current = origin;
    while (true) {
      if (state[current] == 1) {
        reject(errors, ResultClass::kPredecessor, "result.paths.predecessor", "path predecessor chain contains a cycle");
        break;
      }
      if (state[current] == 2)
        break;
      state[current] = 1;
      chain.push_back(current);
      const PathLaneCoordinates lane = decodePathLane(entry_rows[current]);
      const std::size_t data_lane = ((lane.node * kAnalysisCount) + lane.analysis) * kTransitionCount + lane.transition;
      if (data_shape && (data_slews.presence[data_lane] != 1 || data_slews.values[data_lane] != paths.slews.values[current]))
        reject(errors, ResultClass::kNumeric, "result.paths.slews", "path slew must exactly equal the current node data-slew lane");
      if (paths.predecessor_nodes.presence[current] == 0) {
        validatePathRoot(paths, input, current, lane, effective, errors);
        break;
      }

      const NodeId predecessor = paths.predecessor_nodes.values[current];
      const Transition predecessor_transition = paths.predecessor_transitions.values[current];
      if (predecessor.value() >= input.nodes.size() || (predecessor_transition != Transition::kRise && predecessor_transition != Transition::kFall))
        break;
      const std::size_t transition = predecessor_transition == Transition::kRise ? 0 : 1;
      const std::size_t upstream_row = encodePathLane(predecessor.value(), lane.analysis, lane.source, transition);
      const auto upstream = findPathEntry(paths, upstream_row, paths.clock_tags.values[current]);
      if (!upstream) {
        reject(errors, ResultClass::kPredecessor, "result.paths.predecessor", "path predecessor entry does not resolve in the derived upstream lane");
        break;
      }
      if (paths.start_nodes.values[*upstream] != paths.start_nodes.values[current]) {
        reject(errors, ResultClass::kPredecessor, "result.paths.predecessor", "path predecessor chain changes its start node");
        break;
      }
      if (paths.launches.values[*upstream] != paths.launches.values[current]) {
        reject(errors, ResultClass::kPredecessor, "result.paths.launches", "path predecessor chain changes its launch time");
        break;
      }
      const std::size_t arc = paths.predecessor_arcs.values[current].value();
      const double predecessor_delay = paths.predecessor_arc_delays.values[current];
      const std::size_t graph_lane = (((arc * kAnalysisCount) + lane.analysis) * kTransitionCount + transition) * kTransitionCount + lane.transition;
      if (graph_shape && (arc_results.graph_delays.presence[graph_lane] != 1 || arc_results.graph_delays.values[graph_lane] != predecessor_delay))
        reject(errors, ResultClass::kPredecessor, "result.paths.predecessor_arc_delays", "path predecessor delay must equal the selected graph-delay lane");
      double expected_arrival = 0.0;
      if (!checkedRoundedPathArrival(paths.arrivals.values[*upstream], predecessor_delay, expected_arrival)) {
        reject(errors, ResultClass::kNumeric, "result.paths.arrivals", "rounded predecessor arrival plus Arc delay must remain finite");
      } else if (paths.arrivals.values[current] != expected_arrival) {
        reject(errors, ResultClass::kPredecessor, "result.paths.arrivals", "path arrival must equal the rounded predecessor arrival plus arc delay");
      }
      const bool upstream_has_crpr = paths.crpr_nodes.presence[*upstream] == 1;
      const bool current_has_crpr = paths.crpr_nodes.presence[current] == 1;
      if (upstream_has_crpr != current_has_crpr
          || (current_has_crpr
              && (paths.crpr_nodes.values[*upstream] != paths.crpr_nodes.values[current]
                  || paths.crpr_transitions.values[*upstream] != paths.crpr_transitions.values[current]))) {
        reject(errors, ResultClass::kPredecessor, "result.paths.crpr", "path predecessor chain changes its CRPR reference");
        break;
      }
      current = *upstream;
    }
    for (std::size_t entry : chain)
      state[entry] = 2;
  }
}

inline void validatePathSoa(const PathResultSoa& paths, const ArcResultSoa& arc_results, const OptionalSoa<double>& data_slews, const CsrReferenceInput& input,
                            const ClockResultSoa& effective, std::size_t lane_count, TimingResultViolations& errors)
{
  const std::size_t node_count = input.nodes.size();
  const std::size_t arc_count = input.arcs.size();
  const std::size_t entry_count = paths.clock_tags.values.size();
  const bool offsets_ok = validateOffsets(paths.row_offsets, lane_count, entry_count, errors, ResultClass::kShape, "result.paths.row_offsets");
  bool tag_domain_ok = true;
  const bool tags_ok
      = validateOptionalSoa(paths.clock_tags, entry_count, true, errors, ResultClass::kShape, "result.paths.clock_tags", [&](ClockTagId value, std::size_t) {
          if (value.value() >= input.clock_tags.size()) {
            reject(errors, ResultClass::kIdMapping, "result.paths.clock_tags", "ClockTagId is out of range");
            tag_domain_ok = false;
          }
        });
  const bool arrivals_ok
      = validateOptionalSoa(paths.arrivals, entry_count, true, errors, ResultClass::kShape, "result.paths.arrivals", [&](double value, std::size_t) {
          if (!isFinite(value))
            reject(errors, ResultClass::kNumeric, "result.paths.arrivals", "path entry arrival must be finite");
        });
  const bool slews_ok = validateOptionalSoa(paths.slews, entry_count, true, errors, ResultClass::kShape, "result.paths.slews", [&](double value, std::size_t) {
    if (!isFinite(value) || value < 0)
      reject(errors, ResultClass::kNumeric, "result.paths.slews", "slew must be finite and nonnegative");
  });
  const bool launches_ok
      = validateOptionalSoa(paths.launches, entry_count, true, errors, ResultClass::kShape, "result.paths.launches", [&](double value, std::size_t) {
          if (!isFinite(value))
            reject(errors, ResultClass::kNumeric, "result.paths.launches", "launch must be finite");
        });
  bool start_domain_ok = true;
  const bool starts_ok
      = validateOptionalSoa(paths.start_nodes, entry_count, true, errors, ResultClass::kShape, "result.paths.start_nodes", [&](NodeId value, std::size_t) {
          if (value.value() >= node_count) {
            reject(errors, ResultClass::kIdMapping, "result.paths.start_nodes", "start NodeId is out of range");
            start_domain_ok = false;
          }
        });
  bool predecessor_domain_ok = true;
  const bool nodes_ok = validateOptionalSoa(paths.predecessor_nodes, entry_count, false, errors, ResultClass::kShape, "result.paths.predecessor_nodes",
                                            [&](NodeId value, std::size_t) {
                                              if (value.value() >= node_count) {
                                                reject(errors, ResultClass::kIdMapping, "result.paths.predecessor_nodes", "NodeId is out of range");
                                                predecessor_domain_ok = false;
                                              }
                                            });
  const bool arcs_ok = validateOptionalSoa(paths.predecessor_arcs, entry_count, false, errors, ResultClass::kShape, "result.paths.predecessor_arcs",
                                           [&](ArcId value, std::size_t) {
                                             if (value.value() >= arc_count) {
                                               reject(errors, ResultClass::kIdMapping, "result.paths.predecessor_arcs", "ArcId is out of range");
                                               predecessor_domain_ok = false;
                                             }
                                           });
  const bool delays_ok = validateOptionalSoa(paths.predecessor_arc_delays, entry_count, false, errors, ResultClass::kShape,
                                             "result.paths.predecessor_arc_delays", [&](double value, std::size_t) {
                                               if (!isFinite(value))
                                                 reject(errors, ResultClass::kNumeric, "result.paths.predecessor_arc_delays", "delay must be finite");
                                             });
  const bool transitions_ok
      = validateOptionalSoa(paths.predecessor_transitions, entry_count, false, errors, ResultClass::kShape, "result.paths.predecessor_transitions",
                            [&](Transition value, std::size_t) {
                              if (!knownEnum(value) || value == Transition::kNone) {
                                reject(errors, ResultClass::kIdMapping, "result.paths.predecessor_transitions", "transition must be rise or fall");
                                predecessor_domain_ok = false;
                              }
                            });
  bool crpr_domain_ok = true;
  const bool crpr_nodes_ok
      = validateOptionalSoa(paths.crpr_nodes, entry_count, false, errors, ResultClass::kShape, "result.paths.crpr_nodes", [&](NodeId value, std::size_t) {
          if (value.value() >= node_count) {
            reject(errors, ResultClass::kIdMapping, "result.paths.crpr_nodes", "NodeId is out of range");
            crpr_domain_ok = false;
          }
        });
  const bool crpr_transitions_ok = validateOptionalSoa(
      paths.crpr_transitions, entry_count, false, errors, ResultClass::kShape, "result.paths.crpr_transitions", [&](Transition value, std::size_t) {
        if (!knownEnum(value) || value == Transition::kNone) {
          reject(errors, ResultClass::kIdMapping, "result.paths.crpr_transitions", "transition must be rise or fall");
          crpr_domain_ok = false;
        }
      });

  bool tags_ordered = offsets_ok && tags_ok && tag_domain_ok && allPresent(paths.clock_tags);
  if (tags_ordered) {
    for (std::size_t row = 0; row < lane_count; ++row) {
      for (std::uint64_t entry = paths.row_offsets[row] + 1; entry < paths.row_offsets[row + 1]; ++entry) {
        if (paths.clock_tags.values[entry - 1].value() >= paths.clock_tags.values[entry].value()) {
          reject(errors, ResultClass::kIdMapping, "result.paths.clock_tags", "ClockTagIds must be strictly increasing within each lane");
          tags_ordered = false;
        }
      }
    }
  }
  const bool predecessor_shape_ok = nodes_ok && arcs_ok && delays_ok && transitions_ok;
  const bool predecessor_presence_ok = predecessor_shape_ok && paths.predecessor_nodes.presence == paths.predecessor_arcs.presence
                                       && paths.predecessor_nodes.presence == paths.predecessor_arc_delays.presence
                                       && paths.predecessor_nodes.presence == paths.predecessor_transitions.presence;
  if (predecessor_shape_ok && !predecessor_presence_ok)
    reject(errors, ResultClass::kShape, "result.paths.predecessor", "path predecessor presence arrays must match");
  if (offsets_ok && predecessor_presence_ok && predecessor_domain_ok && arrivals_ok) {
    for (std::size_t row = 0; row < lane_count; ++row) {
      const std::size_t node = decodePathLane(row).node;
      for (std::uint64_t entry = paths.row_offsets[row]; entry < paths.row_offsets[row + 1]; ++entry) {
        if (paths.predecessor_nodes.presence[entry] != 1)
          continue;
        if (paths.arrivals.presence[entry] != 1 || paths.arrivals.values[entry] == -std::numeric_limits<double>::infinity())
          reject(errors, ResultClass::kPredecessor, "result.paths.predecessor", "present predecessor requires a computed path arrival");
        const auto predecessor = paths.predecessor_nodes.values[entry].value();
        const auto arc = paths.predecessor_arcs.values[entry].value();
        if (!input.arcs[arc].eligibility.data_arrival || input.arcs[arc].source.value() != predecessor || input.arcs[arc].sink.value() != node)
          reject(errors, ResultClass::kPredecessor, "result.paths.predecessor", "predecessor Arc endpoints do not match the lane node");
      }
    }
  }
  const bool crpr_presence_ok = crpr_nodes_ok && crpr_transitions_ok && paths.crpr_nodes.presence == paths.crpr_transitions.presence;
  if (crpr_nodes_ok && crpr_transitions_ok && !crpr_presence_ok)
    reject(errors, ResultClass::kShape, "result.paths.crpr", "CRPR node and transition presence must match");

  const bool required_ok = tags_ok && arrivals_ok && slews_ok && launches_ok && starts_ok && allPresent(paths.clock_tags) && allPresent(paths.arrivals)
                           && allPresent(paths.slews) && allPresent(paths.launches) && allPresent(paths.start_nodes);
  if (offsets_ok && required_ok && tag_domain_ok && start_domain_ok && tags_ordered && predecessor_presence_ok && predecessor_domain_ok && crpr_presence_ok
      && crpr_domain_ok) {
    validateMaterializedSeedRoots(paths, input, errors);
    validatePathCausality(paths, arc_results, data_slews, input, effective, lane_count, entry_count, errors);
  }
}

}  // namespace

detail::TimingResultViolations detail::validateTimingResultClassified(const CsrReferenceInput& input, const CsrTimingResult& result)
{
  TimingResultViolations errors;
  if (result.schema_version != kCsrSchemaVersion)
    reject(errors, ResultClass::kSchema, "result.schema_version", "unsupported schema version");
  auto identity_errors = validateSnapshotIdentityClassified(result.source_identity, "result.source_identity", true);
  errors.insert(errors.end(), std::make_move_iterator(identity_errors.begin()), std::make_move_iterator(identity_errors.end()));
  if (result.source_identity.fingerprint_schema_version != input.source_identity.fingerprint_schema_version) {
    reject(errors, ResultClass::kSchema, "result.source_identity", "result identity differs from input");
  }
  if (result.source_identity.session_token != input.source_identity.session_token) {
    reject(errors, ResultClass::kSession, "result.source_identity", "result identity differs from input");
  }
  if (result.source_identity.graph_generation != input.source_identity.graph_generation) {
    reject(errors, ResultClass::kGeneration, "result.source_identity", "result identity differs from input");
  }
  if (result.source_identity.topology_fingerprint != input.source_identity.topology_fingerprint
      || result.source_identity.clock_constraint_fingerprint != input.source_identity.clock_constraint_fingerprint
      || result.source_identity.library_fingerprint != input.source_identity.library_fingerprint
      || result.source_identity.corner_fingerprint != input.source_identity.corner_fingerprint
      || result.source_identity.parasitic_fingerprint != input.source_identity.parasitic_fingerprint
      || result.source_identity.timing_input_fingerprint != input.source_identity.timing_input_fingerprint) {
    reject(errors, ResultClass::kSourceIdentity, "result.source_identity", "result identity differs from input");
  }

  const std::size_t node_count = input.nodes.size();
  const CsrFifoLevelization expected = replayFifoLevelization(input);
  if (!expected.complete)
    reject(errors, ResultClass::kShape, "result.levels", "eligible graph contains an active cycle");
  if (result.level_offsets != expected.level_offsets || result.level_nodes != expected.level_nodes
      || result.node_to_level != expected.node_to_level)
    reject(errors, ResultClass::kShape, "result.levels", "levels and timing order must exactly match FIFO Kahn replay");
  for (NodeId root : input.ordered_root_ids)
    if (result.node_to_level.size() == node_count && result.node_to_level[root.value()] != 1)
      reject(errors, ResultClass::kShape, "result.node_to_level", "every ordered root must be at level one");
  if (result.node_to_level.size() == node_count) {
    for (const auto& arc : input.arcs)
      if (arc.eligibility.levelization && result.node_to_level[arc.sink.value()] <= result.node_to_level[arc.source.value()])
        reject(errors, ResultClass::kShape, "result.node_to_level", "eligible arc must advance level");
  }

  validateNodeResultSoa(result.nodes, result.arcs, input, result.effective_clock, errors);

  std::size_t clock_lanes = 0;
  checkedProduct(node_count, kAnalysisCount * kTransitionCount, clock_lanes, errors, ResultClass::kShape, "result.clock");
  validateClockSoa(result.effective_clock, result.arcs, input, clock_lanes, errors, "result.effective_clock", false, &result.physical_clock);
  validateClockSoa(result.physical_clock, result.arcs, input, clock_lanes, errors, "result.physical_clock", true);
  const bool data_slews_ok
      = validateOptionalSoa(result.data_slews, clock_lanes, false, errors, ResultClass::kShape, "result.data_slews", [&](double value, std::size_t) {
          if (!isFinite(value) || value < 0)
            reject(errors, ResultClass::kNumeric, "result.data_slews", "present slew must be finite and nonnegative");
        });
  if (data_slews_ok) {
    for (const auto& seed : input.start_seeds) {
      for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
        for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
          const std::size_t lane = (seed.seed_node.value() * kAnalysisCount + analysis) * kTransitionCount + transition;
          if (result.data_slews.presence[lane] != 1
              || (seed.availability != StartSeedAvailability::kDeferredSequential && result.data_slews.values[lane] != *seed.slew[analysis][transition]))
            reject(errors, ResultClass::kNumeric, "result.data_slews", "supported startpoint data slew must exactly equal its seed lane");
        }
      }
    }
  }

  std::size_t path_lanes = 0;
  checkedProduct(node_count, kAnalysisCount * kPathSourceCount * kTransitionCount, path_lanes, errors, ResultClass::kShape, "result.paths");
  validatePathSoa(result.paths, result.arcs, result.data_slews, input, result.effective_clock, path_lanes, errors);

  validateArcResultSoa(input, result.arcs, result.data_slews, result.physical_clock.slews, errors);
  return errors;
}

std::vector<CsrContractViolation> validateTimingResult(const CsrReferenceInput& input, const CsrTimingResult& result)
{
  if (!validateReferenceInput(input).empty())
    return {{"input", "cannot validate a result against malformed reference input"}};
  if (!input.unsupported_reasons.empty())
    return {{"input.unsupported_reasons", "unsupported reference input must not produce a timing result"}};

  auto classified = detail::validateTimingResultClassified(input, result);
  std::vector<CsrContractViolation> errors;
  errors.reserve(classified.size());
  for (auto& error : classified)
    errors.push_back({std::move(error.field), std::move(error.message)});
  return errors;
}

}  // namespace ista::csr
