#include "CsrCpuReferenceBackendInternal.hpp"

#include <array>
#include <cmath>

namespace ista::csr {
namespace {
using Status = CsrCpuReferenceStageStatus;
bool better(AnalysisMode analysis, double candidate, double current)
{
  return analysis == AnalysisMode::kMax ? candidate > current + 1e-6 : candidate < current - 1e-6;
}
}  // namespace

CsrCpuReferenceStageStatus propagateCsrCpuReferenceClocks(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                        CsrCpuReferenceWorkState& work)
{
  if (payloads.clockOwners().size() != work.node_count)
    return Status::kInvariantViolation;
  struct Candidate { std::size_t lane; double value; NodeId source; double delay; Transition input; bool seed; };
  std::vector<Candidate> candidates;
  auto& clock_state = work.result.physical_clock;
  for (const auto& clock : payloads.clocks())
    for (bool slew_pass : {true, false})
      for (NodeId node : work.result.level_nodes) {
        if (node.value() >= work.node_count)
          return Status::kInvariantViolation;
        const auto& owner = payloads.clockOwners()[node.value()];
        if (!owner || *owner != clock.map_key)
          continue;
        candidates.clear();
        std::array<std::optional<double>, 4> extremes;
        const auto add = [&](Candidate candidate) {
          candidates.push_back(candidate);
          auto& best = extremes[candidate.lane];
          if (!best || (candidate.lane < 2 ? candidate.value > *best : candidate.value < *best))
            best = candidate.value;
        };
        auto& values = slew_pass ? clock_state.slews : clock_state.arrivals;
        for (std::size_t lane = 0; lane < 4; ++lane)
          if (values.presence[node.value() * 4 + lane])
            add({lane, values.values[node.value() * 4 + lane], node, 0.0, Transition::kRise, true});
        Status status = Status::kSuccess;
        const auto visit = edges.forEachIncoming(node, [&](const EdgeEntry& entry) {
          const auto payload = edges.arc(entry.arc_id);
          if (!payload || entry.arc_id.value() >= work.arc_count || payload.value().sink != node
              || payload.value().source.value() >= work.node_count)
            return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
          const auto& arc = payload.value();
          if (!arc.eligibility.clock)
            return EdgeVisitResult::continueVisit();
          if (payloads.clockOwners()[arc.source.value()] != owner)
            return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
          for (AnalysisMode analysis : {AnalysisMode::kMax, AnalysisMode::kMin})
            for (Transition input : {Transition::kRise, Transition::kFall}) {
              const auto source = csrNodeLane(arc.source.value(), analysis, input);
              if (!clock_state.slews.presence[source] || (!slew_pass && !clock_state.arrivals.presence[source]))
                continue;
              for (Transition output : {Transition::kRise, Transition::kFall}) {
                const auto lane = csrAnalysisIndex(analysis) * 2 + csrTransitionIndex(output);
                if (slew_pass) {
                  CsrDynamicArcValue value;
                  status = evaluateCsrDynamicArc(edges, entry.arc_id, analysis, input, output, clock_state.slews.values[source], work, value);
                  if (status != Status::kSuccess)
                    return EdgeVisitResult::fail(CsrViewError::kCallbackError);
                  if (value.present)
                    add({lane, value.slew, arc.source, value.delay, input, false});
                } else {
                  const auto io = csrArcIoLane(entry.arc_id.value(), analysis, input, output);
                  const auto& arcs = work.result.arcs;
                  if (!arcs.input_output_delays.presence[io])
                    continue;
                  const double delay = arcs.graph_delays.presence[io] ? arcs.graph_delays.values[io] : arcs.input_output_delays.values[io];
                  const double scaled = (clock_state.arrivals.values[source] + delay) * 1e15;
                  if (!std::isfinite(scaled)) {
                    status = Status::kNumericFailure;
                    return EdgeVisitResult::fail(CsrViewError::kCallbackError);
                  }
                  add({lane, std::round(scaled) / 1e15, arc.source, delay, input, false});
                }
              }
            }
          return EdgeVisitResult::continueVisit();
        });
        if (visit.control == EdgeVisitControl::kError)
          return status == Status::kSuccess ? Status::kInvariantViolation : status;
        std::array<bool, 4> selected{};
        for (const auto& candidate : candidates) {
          const auto analysis = candidate.lane < 2 ? AnalysisMode::kMax : AnalysisMode::kMin;
          if (selected[candidate.lane] || better(analysis, *extremes[candidate.lane], candidate.value))
            continue;
          selected[candidate.lane] = true;
          const auto lane = node.value() * 4 + candidate.lane;
          values.presence[lane] = 1;
          values.values[lane] = candidate.value;
          if (!slew_pass && !candidate.seed) {
            clock_state.predecessor_nodes.presence[lane] = 1;
            clock_state.predecessor_nodes.values[lane] = candidate.source;
            clock_state.predecessor_arc_delays.presence[lane] = 1;
            clock_state.predecessor_arc_delays.values[lane] = candidate.delay;
            clock_state.predecessor_transitions.presence[lane] = 1;
            clock_state.predecessor_transitions.values[lane] = candidate.input;
          }
        }
      }
  return Status::kSuccess;
}
}  // namespace ista::csr
