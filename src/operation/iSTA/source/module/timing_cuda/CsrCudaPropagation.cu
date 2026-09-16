#include "CsrCudaArcTransfer.cuh"
#include "CsrCudaKernels.cuh"
#include "CsrCudaClockPropagation.cuh"
#include "CsrCudaSequentialPropagation.cuh"
#include "CsrCudaAdjacency.cuh"

namespace ista::csr::cuda_detail {
namespace {

// Deliberately one device thread: this full baseline preserves FIFO and exact
// first-winner ordering. The timed execution performs no CPU propagation.
__global__ void levelize(DeviceView view)
{
  if (*view.status != Status::success)
    return;
  for (Index node = 0; node < view.node_count; ++node) {
    view.indegrees[node] = 0;
    view.node_levels[node] = 0;
    for (Index offset = view.in_offsets[node]; offset < view.in_offsets[node + 1]; ++offset) {
      const Index index = view.in_arcs[offset];
      if (index == absent)
        continue;
      if (index >= view.arc_count || view.arcs[index].sink != node || !view.arcs[index].active) {
        *view.status = Status::invariant;
        return;
      }
      if (view.arcs[index].levelization)
        ++view.indegrees[node];
    }
  }
  Index tail = 0;
  for (Index index = 0; index < view.root_count; ++index) {
    const Index node = view.roots[index];
    if (node >= view.node_count || view.indegrees[node] != 0 || view.node_levels[node] != 0 || tail >= view.node_count) {
      *view.status = Status::invariant;
      return;
    }
    view.node_levels[node] = 1;
    view.order[tail++] = node;
  }
  for (Index node = 0; node < view.node_count; ++node)
    if ((view.indegrees[node] == 0) != (view.node_levels[node] != 0)) {
      *view.status = Status::invariant;
      return;
    }
  for (Index head = 0; head < tail; ++head) {
    const Index node = view.order[head];
    Index index;
    for (auto edges = outgoing(view, node, 0); edges.next(index);) {
      if (index >= view.arc_count || view.arcs[index].source != node || !view.arcs[index].active || view.arcs[index].sink >= view.node_count) {
        *view.status = Status::invariant;
        return;
      }
      const Arc& arc = view.arcs[index];
      if (!arc.levelization)
        continue;
      if (view.indegrees[arc.sink] == 0 || view.node_levels[node] == absent) {
        *view.status = Status::invariant;
        return;
      }
      const Index candidate = view.node_levels[node] + 1;
      if (candidate > view.node_levels[arc.sink])
        view.node_levels[arc.sink] = candidate;
      if (--view.indegrees[arc.sink] == 0) {
        if (tail >= view.node_count) {
          *view.status = Status::invariant;
          return;
        }
        view.order[tail++] = arc.sink;
      }
    }
  }
  view.level_offsets[0] = 0;
  Index count = 1;
  if (tail != 0) {
    Index level = view.node_levels[view.order[0]];
    for (Index index = 1; index < tail; ++index) {
      const Index next = view.node_levels[view.order[index]];
      if (next != level) {
        view.level_offsets[count++] = index;
        level = next;
      }
    }
    view.level_offsets[count++] = tail;
  }
  *view.level_offset_count = count;
  if (tail != view.node_count)
    *view.status = Status::cycle;
}

__device__ void dynamicPass(DeviceView view)
{
  for (Index position = 0; position < view.node_count; ++position) {
    const Index source = view.order[position];
    Index index;
    for (auto edges = outgoing(view, source, 1); edges.next(index);) {
      const Arc& arc = view.arcs[index];
      if (!arc.data_slew)
        continue;
      for (unsigned analysis = 0; analysis < 2; ++analysis) {
        for (unsigned input = 0; input < 2; ++input) {
          const Value<double>& source_slew = view.nodes[source].slew[analysis * 2 + input];
          if (!source_slew.present)
            continue;
          for (unsigned output = 0; output < 2; ++output) {
            const DynamicValue candidate = dynamicArc(view, index, analysis, input, output, source_slew.value);
            if (*view.status != Status::success)
              return;
            if (!candidate.present)
              continue;
            Value<double>& sink_slew = view.nodes[arc.sink].slew[analysis * 2 + output];
            if (!sink_slew.present)
              sink_slew = {candidate.slew, true};
            else if (epsilonBetter(view, analysis, candidate.slew, sink_slew.value))
              sink_slew.value = candidate.slew;
            if (*view.status != Status::success)
              return;
          }
        }
      }
    }
  }
}

__device__ bool negativeInfinity(double value)
{
  return isinf(value) && value < 0.0;
}

__device__ bool positiveInfinity(double value)
{
  return isinf(value) && value > 0.0;
}

__device__ void scalar(DeviceView view, Index index)
{
  const Arc& arc = view.arcs[index];
  const NodeState& source = view.nodes[arc.source];
  NodeState& sink = view.nodes[arc.sink];
  if (!isfinite(source.arrival)) {
    if (!negativeInfinity(source.arrival))
      *view.status = Status::invariant;
    return;
  }
  const double delay = finiteValue(view, view.arc_state[index].delay);
  const double candidate = finiteValue(view, source.arrival + delay);
  bool replace = negativeInfinity(sink.arrival);
  if (!replace) {
    if (!isfinite(sink.arrival)) {
      *view.status = Status::invariant;
      return;
    }
    replace = epsilonBetter(view, 0, candidate, sink.arrival, 1e-9);
  }
  if (replace) {
    sink.arrival = candidate;
    sink.launch = source.launch;
    sink.clock_name = source.clock_name;
    sink.predecessor_node = arc.source;
    sink.predecessor_arc = index;
  }
}

__device__ void pathLane(DeviceView view, Index index, unsigned analysis, unsigned source_kind, unsigned input, unsigned output)
{
  const Arc& arc = view.arcs[index];
  const ArcState& state = view.arc_state[index];
  const unsigned lane = ioLane(analysis, input, output);
  if (!state.io[lane].present)
    return;
  const double delay = finiteValue(view, state.graph[lane].present ? state.graph[lane].value : state.io[lane].value);
  const Index source_row = pathRow(arc.source, analysis, source_kind, input) * view.tag_count;
  const Index sink_row = pathRow(arc.sink, analysis, source_kind, output) * view.tag_count;
  for (Index tag = 0; tag < view.tag_count; ++tag) {
    const PathState& source = view.paths[source_row + tag];
    if (!source.present)
      continue;
    if (!isfinite(source.arrival)) {
      if (negativeInfinity(source.arrival))
        continue;
      *view.status = Status::invariant;
      return;
    }
    const double sum = finiteValue(view, source.arrival + delay);
    const double scaled = finiteValue(view, sum * 1e15);
    const double candidate = finiteValue(view, round(scaled) / 1e15);
    PathState& sink = view.paths[sink_row + tag];
    if (!sink.present || epsilonBetter(view, analysis, candidate, sink.arrival)) {
      sink = source;
      sink.arrival = candidate;
      const Value<double>& slew = view.nodes[arc.sink].slew[analysis * 2 + output];
      sink.slew = slew.present ? slew.value : 0.0;
      sink.predecessor_node = arc.source;
      sink.predecessor_arc = index;
      sink.predecessor_delay = delay;
      sink.predecessor_transition = static_cast<std::uint8_t>(input);
    }
    if (*view.status != Status::success)
      return;
  }
}

__device__ void arrivalPass(DeviceView view)
{
  for (Index position = 0; position < view.node_count; ++position) {
    const Index source = view.order[position];
    Index index;
    for (auto edges = outgoing(view, source, 2); edges.next(index);) {
      if (!view.arcs[index].data_arrival)
        continue;
      scalar(view, index);
      if (*view.status != Status::success)
        return;
      for (unsigned analysis = 0; analysis < 2; ++analysis)
        for (unsigned source_kind = 0; source_kind < 2; ++source_kind)
          for (unsigned input = 0; input < 2; ++input)
            for (unsigned output = 0; output < 2; ++output) {
              pathLane(view, index, analysis, source_kind, input, output);
              if (*view.status != Status::success)
                return;
            }
    }
  }
}

__device__ void requiredPass(DeviceView view)
{
  double default_required = 0.0;
  for (Index index = 0; index < view.end_count; ++index) {
    const Index node = view.ends[index];
    if (node >= view.node_count) {
      *view.status = Status::invariant;
      return;
    }
    const double arrival = view.nodes[node].arrival;
    if (isfinite(arrival)) {
      if (arrival > default_required)
        default_required = arrival;
    } else if (!negativeInfinity(arrival)) {
      *view.status = Status::invariant;
      return;
    }
  }
  for (Index index = 0; index < view.end_count; ++index)
    view.nodes[view.ends[index]].required = default_required;
  for (Index index = 0; index < view.endpoint_count; ++index) {
    const Endpoint& endpoint = view.endpoints[index];
    if (endpoint.node >= view.node_count) {
      *view.status = Status::invariant;
      return;
    }
    if (endpoint.sequential_check) {
      sequentialRequired(view, endpoint);
      if (*view.status != Status::success)
        return;
      continue;
    }
    if (!endpoint.output_delay.present)
      continue;
    const double required = finiteValue(view, endpoint.period - endpoint.output_delay.value);
    view.nodes[endpoint.node].required = required;
  }
  if (*view.status != Status::success)
    return;
  for (Index position = view.node_count; position != 0; --position) {
    const Index source = view.order[position - 1];
    Index index;
    for (auto edges = outgoing(view, source, 3); edges.next(index);) {
      const Arc& arc = view.arcs[index];
      if (!arc.data_arrival)
        continue;
      const double sink_required = view.nodes[arc.sink].required;
      if (!isfinite(sink_required)) {
        if (positiveInfinity(sink_required))
          continue;
        *view.status = Status::invariant;
        return;
      }
      const double candidate = finiteValue(view, sink_required - finiteValue(view, view.arc_state[index].delay));
      double& required = view.nodes[source].required;
      if (positiveInfinity(required))
        required = candidate;
      else if (!isfinite(required)) {
        *view.status = Status::invariant;
        return;
      } else if (candidate < required) {
        required = candidate;
      }
      if (*view.status != Status::success)
        return;
    }
  }
  for (Index index = 0; index < view.node_count; ++index) {
    NodeState& node = view.nodes[index];
    if (isfinite(node.arrival) && isfinite(node.required)) {
      node.slack = finiteValue(view, node.required - node.arrival);
    } else {
      if ((!isfinite(node.arrival) && !negativeInfinity(node.arrival)) || (!isfinite(node.required) && !positiveInfinity(node.required))) {
        *view.status = Status::invariant;
        return;
      }
      node.slack = 0.0;
    }
    if (*view.status != Status::success)
      return;
  }
}

__global__ void propagate(DeviceView view)
{
  if (*view.status != Status::success)
    return;
  for (Index index = 0; index < view.arc_count; ++index) {
    if (!view.arcs[index].active)
      continue;
    initializeArc(view, index);
    if (*view.status != Status::success)
      return;
  }
  clockPass(view);
  if (*view.status != Status::success)
    return;
  sequentialPass(view);
  if (*view.status != Status::success)
    return;
  dynamicPass(view);
  if (*view.status != Status::success)
    return;
  arrivalPass(view);
  if (*view.status != Status::success)
    return;
  requiredPass(view);
}

}  // namespace

void launchLevelization(DeviceView view, cudaStream_t stream)
{
  levelize<<<1, 1, 0, stream>>>(view);
}

std::uint64_t launchPropagation(DeviceView view, cudaStream_t stream)
{
  propagate<<<1, 1, 0, stream>>>(view);
  return 1;
}

}  // namespace ista::csr::cuda_detail
