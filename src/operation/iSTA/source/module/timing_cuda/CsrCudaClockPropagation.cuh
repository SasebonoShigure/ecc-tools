#pragma once

#include "CsrCudaArcTransfer.cuh"
#include "CsrCudaAdjacency.cuh"

namespace ista::csr::cuda_detail {

__device__ inline void clockPass(DeviceView view)
{
  for (Index clock = 0; clock < view.clock_count; ++clock) {
    // Native completes the whole physical slew/delay pass before arrival.
    for (unsigned pass = 0; pass < 2; ++pass) {
      for (Index position = 0; position < view.node_count; ++position) {
        const Index source = view.order[position];
        if (source >= view.node_count) {
          *view.status = Status::invariant;
          return;
        }
        if (view.nodes[source].clock_owner != clock)
          continue;
        Index index;
        for (auto edges = outgoing(view, source, pass == 0 ? 1 : 2); edges.next(index);) {
          const Arc& arc = view.arcs[index];
          if (!arc.clock_eligible)
            continue;
          if (view.nodes[arc.sink].clock_owner != clock) {
            *view.status = Status::invariant;
            return;
          }
          for (unsigned analysis = 0; analysis < 2; ++analysis) {
            for (unsigned input = 0; input < 2; ++input) {
              const auto& from = view.nodes[source].physical_clock[analysis * 2 + input];
              if ((pass == 0 && !from.slew.present) || (pass == 1 && !from.arrival.present))
                continue;
              for (unsigned output = 0; output < 2; ++output) {
                auto& to = view.nodes[arc.sink].physical_clock[analysis * 2 + output];
                if (pass == 0) {
                  const auto candidate = dynamicArc(view, index, analysis, input, output, from.slew.value);
                  if (candidate.present && (!to.slew.present || epsilonBetter(view, analysis, candidate.slew, to.slew.value)))
                    to.slew = {candidate.slew, true};
                } else {
                  const auto lane = ioLane(analysis, input, output);
                  const auto& state = view.arc_state[index];
                  if (!state.io[lane].present)
                    continue;
                  const double delay = finiteValue(view, state.graph[lane].present ? state.graph[lane].value : state.io[lane].value);
                  const double sum = finiteValue(view, from.arrival.value + delay);
                  const double scaled = finiteValue(view, sum * 1.0e15);
                  const double arrival = finiteValue(view, round(scaled) / 1.0e15);
                  if (!to.arrival.present || epsilonBetter(view, analysis, arrival, to.arrival.value)) {
                    to.arrival = {arrival, true};
                    to.predecessor = source;
                    to.predecessor_delay = delay;
                    to.predecessor_transition = static_cast<std::uint8_t>(input);
                  }
                }
                if (*view.status != Status::success)
                  return;
              }
            }
          }
        }
        if (*view.status != Status::success)
          return;
      }
    }
  }
}

}  // namespace ista::csr::cuda_detail
