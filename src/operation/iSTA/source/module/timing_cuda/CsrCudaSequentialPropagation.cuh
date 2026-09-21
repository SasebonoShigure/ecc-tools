#pragma once

#include "CsrCudaArcTransfer.cuh"
#include "CsrCudaAdjacency.cuh"

namespace ista::csr::cuda_detail {

__device__ inline Index pathRow(Index node, unsigned analysis, unsigned source, unsigned transition)
{
  return ((node * 2 + analysis) * 2 + source) * 2 + transition;
}

__device__ inline PathState* findPath(DeviceView view, Index row, Index tag)
{
  Index first = view.path_offsets[row], last = view.path_offsets[row + 1];
  while (first < last) {
    const Index middle = first + (last - first) / 2;
    if (view.path_tags[middle] < tag)
      first = middle + 1;
    else
      last = middle;
  }
  if (first == view.path_offsets[row + 1] || view.path_tags[first] != tag) {
    *view.status = Status::invariant;
    return nullptr;
  }
  return view.paths + first;
}

__device__ inline DynamicValue clockToQ(DeviceView view, const SequentialSeed& seed, unsigned analysis, unsigned output, double clock_slew, double load)
{
  DynamicValue value;
  value.present = true;
  value.delay = seed.child_count == 0 ? (analysis == 0 ? seed.delay_max : seed.delay_min) : seed.delay;
  value.slew = clock_slew;
  bool delay_seen = false, slew_seen = false;
  for (Index index = 0; index < seed.child_count; ++index) {
    const auto& child = view.children[seed.child_begin + index];
    if (!childSelected(child, seed.trigger, output))
      continue;
    if (child.delay[output].present) {
      const auto candidate = lookupTable(view, child.delay[output], child, clock_slew, load);
      if (!delay_seen || rawBetter(analysis, candidate, value.delay))
        value.delay = candidate;
      delay_seen = true;
    }
    if (child.slew[output].present) {
      const auto candidate = lookupTable(view, child.slew[output], child, clock_slew, load);
      if (!slew_seen || rawBetter(analysis, candidate, value.slew))
        value.slew = candidate;
      slew_seen = true;
    }
  }
  value.delay = finiteValue(view, value.delay);
  value.slew = finiteValue(view, value.slew);
  if (value.slew < 0.0)
    *view.status = Status::numeric;
  return value;
}

__device__ inline void sequentialPass(DeviceView view)
{
  for (Index id = 0; id < view.node_count; ++id) {
    auto& node = view.nodes[id];
    const auto& seed = node.sequential;
    if (seed.kind == SeedKind::none)
      continue;
    if ((seed.clock_node != absent && seed.clock_node >= view.node_count) || seed.trigger >= 2
        || seed.child_begin > view.child_count || seed.child_count > view.child_count - seed.child_begin
        || (seed.path_tag != absent && (seed.path_tag >= view.tag_count || seed.path_start >= view.node_count || seed.clock_node == absent))) {
      *view.status = Status::invariant;
      return;
    }
    double loads[4]{};
    if (seed.kind == SeedKind::output) {
      Index index;
      for (auto edges = outgoing(view, id, 1); edges.next(index);) {
        const Arc& arc = view.arcs[index];
        if (!arc.net)
          continue;
        if (!arc.has_load) {
          *view.status = Status::invariant;
          return;
        }
        for (unsigned lane = 0; lane < 4; ++lane)
          loads[lane] = arc.load[lane];
        break;
      }
      if (*view.status != Status::success)
        return;
    }
    for (unsigned analysis = 0; analysis < 2; ++analysis) {
      double launch = 0.0, slew = 0.0;
      if (seed.clock_node != absent && seed.kind != SeedKind::scalar) {
        const auto& clock = view.nodes[seed.clock_node];
        const auto lane = analysis * 2 + seed.trigger;
        launch = clock.effective_arrival[lane].present ? clock.effective_arrival[lane].value : 0.0;
        slew = clock.effective_slew[lane].present ? clock.effective_slew[lane].value : 0.0;
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
    clock_arrival = clock.effective_arrival[lane].present ? clock.effective_arrival[lane].value : 0.0;
    clock_slew = clock.effective_slew[lane].present ? clock.effective_slew[lane].value : 0.0;
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
  // Ideal clocks have no effective predecessor path and zero native CRPR.
  view.nodes[endpoint.node].required = finiteValue(view, round(scaled) / 1.0e15);
}

}  // namespace ista::csr::cuda_detail
