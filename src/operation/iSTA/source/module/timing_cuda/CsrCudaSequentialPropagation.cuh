#pragma once

#include "CsrCudaArcTransfer.cuh"
#include "CsrCudaAdjacency.cuh"

namespace ista::csr::cuda_detail {

__device__ inline Index pathRow(Index node, unsigned analysis, unsigned source, unsigned transition)
{
  return ((node * 2 + analysis) * 2 + source) * 2 + transition;
}

__device__ inline PathState* lookupPath(DeviceView view, Index row, Index tag)
{
  Index first = view.path_offsets[row], last = view.path_offsets[row + 1];
  while (first < last) {
    const Index middle = first + (last - first) / 2;
    if (view.path_tags[middle] < tag)
      first = middle + 1;
    else
      last = middle;
  }
  return first == view.path_offsets[row + 1] || view.path_tags[first] != tag ? nullptr : view.paths + first;
}

__device__ inline PathState* findPath(DeviceView view, Index row, Index tag)
{
  auto* path = lookupPath(view, row, tag);
  if (path == nullptr)
    *view.status = Status::invariant;
  return path;
}

__device__ inline DynamicValue clockToQ(DeviceView view, const SequentialSeed& seed, unsigned analysis, unsigned output, double clock_slew, double load)
{
  DynamicValue value;
  value.present = true;
  value.delay = seed.child_count == 0 ? (analysis == 0 ? seed.delay_max : seed.delay_min) : seed.delay;
  value.slew = clock_slew;
  bool delay_seen = false, slew_seen = false;
  // Match native DCTask cache order; a repeated lookup has no side effects.
  for (unsigned pass = 0; pass < 2; ++pass) {
    for (Index index = 0; index < seed.child_count; ++index) {
      const auto& child = view.children[seed.child_begin + index];
      if (!childSelected(child, seed.trigger, output))
        continue;
      const auto& table = pass == 0 ? child.delay[output] : child.slew[output];
      if (!table.present)
        continue;
      const auto candidate = cellTiming(view, seed.rc_binding, seed.child_begin, index, analysis, output, clock_slew, load, pass != 0);
      auto& result = pass == 0 ? value.delay : value.slew;
      auto& seen = pass == 0 ? delay_seen : slew_seen;
      if (!seen || rawBetter(analysis, candidate, result))
        result = candidate;
      seen = true;
    }
  }
  value.delay = finiteValue(view, value.delay);
  value.slew = finiteValue(view, value.slew);
  if (value.slew < 0.0)
    *view.status = Status::numeric;
  return value;
}

__device__ inline void sequentialNode(DeviceView view, Index id)
{
  auto& node = view.nodes[id];
  const auto& seed = node.sequential;
  if (seed.kind == SeedKind::none)
    return;
  if ((seed.clock_node != absent && seed.clock_node >= view.node_count) || seed.trigger >= 2
      || seed.child_begin > view.child_count || seed.child_count > view.child_count - seed.child_begin
      || (seed.path_tag != absent && (seed.path_tag >= view.tag_count || seed.path_start >= view.node_count || seed.clock_node == absent))) {
    *view.status = Status::invariant;
    return;
  }
  double loads[4]{};
  if (seed.kind == SeedKind::output && seed.rc_binding == absent) {
    for (unsigned lane = 0; lane < 4; ++lane)
      loads[lane] = seed.output_load[lane];
  }
  for (unsigned analysis = 0; analysis < 2; ++analysis) {
    double launch = 0.0, slew = 0.0;
    if (seed.clock_node != absent && seed.kind != SeedKind::scalar) {
      const auto& clock = view.nodes[seed.clock_node];
      const auto lane = analysis * 2 + seed.trigger;
      launch = clock.propagated_clock && clock.physical_clock[lane].arrival.present ? clock.physical_clock[lane].arrival.value : 0.0;
      slew = clock.propagated_clock && clock.physical_clock[lane].slew.present ? clock.physical_clock[lane].slew.value : 0.0;
    }
    for (unsigned transition = 0; transition < 2; ++transition) {
      double arrival = launch, output_slew = slew;
      if (seed.kind == SeedKind::output) {
        const auto value = clockToQ(view, seed, analysis, transition, slew, loads[analysis * 2 + transition]);
        arrival = finiteValue(view, launch + value.delay);
        output_slew = value.slew;
      }
      if (!isfinite(arrival) || !isfinite(launch) || !isfinite(output_slew) || output_slew < 0.0)
        *view.status = Status::numeric;
      if (*view.status != Status::success)
        return;
      node.slew[analysis * 2 + transition] = {output_slew, true};
      if (analysis == 0 && transition == 0) {
        node.arrival = arrival;
        node.launch = launch;
      }
      if (seed.path_tag != absent) {
        auto* slot = findPath(view, pathRow(id, analysis, 1, transition), seed.path_tag);
        if (slot == nullptr)
          return;
        auto& path = *slot;
        path = {};
        path.present = true;
        path.arrival = arrival;
        path.slew = output_slew;
        path.launch = launch;
        path.start = seed.path_start;
        path.crpr_node = seed.clock_node;
        path.crpr_transition = seed.trigger;
      }
    }
  }
  selectRcDriver(view, id, false);
}

__device__ inline void sequentialPass(DeviceView view)
{
  for (Index id = 0; id < view.node_count; ++id) {
    sequentialNode(view, id);
    if (*view.status != Status::success)
      return;
  }
}

__device__ inline const PathState* worstEndpointPath(DeviceView view, Index node, unsigned& selected_transition)
{
  for (unsigned source = 0; source < 2; ++source) {
    const PathState* best = nullptr;
    for (unsigned transition = 0; transition < 2; ++transition) {
      const PathState* lane_best = nullptr;
      const auto row = pathRow(node, 0, source, transition);
      for (Index slot = view.path_offsets[row]; slot < view.path_offsets[row + 1]; ++slot) {
        const auto& path = view.paths[slot];
        if (path.present && isfinite(path.arrival) && (lane_best == nullptr || epsilonBetter(view, 0, path.arrival, lane_best->arrival)))
          lane_best = &path;
      }
      if (lane_best != nullptr && (best == nullptr || epsilonBetter(view, 0, lane_best->arrival, best->arrival))) {
        best = lane_best;
        selected_transition = transition;
      }
    }
    if (best != nullptr)
      return best;
  }
  return nullptr;
}

__device__ inline void sequentialRequired(DeviceView view, const Endpoint& endpoint)
{
  if (endpoint.node >= view.node_count || (endpoint.clock_node != absent && endpoint.clock_node >= view.node_count)
      || endpoint.child_begin > view.child_count || endpoint.child_count > view.child_count - endpoint.child_begin || endpoint.clock_transition > 2) {
    *view.status = Status::invariant;
    return;
  }
  unsigned transition = 0;
  const auto* path = worstEndpointPath(view, endpoint.node, transition);
  bool match = endpoint.child_count == 0;
  for (Index index = 0; index < endpoint.child_count; ++index)
    match |= view.children[endpoint.child_begin + index].check[transition].present;
  if (!match || *view.status != Status::success)
    return;
  double clock_arrival = 0.0, clock_slew = 0.0;
  if (endpoint.clock_node != absent && endpoint.clock_transition != 2) {
    const auto& clock = view.nodes[endpoint.clock_node];
    const auto lane = 2 + endpoint.clock_transition;  // Native max analysis uses the min capture clock.
    clock_arrival = clock.propagated_clock && clock.physical_clock[lane].arrival.present ? clock.physical_clock[lane].arrival.value : 0.0;
    clock_slew = clock.propagated_clock && clock.physical_clock[lane].slew.present ? clock.physical_clock[lane].slew.value : 0.0;
  }
  const double data_slew = path == nullptr ? 0.0 : path->slew;
  double check_time = endpoint.check_time;
  bool seen = false;
  for (Index index = 0; index < endpoint.child_count; ++index) {
    const auto& child = view.children[endpoint.child_begin + index];
    if (!child.check[transition].present)
      continue;
    const auto value = lookupCheckTable(view, child.check[transition], child, clock_slew, data_slew);
    if (!seen || value > check_time)
      check_time = value;
    seen = true;
  }
  const double capture = finiteValue(view, endpoint.period + clock_arrival);
  const double required = finiteValue(view, capture - finiteValue(view, check_time));
  const double scaled = finiteValue(view, required * 1.0e15);
  // Native scalar RAT has no launch path; report analysis applies per-path CRPR.
  view.nodes[endpoint.node].required = finiteValue(view, round(scaled) / 1.0e15);
}

}  // namespace ista::csr::cuda_detail
