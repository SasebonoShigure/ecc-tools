#include "CsrCpuReferenceBackendInternal.hpp"

#include <cmath>

namespace ista::csr {
namespace {

using Status = CsrCpuReferenceStageStatus;

bool better(AnalysisMode analysis, double candidate, double current)
{
  return analysis == AnalysisMode::kMax ? candidate > current + 1e-6 : candidate < current - 1e-6;
}

Status clockArc(const ActiveCsrEdgeView& edges, const EdgeEntry& entry, const ArcSemanticPayload& arc,
                bool slew_pass, CsrCpuReferenceWorkState& work)
{
  auto& clock = work.result.physical_clock;
  for (AnalysisMode analysis : {AnalysisMode::kMax, AnalysisMode::kMin}) {
    for (Transition input : {Transition::kRise, Transition::kFall}) {
      const auto source = csrNodeLane(arc.source.value(), analysis, input);
      if (!clock.slews.presence[source] || (!slew_pass && !clock.arrivals.presence[source]))
        continue;
      for (Transition output : {Transition::kRise, Transition::kFall}) {
        const auto sink = csrNodeLane(arc.sink.value(), analysis, output);
        if (slew_pass) {
          CsrDynamicArcValue value;
          const auto status = evaluateCsrDynamicArc(edges, entry.arc_id, analysis, input, output, clock.slews.values[source], work, value);
          if (status != Status::kSuccess)
            return status;
          if (!value.present)
            continue;
          if (!clock.slews.presence[sink] || better(analysis, value.slew, clock.slews.values[sink])) {
            clock.slews.presence[sink] = 1;
            clock.slews.values[sink] = value.slew;
          }
        } else {
          const auto lane = csrArcIoLane(entry.arc_id.value(), analysis, input, output);
          const auto& arcs = work.result.arcs;
          if (!arcs.input_output_delays.presence[lane])
            continue;
          const double delay = arcs.graph_delays.presence[lane] ? arcs.graph_delays.values[lane] : arcs.input_output_delays.values[lane];
          const double scaled = (clock.arrivals.values[source] + delay) * 1e15;
          if (!std::isfinite(scaled))
            return Status::kNumericFailure;
          const double arrival = std::round(scaled) / 1e15;
          if (!clock.arrivals.presence[sink] || better(analysis, arrival, clock.arrivals.values[sink])) {
            clock.arrivals.presence[sink] = 1;
            clock.arrivals.values[sink] = arrival;
            clock.predecessor_nodes.presence[sink] = 1;
            clock.predecessor_nodes.values[sink] = arc.source;
            clock.predecessor_arc_delays.presence[sink] = 1;
            clock.predecessor_arc_delays.values[sink] = delay;
            clock.predecessor_transitions.presence[sink] = 1;
            clock.predecessor_transitions.values[sink] = input;
          }
        }
      }
    }
  }
  return Status::kSuccess;
}

}  // namespace

CsrCpuReferenceStageStatus propagateCsrCpuReferenceClocks(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                        CsrCpuReferenceWorkState& work)
{
  if (payloads.clockOwners().size() != work.node_count)
    return Status::kInvariantViolation;
  for (const auto& clock : payloads.clocks()) {
    for (bool slew_pass : {true, false}) {
      for (NodeId node : work.result.timing_order) {
        if (node.value() >= work.node_count)
          return Status::kInvariantViolation;
        const auto& owner = payloads.clockOwners()[node.value()];
        if (!owner || *owner != clock.map_key)
          continue;
        Status status = Status::kSuccess;
        const auto visit = edges.forEachOutgoing(node, [&](const EdgeEntry& entry) {
          const auto payload = edges.arc(entry.arc_id);
          if (!payload || entry.arc_id.value() >= work.arc_count || payload.value().source != node
              || payload.value().sink.value() >= work.node_count) {
            status = Status::kInvariantViolation;
            return EdgeVisitResult::fail(CsrViewError::kCallbackError);
          }
          const auto& arc = payload.value();
          if (!arc.eligibility.clock)
            return EdgeVisitResult::continueVisit();
          const auto& sink_owner = payloads.clockOwners()[arc.sink.value()];
          if (!sink_owner || *sink_owner != *owner) {
            status = Status::kInvariantViolation;
            return EdgeVisitResult::fail(CsrViewError::kCallbackError);
          }
          status = clockArc(edges, entry, arc, slew_pass, work);
          return status == Status::kSuccess ? EdgeVisitResult::continueVisit() : EdgeVisitResult::fail(CsrViewError::kCallbackError);
        });
        if (visit.control == EdgeVisitControl::kError)
          return status == Status::kSuccess ? Status::kInvariantViolation : status;
      }
    }
  }
  return Status::kSuccess;
}

}  // namespace ista::csr
