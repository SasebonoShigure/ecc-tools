#pragma once

#include "CsrCudaTableLookup.cuh"

namespace ista::csr::cuda_detail {

__device__ inline bool rawBetter(unsigned analysis, double candidate, double current)
{
  return analysis == 0 ? candidate > current : candidate < current;
}

__device__ inline bool epsilonBetter(DeviceView view, unsigned analysis, double candidate, double current, double epsilon = 1e-6)
{
  const double threshold = finiteValue(view, analysis == 0 ? current + epsilon : current - epsilon);
  return rawBetter(analysis, candidate, threshold);
}

__device__ inline unsigned ioLane(unsigned analysis, unsigned input, unsigned output)
{
  return (analysis * 2 + input) * 2 + output;
}

__device__ inline bool childSelected(const Child& child, unsigned input, unsigned output)
{
  if (child.sense == 3)
    return true;
  return child.sense == 2 ? input != output : input == output;
}

__device__ inline bool relationExists(DeviceView view, const Arc& arc, unsigned input, unsigned output)
{
  const Child* children = view.children + arc.child_begin;
  if (arc.clock && input != (children[0].trigger == 1 ? 1U : 0U))
    return false;
  bool negative = false;
  bool positive = false;
  bool non_unate = false;
  for (Index index = 0; index < arc.child_count; ++index) {
    negative |= children[index].sense == 2;
    positive |= children[index].sense == 0 || children[index].sense == 1;
    non_unate |= children[index].sense == 3;
  }
  if (!(arc.clock || non_unate || (negative && positive))) {
    const unsigned expected = negative && !positive && !non_unate ? 1 - input : input;
    if (output != expected)
      return false;
  }
  for (Index index = 0; index < arc.child_count; ++index)
    if (children[index].delay[output].present || children[index].slew[output].present || children[index].check[output].present)
      return true;
  return false;
}

__device__ inline void initializeArc(DeviceView view, Index index)
{
  const Arc& arc = view.arcs[index];
  ArcState& state = view.arc_state[index];
  if (!arc.active)
    return;
  if (!arc.net && !arc.has_load) {
    *view.status = Status::invariant;
    return;
  }
  for (unsigned analysis = 0; analysis < 2; ++analysis) {
    for (unsigned input = 0; input < 2; ++input) {
      if (arc.net || arc.child_count == 0) {
        const double delay = arc.net ? 0.0 : (analysis == 0 ? arc.delay_max : arc.delay_min);
        state.transition[analysis * 2 + input] = delay;
        state.io[ioLane(analysis, input, input)] = {delay, true};
        state.type[input] = {static_cast<std::uint8_t>(input), true};
        continue;
      }
      bool winner = false;
      double winning_delay = 0.0;
      unsigned winning_output = 2;
      for (unsigned output = 0; output < 2; ++output) {
        if (!relationExists(view, arc, input, output))
          continue;
        bool seen = false;
        double delay = 0.0;
        const unsigned lane = ioLane(analysis, input, output);
        for (Index local = 0; local < arc.child_count; ++local) {
          const Index child_index = arc.child_begin + local;
          const Child& child = view.children[child_index];
          if (!childSelected(child, input, output) || !child.delay[output].present)
            continue;
          const double candidate = lookupTable(view, child.delay[output], child, 0.0, arc.load[analysis * 2 + output]);
          ChildState& cache = view.child_state[arc.child_state_begin + local];
          if (!cache.delay[lane].present) {
            cache.delay[lane] = {candidate, true};
            cache.fallback[lane] = 1;
          } else if (rawBetter(analysis, candidate, cache.delay[lane].value)) {
            cache.delay[lane].value = candidate;
          }
          if (!seen || rawBetter(analysis, candidate, delay)) {
            delay = candidate;
            seen = true;
          }
        }
        if (!seen)
          delay = arc.delay;
        state.io[lane] = {delay, true};
        if (!winner || rawBetter(analysis, delay, winning_delay)) {
          winning_delay = delay;
          winning_output = output;
          winner = true;
        }
      }
      if (winner) {
        state.transition[analysis * 2 + input] = winning_delay;
        state.type[input] = {static_cast<std::uint8_t>(winning_output), true};
      }
    }
  }
  state.delay_max = state.transition[1] > state.transition[0] ? state.transition[1] : state.transition[0];
  state.delay_min = state.transition[3] < state.transition[2] ? state.transition[3] : state.transition[2];
  state.delay = state.delay_max;
}

struct DynamicValue
{
  double delay{0.0};
  double slew{0.0};
  bool present{false};
};

__device__ inline DynamicValue dynamicArc(DeviceView view, Index index, unsigned analysis, unsigned input, unsigned output, double source_slew)
{
  DynamicValue value;
  if (!isfinite(source_slew) || source_slew < 0.0) {
    numeric(view);
    return value;
  }
  const Arc& arc = view.arcs[index];
  ArcState& state = view.arc_state[index];
  const unsigned lane = ioLane(analysis, input, output);
  if (!state.io[lane].present)
    return value;
  value = {state.io[lane].value, source_slew, true};
  if (arc.child_count != 0) {
    if (!arc.has_load) {
      *view.status = Status::invariant;
      return value;
    }
    bool delay_seen = false;
    bool slew_seen = false;
    double delay = 0.0;
    double slew = 0.0;
    for (Index local = 0; local < arc.child_count; ++local) {
      const Index child_index = arc.child_begin + local;
      const Child& child = view.children[child_index];
      if (!childSelected(child, input, output))
        continue;
      const double load = arc.load[analysis * 2 + output];
      if (child.delay[output].present) {
        const double candidate = lookupTable(view, child.delay[output], child, source_slew, load);
        ChildState& cache = view.child_state[arc.child_state_begin + local];
        if (!cache.delay[lane].present) {
          *view.status = Status::invariant;
          return value;
        }
        if (cache.fallback[lane] != 0) {
          cache.delay[lane].value = candidate;
          cache.fallback[lane] = 0;
        } else if (rawBetter(analysis, candidate, cache.delay[lane].value)) {
          cache.delay[lane].value = candidate;
        }
        if (!delay_seen || rawBetter(analysis, candidate, delay)) {
          delay = candidate;
          delay_seen = true;
        }
      }
      if (child.slew[output].present) {
        const double candidate = lookupTable(view, child.slew[output], child, source_slew, load);
        if (candidate < 0.0) {
          numeric(view);
          return value;
        }
        if (!slew_seen || rawBetter(analysis, candidate, slew)) {
          slew = candidate;
          slew_seen = true;
        }
      }
    }
    value.delay = delay_seen ? delay : arc.delay;
    value.slew = slew_seen ? slew : source_slew;
  }
  if (!isfinite(value.delay) || !isfinite(value.slew) || value.slew < 0.0) {
    numeric(view);
    return value;
  }
  if (!state.graph[lane].present)
    state.graph[lane] = {value.delay, true};
  else if (epsilonBetter(view, analysis, value.delay, state.graph[lane].value))
    state.graph[lane].value = value.delay;
  value.delay = state.graph[lane].value;
  return value;
}

}  // namespace ista::csr::cuda_detail
