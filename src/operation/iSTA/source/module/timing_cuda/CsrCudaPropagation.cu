#include <cub/device/device_reduce.cuh>
#include <tuple>
#include "CudaDualCsrInternal.cuh"
#include "CsrCudaArcTransfer.cuh"
#include "CsrCudaKernels.cuh"
#include "CsrCudaMemory.cuh"
#include "CsrCudaClockPropagation.cuh"
#include "CsrCudaSequentialPropagation.cuh"
#include "CsrCudaAdjacency.cuh"

namespace ista::csr::cuda_detail {
namespace {

__device__ void dynamicNode(DeviceView view, Index sink)
{
  Value<double> seed[4], extreme[4];
  bool selected[4]{};
  for (unsigned lane = 0; lane < 4; ++lane)
    seed[lane] = extreme[lane] = view.nodes[sink].slew[lane];
  for (unsigned pass = 0; pass < 2; ++pass) {
    if (pass == 1)
      for (unsigned lane = 0; lane < 4; ++lane)
        if (seed[lane].present && !epsilonBetter(view, lane / 2, extreme[lane].value, seed[lane].value))
          selected[lane] = true;
    Index index;
    for (auto edges = incoming(view, sink, 1); edges.next(index);) {
      const Arc& arc = view.arcs[index];
      if (!arc.data_slew)
        continue;
      for (unsigned analysis = 0; analysis < 2; ++analysis)
        for (unsigned input = 0; input < 2; ++input) {
          const auto from = view.nodes[arc.source].slew[analysis * 2 + input];
          if (!from.present)
            continue;
          for (unsigned output = 0; output < 2; ++output) {
            const auto lane = analysis * 2 + output;
            if (pass == 1 && selected[lane])
              continue;
            const auto candidate = dynamicArc(view, index, analysis, input, output, from.value);
            if (*view.status != Status::success)
              return;
            if (!candidate.present)
              continue;
            if (pass == 0) {
              if (!extreme[lane].present || rawBetter(analysis, candidate.slew, extreme[lane].value))
                extreme[lane] = {candidate.slew, true};
            } else if (!epsilonBetter(view, analysis, extreme[lane].value, candidate.slew)) {
              view.nodes[sink].slew[lane] = {candidate.slew, true};
              selected[lane] = true;
            }
          }
        }
    }
    if (*view.status != Status::success)
      return;
  }
  selectRcDriver(view, sink, false);
}

__device__ bool negativeInfinity(double value)
{
  return isinf(value) && value < 0.0;
}

__device__ bool positiveInfinity(double value)
{
  return isinf(value) && value > 0.0;
}

__device__ void arrivalNode(DeviceView view, Index id)
{
  auto& sink = view.nodes[id];
  double extreme = sink.arrival;
  if (!isfinite(extreme) && !negativeInfinity(extreme)) {
    *view.status = Status::invariant;
    return;
  }
  for (unsigned pass = 0; pass < 2; ++pass) {
    if (pass == 1 && isfinite(sink.arrival) && !epsilonBetter(view, 0, extreme, sink.arrival, 1e-9))
      break;
    Index index;
    for (auto edges = incoming(view, id, 2); edges.next(index);) {
      const auto& arc = view.arcs[index];
      if (!arc.data_arrival)
        continue;
      const auto& source = view.nodes[arc.source];
      if (!isfinite(source.arrival)) {
        if (negativeInfinity(source.arrival))
          continue;
        *view.status = Status::invariant;
        return;
      }
      const auto value = finiteValue(view, source.arrival + finiteValue(view, view.arc_state[index].delay));
      if (pass == 0) {
        if (value > extreme)
          extreme = value;
      } else if (!epsilonBetter(view, 0, extreme, value, 1e-9)) {
        sink.arrival = value;
        sink.launch = source.launch;
        sink.clock_name = source.clock_name;
        sink.predecessor_node = arc.source;
        sink.predecessor_arc = index;
        break;
      }
    }
    if (*view.status != Status::success)
      return;
  }

  for (unsigned analysis = 0; analysis < 2; ++analysis)
    for (unsigned kind = 0; kind < 2; ++kind)
      for (unsigned output = 0; output < 2; ++output) {
        const auto row = pathRow(id, analysis, kind, output);
        for (Index slot = view.path_offsets[row]; slot < view.path_offsets[row + 1]; ++slot) {
          auto& target = view.paths[slot];
          const auto tag = view.path_tags[slot];
          Value<double> best{target.arrival, target.present && isfinite(target.arrival)};
          if (target.present && !isfinite(target.arrival) && !negativeInfinity(target.arrival)) {
            *view.status = Status::invariant;
            return;
          }
          for (unsigned pass = 0; pass < 2; ++pass) {
            // The existing seed has the first local candidate identity.
            if (pass == 1 && target.present && isfinite(target.arrival)
                && !epsilonBetter(view, analysis, best.value, target.arrival))
              break;
            bool selected = false;
            Index index;
            for (auto edges = incoming(view, id, 2); !selected && edges.next(index);) {
              const auto& arc = view.arcs[index];
              if (!arc.data_arrival)
                continue;
              for (unsigned input = 0; input < 2; ++input) {
                const auto lane = ioLane(analysis, input, output);
                const auto& state = view.arc_state[index];
                if (!state.io[lane].present)
                  continue;
                const auto* source = lookupPath(view, pathRow(arc.source, analysis, kind, input), tag);
                if (source == nullptr || !source->present)
                  continue;
                if (!isfinite(source->arrival)) {
                  if (negativeInfinity(source->arrival))
                    continue;
                  *view.status = Status::invariant;
                  return;
                }
                const double delay = finiteValue(view, state.graph[lane].present ? state.graph[lane].value : state.io[lane].value);
                const double sum = finiteValue(view, source->arrival + delay);
                const double scaled = finiteValue(view, sum * 1e15);
                const double value = finiteValue(view, round(scaled) / 1e15);
                if (pass == 0) {
                  if (!best.present || rawBetter(analysis, value, best.value))
                    best = {value, true};
                } else if (!epsilonBetter(view, analysis, best.value, value)) {
                  target = *source;
                  target.arrival = value;
                  const auto slew = sink.slew[analysis * 2 + output];
                  target.slew = slew.present ? slew.value : 0.0;
                  target.predecessor_node = arc.source;
                  target.predecessor_arc = index;
                  target.predecessor_delay = delay;
                  target.predecessor_transition = static_cast<std::uint8_t>(input);
                  selected = true;
                  break;
                }
                if (*view.status != Status::success)
                  return;
              }
            }
            if (*view.status != Status::success)
              return;
          }
        }
      }
}

__device__ void requiredNode(DeviceView view, Index source)
{
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
__device__ void slackNode(DeviceView view, Index id)
{
  NodeState& node = view.nodes[id];
  if (isfinite(node.arrival) && isfinite(node.required)) {
    node.slack = finiteValue(view, node.required - node.arrival);
  } else {
    if ((!isfinite(node.arrival) && !negativeInfinity(node.arrival)) || (!isfinite(node.required) && !positiveInfinity(node.required))) {
      *view.status = Status::invariant;
      return;
    }
    node.slack = 0.0;
  }
}
__global__ void initializeArcs(DeviceView view)
{
  if (readStatus(view.status) != Status::success)
    return;
  Status status = Status::success;
  auto local = view;
  local.status = &status;
  for (Index id = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; id < view.arc_count;
       id += static_cast<Index>(blockDim.x) * gridDim.x) {
    const auto& arc = view.arcs[id];
    if (arc.active && !(arc.rc_binding != absent && !arc.net && arc.child_count))
      initializeArc(local, id);
    if (status != Status::success)
      break;
  }
  recordFailure(view.status, status);
}
// Different nets run in parallel. Within one driver, initialization preserves
// native cell ordinal and cache miss order, including disabled cell arcs.
__global__ void initializeRcDrivers(DeviceView view)
{
  if (readStatus(view.status) != Status::success) return;
  Status status = Status::success;
  auto local = view;
  local.status = &status;
  for (Index index = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; index < view.rc_net_count;
       index += static_cast<Index>(blockDim.x) * gridDim.x) {
    auto& net = view.rc_nets[index];
    for (unsigned lane = 0; lane < 4; ++lane) {
      net.drivers[lane] = {lane * net.child_count, net.child_count};
      for (Index child = 0; child < net.child_count; ++child)
        net.rows[lane * net.child_count + child].size = 0;
    }
    for (Index position = 0; position < net.initialize_count; ++position) {
      const auto id = net.initialize_arcs[position];
      if (id >= view.arc_count) { status = Status::invariant; break; }
      if (view.arcs[id].active) initializeArc(local, id);
      if (status != Status::success) break;
    }
    if (status != Status::success) break;
  }
  recordFailure(view.status, status);
}
enum class Pass { sequential, slew, arrival, required, slack };
template <Pass pass> __global__ void propagateNodes(DeviceView view, Index begin, Index count)
{
  if (readStatus(view.status) != Status::success)
    return;
  Status status = Status::success;
  auto local = view;
  local.status = &status;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
       i += static_cast<Index>(blockDim.x) * gridDim.x) {
    Index node;
    if constexpr (pass == Pass::sequential || pass == Pass::slack)
      node = begin + i;
    else
      node = view.order[begin + i];
    if (node >= view.node_count) {
      status = Status::invariant;
      break;
    }
    if constexpr (pass == Pass::sequential)
      sequentialNode(local, node);
    else if constexpr (pass == Pass::slew)
      dynamicNode(local, node);
    else if constexpr (pass == Pass::arrival)
      arrivalNode(local, node);
    else if constexpr (pass == Pass::required)
      requiredNode(local, node);
    else
      slackNode(local, node);
    if (status != Status::success)
      break;
  }
  recordFailure(view.status, status);
}
__global__ void propagateClocks(DeviceView view, const ClockWorkItem* work, Index begin, Index count, bool slew)
{
  if (readStatus(view.status) != Status::success)
    return;
  Status status = Status::success;
  auto local = view;
  local.status = &status;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
       i += static_cast<Index>(blockDim.x) * gridDim.x) {
    const auto item = work[begin + i];
    if (item.node >= view.node_count || item.owner >= view.clock_count || view.nodes[item.node].clock_owner != item.owner) {
      status = Status::invariant;
      break;
    }
    clockNode(local, item.node, item.owner, slew);
    if (status != Status::success)
      break;
  }
  recordFailure(view.status, status);
}
__global__ void collectEndArrivals(DeviceView view, double* arrivals)
{
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < view.end_count;
       i += static_cast<Index>(blockDim.x) * gridDim.x) {
    arrivals[i] = 0.0;
    if (readStatus(view.status) != Status::success)
      continue;
    const auto node = view.ends[i];
    if (node >= view.node_count) {
      recordFailure(view.status, Status::invariant);
      continue;
    }
    const auto value = view.nodes[node].arrival;
    if (isfinite(value))
      arrivals[i] = value > 0.0 ? value : 0.0;
    else if (!negativeInfinity(value))
      recordFailure(view.status, Status::invariant);
  }
}
__global__ void initializeRequired(DeviceView view, const double* required)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < view.end_count;
       i += static_cast<Index>(blockDim.x) * gridDim.x)
    view.nodes[view.ends[i]].required = *required;
}
__global__ void endpointRequired(DeviceView view)
{
  if (readStatus(view.status) != Status::success)
    return;
  Status status = Status::success;
  auto local = view;
  local.status = &status;
  // Packing selects one endpoint per node; patches only append unique new nodes.
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < view.endpoint_count;
       i += static_cast<Index>(blockDim.x) * gridDim.x) {
    const auto& endpoint = view.endpoints[i];
    if (endpoint.node >= view.node_count) {
      status = Status::invariant;
      break;
    }
    if (endpoint.sequential_check)
      sequentialRequired(local, endpoint);
    else if (endpoint.output_delay.present)
      view.nodes[endpoint.node].required = finiteValue(local, endpoint.period - endpoint.output_delay.value);
    if (status != Status::success)
      break;
  }
  recordFailure(view.status, status);
}

__global__ void clearNodes(DeviceView view)
{
  for (Index node = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; node < view.node_count; node += blockDim.x * gridDim.x) {
    view.nodes[node] = {};
  }
}
__global__ void seedNodes(DeviceView view, const NodeSeed* seeds, Index count)
{
  for (Index index = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; index < count; index += blockDim.x * gridDim.x)
    view.nodes[seeds[index].node] = seeds[index].state;
}
__global__ void clearPaths(DeviceView view)
{
  for (Index slot = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; slot < view.path_count; slot += blockDim.x * gridDim.x) {
    view.paths[slot] = {};
  }
}
__global__ void seedPaths(DeviceView view, const PathSeed* seeds, Index count)
{
  if (readStatus(view.status) != Status::success)
    return;
  Status status = Status::success;
  auto local = view;
  local.status = &status;
  for (Index index = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; index < count;
       index += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto& seed = seeds[index];
    auto* path = findPath(local, seed.row, seed.tag);
    if (path != nullptr)
      *path = seed.state;
  }
  recordFailure(view.status, status);
}

}  // namespace

std::uint64_t launchSeedInitialization(DeviceView view, const NodeSeed* nodes, Index node_count,
                                       const PathSeed* paths, Index path_count, cudaStream_t stream, unsigned threads)
{
  std::uint64_t launches = 0;
  if (view.node_count) {
    // D2H returns complete records; initialize padding as well as named fields.
    check(cudaMemsetAsync(view.nodes, 0, view.node_count * sizeof(NodeState), stream), "CUDA node storage reset");
    clearNodes<<<kernelBlocks(view.node_count, threads), threads, 0, stream>>>(view);
    ++launches;
  }
  if (node_count) {
    seedNodes<<<kernelBlocks(node_count, threads), threads, 0, stream>>>(view, nodes, node_count);
    ++launches;
  }
  if (view.path_count) {
    check(cudaMemsetAsync(view.paths, 0, view.path_count * sizeof(PathState), stream), "CUDA path storage reset");
    clearPaths<<<kernelBlocks(view.path_count, threads), threads, 0, stream>>>(view);
    ++launches;
  }
  if (path_count) {
    seedPaths<<<kernelBlocks(path_count, threads), threads, 0, stream>>>(view, paths, path_count);
    ++launches;
  }
  return launches;
}

}  // namespace ista::csr::cuda_detail

namespace ista::csr {
using namespace cuda_detail;
void CudaDualCsrSession::Impl::propagate()
{
  const auto threads = storage->kernel_threads;
  const auto ensure = [&]<class T>(DeviceBuffer<T>& buffer, Index count) {
    if (memory.ensure(buffer, count))
      ++telemetry.grown_buffers;
    return buffer.data;
  };
  auto& clocks = storage->clock_work;
  clocks.clear();
  for (const auto& seed : host->node_seeds)
    if (seed.state.clock_owner != absent)
      clocks.push_back({seed.node, seed.state.clock_owner, levels.node_to_level[seed.node]});
  std::sort(clocks.begin(), clocks.end(), [](const ClockWorkItem& a, const ClockWorkItem& b) {
    return std::tie(a.level, a.owner, a.node) < std::tie(b.level, b.owner, b.node);
  });
  auto* clock_work = ensure(storage->clock_work_device, clocks.size());
  auto* end_arrivals = ensure(storage->end_arrivals, view.end_count);
  auto* default_required = ensure(storage->default_required, 1);
  if (view.end_count > static_cast<Index>(std::numeric_limits<int>::max()))
    throw std::length_error("CUDA endpoint reduction exceeds Toolkit item capacity");
  std::size_t reduce_bytes = 0;
  if (view.end_count)
    check(cub::DeviceReduce::Max(nullptr, reduce_bytes, end_arrivals, default_required,
                                static_cast<int>(view.end_count), memory.stream()), "CUDA required maximum scratch");
  auto* scratch = ensure(storage->algorithm_scratch, reduce_bytes);
  memory.upload(clocks, clock_work);
  auto launched = [&](const char* operation) {
    check(cudaGetLastError(), operation);
    ++telemetry.cuda.kernel_launches;
  };
  if (view.arc_count) {
    initializeArcs<<<kernelBlocks(view.arc_count, threads), threads, 0, memory.stream()>>>(view);
    launched("CUDA arc initialization launch");
  }
  if (view.rc_net_count) {
    initializeRcDrivers<<<kernelBlocks(view.rc_net_count, threads), threads, 0, memory.stream()>>>(view);
    launched("CUDA RC driver initialization launch");
  }
  // Only actual clock/domain nodes are visited, with no per-level host wait.
  for (unsigned pass = 0; pass < 2; ++pass)
    for (std::size_t begin = 0; begin < clocks.size();) {
      auto end = begin + 1;
      while (end < clocks.size() && clocks[end].level == clocks[begin].level)
        ++end;
      propagateClocks<<<kernelBlocks(end - begin, threads), threads, 0, memory.stream()>>>(
          view, clock_work, begin, end - begin, pass == 0);
      launched("CUDA clock propagation launch");
      begin = end;
    }
  if (view.node_count) {
    propagateNodes<Pass::sequential><<<kernelBlocks(view.node_count, threads), threads, 0, memory.stream()>>>(view, 0, view.node_count);
    launched("CUDA sequential launch");
  }
  const auto& offsets = levels.level_offsets;
  for (std::size_t level = 0; level + 1 < offsets.size(); ++level) {
    const auto count = offsets[level + 1] - offsets[level];
    propagateNodes<Pass::slew><<<kernelBlocks(count, threads), threads, 0, memory.stream()>>>(view, offsets[level], count);
    launched("CUDA data slew launch");
  }
  for (std::size_t level = 0; level + 1 < offsets.size(); ++level) {
    const auto count = offsets[level + 1] - offsets[level];
    propagateNodes<Pass::arrival><<<kernelBlocks(count, threads), threads, 0, memory.stream()>>>(view, offsets[level], count);
    launched("CUDA data arrival launch");
  }
  if (view.end_count) {
    collectEndArrivals<<<kernelBlocks(view.end_count, threads), threads, 0, memory.stream()>>>(view, end_arrivals);
    launched("CUDA endpoint arrival collection launch");
    check(cub::DeviceReduce::Max(scratch, reduce_bytes, end_arrivals, default_required,
                                static_cast<int>(view.end_count), memory.stream()), "CUDA required maximum");
    ++telemetry.cuda.device_algorithm_calls;
    initializeRequired<<<kernelBlocks(view.end_count, threads), threads, 0, memory.stream()>>>(view, default_required);
    launched("CUDA required initialization launch");
  }
  if (view.endpoint_count) {
    endpointRequired<<<kernelBlocks(view.endpoint_count, threads), threads, 0, memory.stream()>>>(view);
    launched("CUDA endpoint required launch");
  }
  for (std::size_t level = offsets.size() - 1; level > 0; --level) {
    const auto count = offsets[level] - offsets[level - 1];
    propagateNodes<Pass::required><<<kernelBlocks(count, threads), threads, 0, memory.stream()>>>(view, offsets[level - 1], count);
    launched("CUDA reverse required launch");
  }
  if (view.node_count) {
    propagateNodes<Pass::slack><<<kernelBlocks(view.node_count, threads), threads, 0, memory.stream()>>>(view, 0, view.node_count);
    launched("CUDA slack launch");
  }
}
}  // namespace ista::csr
