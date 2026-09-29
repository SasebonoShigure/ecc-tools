#pragma once

#include "CsrCudaArcTransfer.cuh"
#include "CsrCudaAdjacency.cuh"

namespace ista::csr::cuda_detail {

__device__ inline void clockNode(DeviceView view, Index id, Index clock, bool slew_pass)
{
  Value<double> extreme[4];
  bool selected[4]{};
  for (unsigned lane = 0; lane < 4; ++lane) {
    const auto& seed = view.nodes[id].physical_clock[lane];
    extreme[lane] = slew_pass ? seed.slew : seed.arrival;
  }
  for (unsigned pass = 0; pass < 2; ++pass) {
    if (pass == 1)
      for (unsigned lane = 0; lane < 4; ++lane) {
        const auto& state = view.nodes[id].physical_clock[lane];
        const auto seed = slew_pass ? state.slew : state.arrival;
        selected[lane] = seed.present && !epsilonBetter(view, lane / 2, extreme[lane].value, seed.value);
      }
    Index index;
    for (auto edges = incoming(view, id, slew_pass ? 1 : 2); edges.next(index);) {
      const auto& arc = view.arcs[index];
      if (!arc.clock_eligible)
        continue;
      if (view.nodes[arc.source].clock_owner != clock) {
        *view.status = Status::invariant;
        return;
      }
      for (unsigned analysis = 0; analysis < 2; ++analysis)
        for (unsigned input = 0; input < 2; ++input) {
          const auto& source = view.nodes[arc.source].physical_clock[analysis * 2 + input];
          if (!source.slew.present || (!slew_pass && !source.arrival.present))
            continue;
          for (unsigned output = 0; output < 2; ++output) {
            const auto lane = analysis * 2 + output;
            if (pass == 1 && selected[lane])
              continue;
            double value, delay = 0.0;
            if (slew_pass) {
              const auto candidate = dynamicArc(view, index, analysis, input, output, source.slew.value);
              if (*view.status != Status::success)
                return;
              if (!candidate.present)
                continue;
              value = candidate.slew;
            } else {
              const auto io = ioLane(analysis, input, output);
              const auto& state = view.arc_state[index];
              if (!state.io[io].present)
                continue;
              delay = finiteValue(view, state.graph[io].present ? state.graph[io].value : state.io[io].value);
              const auto sum = finiteValue(view, source.arrival.value + delay);
              const auto scaled = finiteValue(view, sum * 1e15);
              value = finiteValue(view, round(scaled) / 1e15);
            }
            if (pass == 0) {
              if (!extreme[lane].present || rawBetter(analysis, value, extreme[lane].value))
                extreme[lane] = {value, true};
            } else if (!epsilonBetter(view, analysis, extreme[lane].value, value)) {
              auto& target = view.nodes[id].physical_clock[lane];
              if (slew_pass) {
                target.slew = {value, true};
              } else {
                target.arrival = {value, true};
                target.predecessor = arc.source;
                target.predecessor_delay = delay;
                target.predecessor_transition = static_cast<std::uint8_t>(input);
              }
              selected[lane] = true;
            }
            if (*view.status != Status::success)
              return;
          }
        }
    }
    if (*view.status != Status::success)
      return;
  }
}

__device__ inline void clockPass(DeviceView view)
{
  // Clock domains are disjoint. Each pass can visit their nodes together;
  // no clock scans nodes belonging to all the other domains.
  for (unsigned pass = 0; pass < 2; ++pass)
    for (Index position = 0; position < view.node_count; ++position) {
      const auto id = view.order[position];
      if (id >= view.node_count) {
        *view.status = Status::invariant;
        return;
      }
      const auto owner = view.nodes[id].clock_owner;
      if (owner == absent)
        continue;
      if (owner >= view.clock_count) {
        *view.status = Status::invariant;
        return;
      }
      clockNode(view, id, owner, pass == 0);
      if (*view.status != Status::success)
        return;
    }
}

}  // namespace ista::csr::cuda_detail
