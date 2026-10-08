// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under MulanPSL v2.
// ***************************************************************************************
#include "DualCsrTimingResultSemantics.hpp"
#include "DualCsrEdgeViewImpl.hpp"

namespace ista::timing_dual_csr::detail {

void validateClockAndSlewDomains(const DualCsrState& state,
                                 const csr::CsrTimingResult& result,
                                 DualCsrViolations& violations)
{
  const DualCsrEdgeViewImpl edges(state);
  const std::size_t lanes
      = state.nodes.size() * csr::kAnalysisCount * csr::kTransitionCount;
  const auto validate_clock = [&](const csr::ClockResultSoa& clock,
                                  const std::string& field, bool physical) {
    const bool arrivals_ok = validatePresentDomain(
        clock.arrivals, lanes, field + ".arrivals", csr::detail::isFinite,
        "present clock arrival must be finite", violations);
    const bool slews_ok = validatePresentDomain(
        clock.slews, lanes, field + ".slews",
        [](double value) {
          return csr::detail::isFinite(value) && value >= 0.0;
        }, "present clock slew must be finite and nonnegative", violations);
    const bool nodes_ok = validatePresentDomain(
        clock.predecessor_nodes, lanes, field + ".predecessor_nodes",
        [&](csr::NodeId value) {
          return value.value() < state.nodes.size();
        }, "clock predecessor NodeId is out of range", violations);
    const bool delays_ok = validatePresentDomain(
        clock.predecessor_arc_delays, lanes,
        field + ".predecessor_arc_delays", csr::detail::isFinite,
        "clock predecessor delay must be finite", violations);
    const bool transitions_ok = validatePresentDomain(
        clock.predecessor_transitions, lanes,
        field + ".predecessor_transitions",
        [](csr::Transition value) {
          return value == csr::Transition::kRise
                 || value == csr::Transition::kFall;
        }, "clock predecessor transition must be rise or fall", violations);
    if (!(arrivals_ok && slews_ok && nodes_ok && delays_ok
          && transitions_ok))
      return;
    if (clock.predecessor_nodes.presence
            != clock.predecessor_arc_delays.presence
        || clock.predecessor_nodes.presence
               != clock.predecessor_transitions.presence)
      rejectSemantic(violations, field + ".predecessor",
                     "clock predecessor presence arrays must match");
    for (std::size_t lane = 0; lane < lanes; ++lane) {
      const csr::NodeId node{
          lane / (csr::kAnalysisCount * csr::kTransitionCount)};
      const bool owned = state.reference_payload.clock_owner_by_node[node.value()].has_value();
      if (!physical) {
        const auto& owners = state.reference_payload.clock_owner_by_node;
        const auto& clocks = state.reference_payload.clocks;
        const auto owner = owned ? std::ranges::find(clocks, *owners[node.value()], &csr::ClockConstraintModel::map_key) : clocks.end();
        if (owner != clocks.end() && owner->propagated) {
          const auto matches = [lane](const auto& actual, const auto& expected) {
            return lane < expected.presence.size() && lane < expected.values.size()
                   && actual.presence[lane] == expected.presence[lane]
                   && (!actual.presence[lane] || actual.values[lane] == expected.values[lane]);
          };
          const auto& expected = result.physical_clock;
          if (!matches(clock.arrivals, expected.arrivals) || !matches(clock.slews, expected.slews)
              || !matches(clock.predecessor_nodes, expected.predecessor_nodes)
              || !matches(clock.predecessor_arc_delays, expected.predecessor_arc_delays)
              || !matches(clock.predecessor_transitions, expected.predecessor_transitions))
            rejectSemantic(violations, field, "propagated effective clock must preserve physical values and identity");
          continue;
        }
        const std::uint8_t expected = owned ? 1 : 0;
        if (clock.arrivals.presence[lane] != expected || clock.slews.presence[lane] != expected
            || (owned && (clock.arrivals.values[lane] != 0.0 || clock.slews.values[lane] != 0.0)))
          rejectSemantic(violations, field, "effective ideal-clock lanes must be zero exactly at owned nodes");
        if (clock.predecessor_nodes.presence[lane] != 0)
          rejectSemantic(violations, field + ".predecessor", "effective ideal clocks must not have predecessors");
        continue;
      }
      const bool source = std::ranges::any_of(state.reference_payload.clocks, [&](const auto& model) {
        return hasResolvedClockSource(model, node);
      });
      if (source && (clock.arrivals.presence[lane] != 1 || clock.slews.presence[lane] != 1))
        rejectSemantic(violations, field, "resolved source must retain every physical clock lane");
      if (!owned && (clock.arrivals.presence[lane] || clock.slews.presence[lane] || clock.predecessor_nodes.presence[lane]))
        rejectSemantic(violations, field, "physical clock state requires a replayed owner");
      if (clock.predecessor_nodes.presence[lane] == 0) {
        if (clock.arrivals.presence[lane] && (!source || clock.arrivals.values[lane] != 0.0))
          rejectSemantic(violations, field, "physical clock arrival without predecessor must be a zero source seed");
        continue;
      }
      const auto predecessor = clock.predecessor_nodes.values[lane];
      const auto transition = static_cast<std::size_t>(clock.predecessor_transitions.values[lane]);
      if (predecessor.value() >= state.nodes.size() || transition >= csr::kTransitionCount)
        continue;
      const auto analysis = (lane / csr::kTransitionCount) % csr::kAnalysisCount;
      const auto upstream = (predecessor.value() * csr::kAnalysisCount + analysis) * csr::kTransitionCount + transition;
      bool edge_found = false;
      const auto visit = edges.forEachIncoming(node, [&](const csr::EdgeEntry& entry) {
        const auto arc = edges.arc(entry.arc_id);
        if (!arc)
          return csr::EdgeVisitResult::fail(arc.error());
        const auto arc_lane = ((entry.arc_id.value() * csr::kAnalysisCount + analysis) * csr::kTransitionCount + transition)
                              * csr::kTransitionCount + lane % csr::kTransitionCount;
        edge_found |= arc.value().eligibility.clock && arc.value().source == predecessor
                      && arc_lane < result.arcs.input_output_delays.presence.size()
                      && result.arcs.input_output_delays.presence[arc_lane] == 1;
        return csr::EdgeVisitResult::continueVisit();
      });
      double expected_arrival = 0.0;
      if (visit.error || !owned || state.reference_payload.clock_owner_by_node[node.value()]
                                     != state.reference_payload.clock_owner_by_node[predecessor.value()]
          || !edge_found || clock.arrivals.presence[lane] != 1 || clock.arrivals.presence[upstream] != 1)
        rejectSemantic(violations, field, "physical predecessor must follow an owned active clock edge with present arrivals");
      else if (!checkedRoundedPathArrival(clock.arrivals.values[upstream], clock.predecessor_arc_delays.values[lane], expected_arrival)
               || clock.arrivals.values[lane] != expected_arrival)
        rejectSemantic(violations, field, "physical arrival differs from its predecessor plus recorded delay");
    }
  };
  validate_clock(result.effective_clock, "result.effective_clock", false);
  validate_clock(result.physical_clock, "result.physical_clock", true);
  validatePresentDomain(
      result.data_slews, lanes, "result.data_slews",
      [](double value) {
        return csr::detail::isFinite(value) && value >= 0.0;
      }, "present data slew must be finite and nonnegative", violations);
}

}  // namespace ista::timing_dual_csr::detail
