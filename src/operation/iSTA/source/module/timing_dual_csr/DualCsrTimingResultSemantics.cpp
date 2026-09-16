// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under MulanPSL v2.
// ***************************************************************************************
#include "DualCsrTimingResultSemantics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "CsrReferenceValidatorDetail.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

bool containsNode(const std::vector<csr::NodeId>& nodes,
                  csr::NodeId target)
{
  return std::ranges::find(nodes, target) != nodes.end();
}

const csr::ArcSemanticPayload* activeArc(
    const DualCsrState& state, csr::ArcId id, const std::string& field,
    DualCsrViolations& violations)
{
  if (id.value() >= state.arc_slots.size()
      || !state.arc_slots[id.value()].active) {
    rejectSemantic(violations, field,
                   "predecessor ArcId must identify an active stable slot");
    return nullptr;
  }
  return &state.arc_slots[id.value()].payload;
}

struct PathCoordinates
{
  std::size_t node{0};
  std::size_t analysis{0};
  std::size_t source{0};
  std::size_t transition{0};
};

PathCoordinates decodePathRow(std::size_t row) noexcept
{
  PathCoordinates result;
  result.transition = row % csr::kTransitionCount;
  row /= csr::kTransitionCount;
  result.source = row % csr::kPathSourceCount;
  row /= csr::kPathSourceCount;
  result.analysis = row % csr::kAnalysisCount;
  result.node = row / csr::kAnalysisCount;
  return result;
}

std::size_t encodePathRow(std::size_t node, std::size_t analysis,
                          std::size_t source,
                          std::size_t transition) noexcept
{
  return (((node * csr::kAnalysisCount) + analysis)
           * csr::kPathSourceCount
          + source)
             * csr::kTransitionCount
         + transition;
}

std::optional<std::size_t> findPathEntry(const csr::PathResultSoa& paths,
                                         std::size_t row,
                                         csr::ClockTagId tag)
{
  for (std::uint64_t entry = paths.row_offsets[row];
       entry < paths.row_offsets[row + 1]; ++entry)
    if (paths.clock_tags.values[entry] == tag)
      return static_cast<std::size_t>(entry);
  return std::nullopt;
}

void validateNodeDomainsAndCausality(const DualCsrState& state,
                                     const csr::CsrTimingResult& result,
                                     DualCsrViolations& violations)
{
  const std::size_t count = state.nodes.size();
  const auto& nodes = result.nodes;
  const bool clock_points_ok = validatePresentDomain(
      nodes.is_clock_point, count, "result.nodes.is_clock_point",
      [](std::uint8_t value) { return value <= 1; },
      "clock-point value must be zero or one", violations);
  const bool arrivals_ok = validatePresentDomain(
      nodes.arrivals, count, "result.nodes.arrivals",
      csr::detail::validArrival,
      "arrival must be finite or negative infinity", violations);
  const bool requireds_ok = validatePresentDomain(
      nodes.requireds, count, "result.nodes.requireds",
      csr::detail::validRequired,
      "required must be finite or positive infinity", violations);
  const bool slacks_ok = validatePresentDomain(
      nodes.slacks, count, "result.nodes.slacks", csr::detail::isFinite,
      "slack must be finite", violations);
  const bool launches_ok = validatePresentDomain(
      nodes.launches, count, "result.nodes.launches", csr::detail::isFinite,
      "launch must be finite", violations);
  const bool names_ok = validatePresentDomain(
      nodes.clock_names, count, "result.nodes.clock_names",
      [&](const std::string& value) {
        return !value.empty()
               && std::ranges::find(state.reference_payload.clock_tags,
                                    value)
                      != state.reference_payload.clock_tags.end();
      }, "present clock name must resolve to a clock tag", violations);
  const bool predecessor_nodes_ok = validatePresentDomain(
      nodes.predecessor_nodes, count, "result.nodes.predecessor_nodes",
      [&](csr::NodeId value) { return value.value() < count; },
      "predecessor NodeId is out of range", violations);
  const bool predecessor_arcs_ok
      = hasShape(nodes.predecessor_arcs, count);
  if (!(arrivals_ok && requireds_ok && slacks_ok && launches_ok
        && names_ok && predecessor_nodes_ok && predecessor_arcs_ok))
    return;

  if (clock_points_ok) {
    for (std::size_t node = 0; node < count; ++node) {
      const bool expected = state.reference_payload.clock_owner_by_node[node].has_value();
      if (nodes.is_clock_point.values[node]
          != static_cast<std::uint8_t>(expected))
        rejectSemantic(violations, "result.nodes.is_clock_point",
                       "clock points must equal replayed clock ownership");
    }
  }
  if (nodes.predecessor_nodes.presence
      != nodes.predecessor_arcs.presence) {
    rejectSemantic(violations, "result.nodes.predecessor",
                   "node and arc predecessor presence must match");
    return;
  }

  const bool arc_delay_shape
      = hasShape(result.arcs.delays, state.arc_slots.size());
  for (std::size_t node = 0; node < count; ++node) {
    const double arrival = nodes.arrivals.values[node];
    const double required = nodes.requireds.values[node];
    const double expected_slack
        = csr::detail::isFinite(arrival)
                  && csr::detail::isFinite(required)
              ? required - arrival
              : 0.0;
    if (nodes.slacks.values[node] != expected_slack)
      rejectSemantic(violations, "result.nodes.slacks",
                     "slack must equal required minus arrival or zero reset");

    if (nodes.predecessor_nodes.presence[node] == 0) {
      if (!containsNode(state.reference_payload.ordered_start_ids,
                        csr::NodeId{node})
          && csr::detail::isFinite(arrival))
        rejectSemantic(violations, "result.nodes.predecessor",
                       "finite non-start arrival requires a predecessor");
      continue;
    }

    const csr::NodeId predecessor
        = nodes.predecessor_nodes.values[node];
    const csr::ArcId arc_id = nodes.predecessor_arcs.values[node];
    const auto* arc = activeArc(state, arc_id,
                                "result.nodes.predecessor_arcs",
                                violations);
    if (predecessor.value() >= count || arc == nullptr)
      continue;
    if (!arc->eligibility.data_arrival || arc->source != predecessor
        || arc->sink != csr::NodeId{node})
      rejectSemantic(violations, "result.nodes.predecessor",
                     "predecessor Arc endpoints or eligibility do not match");
    if (!csr::detail::isFinite(arrival)
        || !csr::detail::isFinite(nodes.arrivals.values[predecessor.value()]))
      rejectSemantic(violations, "result.nodes.arrivals",
                     "present predecessor requires finite arrivals");
    if (nodes.launches.values[node]
        != nodes.launches.values[predecessor.value()])
      rejectSemantic(violations, "result.nodes.launches",
                     "predecessor chain must preserve launch time");
    if (nodes.clock_names.presence[node]
            != nodes.clock_names.presence[predecessor.value()]
        || (nodes.clock_names.presence[node] == 1
            && nodes.clock_names.values[node]
                   != nodes.clock_names.values[predecessor.value()]))
      rejectSemantic(violations, "result.nodes.clock_names",
                     "predecessor chain must preserve clock name");
    if (!arc_delay_shape || arc_id.value() >= result.arcs.delays.values.size()
        || result.arcs.delays.presence[arc_id.value()] != 1
        || !csr::detail::isFinite(
            result.arcs.delays.values[arc_id.value()]))
      continue;
    const double expected_arrival
        = nodes.arrivals.values[predecessor.value()]
          + result.arcs.delays.values[arc_id.value()];
    if (!csr::detail::isFinite(expected_arrival)
        || arrival != expected_arrival)
      rejectSemantic(violations, "result.nodes.arrivals",
                     "arrival must equal predecessor arrival plus Arc delay");
  }

  if (!arc_delay_shape)
    return;
  double default_required = 0.0;
  for (const csr::NodeId endpoint
       : state.reference_payload.ordered_end_ids) {
    if (endpoint.value() < count
        && csr::detail::isFinite(nodes.arrivals.values[endpoint.value()]))
      default_required = std::max(
          default_required, nodes.arrivals.values[endpoint.value()]);
  }
  std::vector<double> expected_required(
      count, std::numeric_limits<double>::infinity());
  for (const csr::NodeId endpoint
       : state.reference_payload.ordered_end_ids)
    if (endpoint.value() < count)
      expected_required[endpoint.value()] = default_required;
  for (const auto& endpoint : state.reference_payload.endpoint_required)
    if (endpoint.node.value() < count && endpoint.output_delay[0])
      expected_required[endpoint.node.value()]
          = endpoint.clock_period - *endpoint.output_delay[0];
  std::vector<bool> check_seeded(count, false);
  for (const auto& endpoint : state.reference_payload.timing_check_endpoints) {
    if (!endpoint.resolved_data_node
        || (endpoint.check_kind != csr::TimingCheckKind::kSetup && endpoint.check_kind != csr::TimingCheckKind::kRecovery))
      continue;
    const auto node = endpoint.resolved_data_node->value();
    if (node < count && !state.nodes[node].is_port && state.nodes[node].is_sequential && state.nodes[node].is_endpoint) {
      check_seeded[node] = true;
      expected_required[node] = std::numeric_limits<double>::infinity();
    }
  }
  for (std::size_t id = 0; id < state.arc_slots.size(); ++id) {
    const auto& slot = state.arc_slots[id];
    if (!slot.active || !slot.payload.eligibility.data_arrival
        || result.arcs.delays.presence[id] != 1)
      continue;
    const std::size_t source = slot.payload.source.value();
    const std::size_t sink = slot.payload.sink.value();
    if (!csr::detail::isFinite(nodes.requireds.values[sink]))
      continue;
    const double candidate
        = nodes.requireds.values[sink] - result.arcs.delays.values[id];
    if (!csr::detail::isFinite(candidate)) {
      rejectSemantic(violations, "result.nodes.requireds",
                     "backward required candidate must remain finite");
      continue;
    }
    expected_required[source]
        = std::min(expected_required[source], candidate);
  }
  for (std::size_t node = 0; node < count; ++node)
    // A raw setup/recovery table has no computed seed in the input. Fresh
    // native proves that value; every other node retains the local min equality.
    if (check_seeded[node] ? nodes.requireds.values[node] > expected_required[node]
                          : nodes.requireds.values[node] != expected_required[node])
      rejectSemantic(violations, "result.nodes.requireds",
                     "required must satisfy its endpoint seed/backward minimum");
}

void validatePathDomainsAndCausality(const DualCsrState& state,
                                     const csr::CsrTimingResult& result,
                                     DualCsrViolations& violations)
{
  const auto& paths = result.paths;
  const std::size_t rows = state.nodes.size() * csr::kAnalysisCount
                           * csr::kPathSourceCount * csr::kTransitionCount;
  if (paths.row_offsets.size() != rows + 1 || paths.row_offsets.empty()
      || paths.row_offsets.front() != 0
      || !std::ranges::is_sorted(paths.row_offsets))
    return;
  const std::size_t entries = paths.row_offsets.back();
  const bool tags_ok = validatePresentDomain(
      paths.clock_tags, entries, "result.paths.clock_tags",
      [&](csr::ClockTagId value) {
        return value.value() < state.reference_payload.clock_tags.size();
      }, "path ClockTagId is out of range", violations);
  const bool arrivals_ok = validatePresentDomain(
      paths.arrivals, entries, "result.paths.arrivals",
      csr::detail::isFinite, "path arrival must be finite", violations);
  const bool slews_ok = validatePresentDomain(
      paths.slews, entries, "result.paths.slews",
      [](double value) {
        return csr::detail::isFinite(value) && value >= 0.0;
      }, "path slew must be finite and nonnegative", violations);
  const bool launches_ok = validatePresentDomain(
      paths.launches, entries, "result.paths.launches",
      csr::detail::isFinite, "path launch must be finite", violations);
  const bool starts_ok = validatePresentDomain(
      paths.start_nodes, entries, "result.paths.start_nodes",
      [&](csr::NodeId value) { return value.value() < state.nodes.size(); },
      "path start NodeId is out of range", violations);
  const bool predecessor_nodes_ok = validatePresentDomain(
      paths.predecessor_nodes, entries, "result.paths.predecessor_nodes",
      [&](csr::NodeId value) { return value.value() < state.nodes.size(); },
      "path predecessor NodeId is out of range", violations);
  const bool predecessor_arcs_ok
      = hasShape(paths.predecessor_arcs, entries);
  const bool predecessor_delays_ok = validatePresentDomain(
      paths.predecessor_arc_delays, entries,
      "result.paths.predecessor_arc_delays", csr::detail::isFinite,
      "path predecessor delay must be finite", violations);
  const bool predecessor_transitions_ok = validatePresentDomain(
      paths.predecessor_transitions, entries,
      "result.paths.predecessor_transitions",
      [](csr::Transition value) {
        return value == csr::Transition::kRise
               || value == csr::Transition::kFall;
      }, "path predecessor transition must be rise or fall", violations);
  const bool crpr_nodes_ok = validatePresentDomain(
      paths.crpr_nodes, entries, "result.paths.crpr_nodes",
      [&](csr::NodeId value) { return value.value() < state.nodes.size(); },
      "CRPR NodeId is out of range", violations);
  const bool crpr_transitions_ok = validatePresentDomain(
      paths.crpr_transitions, entries, "result.paths.crpr_transitions",
      [](csr::Transition value) {
        return value == csr::Transition::kRise
               || value == csr::Transition::kFall;
      }, "CRPR transition must be rise or fall", violations);
  if (!(tags_ok && arrivals_ok && slews_ok && launches_ok && starts_ok
        && predecessor_nodes_ok && predecessor_arcs_ok
        && predecessor_delays_ok && predecessor_transitions_ok
        && crpr_nodes_ok && crpr_transitions_ok))
    return;
  if (paths.predecessor_nodes.presence != paths.predecessor_arcs.presence
      || paths.predecessor_nodes.presence
             != paths.predecessor_arc_delays.presence
      || paths.predecessor_nodes.presence
             != paths.predecessor_transitions.presence)
    rejectSemantic(violations, "result.paths.predecessor",
                   "path predecessor presence arrays must match");
  if (paths.crpr_nodes.presence != paths.crpr_transitions.presence)
    rejectSemantic(violations, "result.paths.crpr",
                   "CRPR presence arrays must match");

  std::vector<std::size_t> entry_rows(entries, 0);
  for (std::size_t row = 0; row < rows; ++row) {
    for (std::uint64_t entry = paths.row_offsets[row];
         entry < paths.row_offsets[row + 1]; ++entry) {
      entry_rows[entry] = row;
      if (entry != paths.row_offsets[row]
          && paths.clock_tags.values[entry - 1].value()
                 >= paths.clock_tags.values[entry].value())
        rejectSemantic(violations, "result.paths.clock_tags",
                       "clock tags must be strictly ordered per row");
    }
  }
  const std::size_t data_lanes = state.nodes.size() * csr::kAnalysisCount
                                 * csr::kTransitionCount;
  const bool data_shape = hasShape(result.data_slews, data_lanes);
  const std::size_t graph_lanes = state.arc_slots.size()
                                  * csr::kAnalysisCount
                                  * csr::kTransitionCount
                                  * csr::kTransitionCount;
  const bool graph_shape = hasShape(result.arcs.graph_delays, graph_lanes);

  for (std::size_t entry = 0; entry < entries; ++entry) {
    const PathCoordinates lane = decodePathRow(entry_rows[entry]);
    const std::size_t data_lane
        = ((lane.node * csr::kAnalysisCount) + lane.analysis)
          * csr::kTransitionCount
          + lane.transition;
    if (data_shape
        && (result.data_slews.presence[data_lane] != 1
            || result.data_slews.values[data_lane]
                   != paths.slews.values[entry]))
      rejectSemantic(violations, "result.paths.slews",
                     "path slew must equal the current data-slew lane");
    if (paths.predecessor_nodes.presence[entry] == 0) {
      const std::string& tag = state.reference_payload.clock_tags[
          paths.clock_tags.values[entry].value()];
      const csr::PathSource source
          = state.reference_payload.path_source_order[lane.source];
      const csr::StartSeedModel* match = nullptr;
      std::size_t matches = 0;
      for (const auto& seed : state.reference_payload.start_seeds)
        if (seed.path_source && seed.seed_node == csr::NodeId{lane.node}
            && seed.path_start_node == paths.start_nodes.values[entry]
            && *seed.path_source == source && seed.clock_tag == tag) {
          match = &seed;
          ++matches;
        }
      if (matches != 1) {
        rejectSemantic(violations, "result.paths.predecessor",
                       "root path must resolve to exactly one start seed");
      } else if (match->availability == csr::StartSeedAvailability::kDeferredSequential) {
        if (source != csr::PathSource::kRegister || paths.launches.values[entry] != 0.0)
          rejectSemantic(violations, "result.paths.launches", "deferred register root requires zero ideal-clock launch");
        if (!match->crpr_node || !match->crpr_transition || paths.crpr_nodes.presence[entry] != 1
            || paths.crpr_transitions.presence[entry] != 1 || paths.crpr_nodes.values[entry] != *match->crpr_node
            || paths.crpr_transitions.values[entry] != *match->crpr_transition)
          rejectSemantic(violations, "result.paths.crpr", "deferred register root must retain its exact seed CRPR binding");
      } else if (!match->arrival[lane.analysis][lane.transition]
                 || !match->slew[lane.analysis][lane.transition]
                 || paths.arrivals.values[entry]
                        != *match->arrival[lane.analysis][lane.transition]
                 || paths.slews.values[entry]
                        != *match->slew[lane.analysis][lane.transition]) {
        rejectSemantic(violations, "result.paths.predecessor",
                       "root path must equal its start-seed lane");
      }
      continue;
    }

    const csr::NodeId predecessor = paths.predecessor_nodes.values[entry];
    const csr::ArcId arc_id = paths.predecessor_arcs.values[entry];
    const auto* arc = activeArc(state, arc_id,
                                "result.paths.predecessor_arcs",
                                violations);
    if (arc == nullptr)
      continue;
    if (!arc->eligibility.data_arrival || arc->source != predecessor
        || arc->sink != csr::NodeId{lane.node})
      rejectSemantic(violations, "result.paths.predecessor",
                     "path predecessor Arc endpoints or eligibility do not match");
    const csr::Transition predecessor_transition
        = paths.predecessor_transitions.values[entry];
    const std::size_t predecessor_transition_index
        = predecessor_transition == csr::Transition::kRise ? 0 : 1;
    const std::size_t graph_lane
        = (((arc_id.value() * csr::kAnalysisCount) + lane.analysis)
           * csr::kTransitionCount
          + predecessor_transition_index)
             * csr::kTransitionCount
         + lane.transition;
    if (graph_shape
        && (result.arcs.graph_delays.presence[graph_lane] != 1
            || result.arcs.graph_delays.values[graph_lane]
                   != paths.predecessor_arc_delays.values[entry]))
      rejectSemantic(violations, "result.paths.predecessor_arc_delays",
                     "path predecessor delay must equal graph-delay lane");
    const std::size_t upstream_row = encodePathRow(
        predecessor.value(), lane.analysis, lane.source,
        predecessor_transition_index);
    const auto upstream = findPathEntry(
        paths, upstream_row, paths.clock_tags.values[entry]);
    if (!upstream) {
      rejectSemantic(violations, "result.paths.predecessor",
                     "path predecessor does not resolve upstream");
      continue;
    }
    if (paths.start_nodes.values[*upstream]
            != paths.start_nodes.values[entry]
        || paths.launches.values[*upstream]
               != paths.launches.values[entry])
      rejectSemantic(violations, "result.paths.predecessor",
                     "path predecessor must preserve start and launch");
    double expected_arrival = 0.0;
    if (!checkedRoundedPathArrival(
            paths.arrivals.values[*upstream],
            paths.predecessor_arc_delays.values[entry], expected_arrival)
        || paths.arrivals.values[entry] != expected_arrival)
      rejectSemantic(violations, "result.paths.arrivals",
                     "path arrival must follow rounded predecessor recurrence");
    if (paths.crpr_nodes.presence[*upstream]
            != paths.crpr_nodes.presence[entry]
        || (paths.crpr_nodes.presence[entry] == 1
            && (paths.crpr_nodes.values[*upstream]
                    != paths.crpr_nodes.values[entry]
                || paths.crpr_transitions.values[*upstream]
                       != paths.crpr_transitions.values[entry])))
      rejectSemantic(violations, "result.paths.crpr",
                     "path predecessor must preserve CRPR reference");
  }
}

void validateArcDomains(const DualCsrState& state,
                        const DualCsrTimingResult& result,
                        DualCsrViolations& violations)
{
  const auto& arcs = result.values.arcs;
  const std::size_t count = state.arc_slots.size();
  const auto finite = [](double value) {
    return csr::detail::isFinite(value);
  };
  const bool delays_ok = validatePresentDomain(
      arcs.delays, count, "result.arcs.delays", finite,
      "present Arc delay must be finite", violations);
  const bool min_ok = validatePresentDomain(
      arcs.min_delays, count, "result.arcs.min_delays", finite,
      "present minimum delay must be finite", violations);
  const bool max_ok = validatePresentDomain(
      arcs.max_delays, count, "result.arcs.max_delays", finite,
      "present maximum delay must be finite", violations);
  const std::size_t transition_lanes
      = count * csr::kAnalysisCount * csr::kTransitionCount;
  const bool transitions_ok = validatePresentDomain(
      arcs.transition_delays, transition_lanes,
      "result.arcs.transition_delays", finite,
      "present transition delay must be finite", violations);
  const std::size_t graph_lanes
      = transition_lanes * csr::kTransitionCount;
  validatePresentDomain(arcs.input_output_delays, graph_lanes,
                        "result.arcs.input_output_delays", finite,
                        "present input/output delay must be finite",
                        violations);
  validatePresentDomain(arcs.graph_delays, graph_lanes,
                        "result.arcs.graph_delays", finite,
                        "present graph delay must be finite", violations);
  validatePresentDomain(
      arcs.transition_types, count * csr::kTransitionCount,
      "result.arcs.transition_types",
      [](csr::Transition value) {
        return value == csr::Transition::kRise
               || value == csr::Transition::kFall;
      }, "present transition type must be rise or fall", violations);
  validatePresentDomain(arcs.timing_arc_delays,
                        arcs.timing_arc_delays.values.size(),
                        "result.arcs.timing_arc_delays", finite,
                        "present child Arc delay must be finite", violations);
  validatePresentDomain(
      arcs.timing_arc_fallbacks, arcs.timing_arc_fallbacks.values.size(),
      "result.arcs.timing_arc_fallbacks",
      [](std::uint8_t value) { return value <= 1; },
      "child Arc fallback must be zero or one", violations);
  if (!(delays_ok && min_ok && max_ok && transitions_ok))
    return;
  for (std::size_t arc = 0; arc < count; ++arc) {
    if (!state.arc_slots[arc].active)
      continue;
    const std::size_t base
        = arc * csr::kAnalysisCount * csr::kTransitionCount;
    const double expected_max = std::max(
        arcs.transition_delays.values[base],
        arcs.transition_delays.values[base + 1]);
    const double expected_min = std::min(
        arcs.transition_delays.values[base + csr::kTransitionCount],
        arcs.transition_delays.values[
            base + csr::kTransitionCount + 1]);
    if (arcs.delays.values[arc] != expected_max
        || arcs.max_delays.values[arc] != expected_max
        || arcs.min_delays.values[arc] != expected_min)
      rejectSemantic(violations, "result.arcs.delays",
                     "scalar delays must equal transition-delay extrema");
  }
}

}  // namespace

void validateDualCsrTimingSemantics(const DualCsrState& state,
                                    const DualCsrTimingResult& result,
                                    DualCsrViolations& violations)
{
  validateNodeDomainsAndCausality(state, result.values, violations);
  validateClockAndSlewDomains(state, result.values, violations);
  validatePathDomainsAndCausality(state, result.values, violations);
  validateArcDomains(state, result, violations);
}

}  // namespace ista::timing_dual_csr::detail
