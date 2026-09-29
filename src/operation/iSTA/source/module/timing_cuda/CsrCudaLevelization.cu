#include <cub/device/device_scan.cuh>
#include <cub/device/device_select.cuh>

#include "CsrCudaKernels.cuh"
#include "CudaDualCsrInternal.cuh"

namespace ista::csr {
using namespace cuda_detail;
namespace {
__global__ void initializeDegrees(DeviceView view)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index node = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; node < view.node_count;
       node += static_cast<Index>(gridDim.x) * blockDim.x) {
    Index degree = 0;
    for (Index offset = view.in_offsets[node]; offset < view.in_offsets[node + 1]; ++offset) {
      const auto id = view.in_arcs[offset];
      if (id == absent)
        continue;
      if (id >= view.arc_count || view.arcs[id].sink != node || !view.arcs[id].active || view.arcs[id].source >= view.node_count) {
        recordFailure(view.status, Status::invariant);
        continue;
      }
      degree += view.arcs[id].levelization;
    }
    view.indegrees[node] = degree;
    view.node_levels[node] = 0;
  }
}
__global__ void initializeRoots(DeviceView view)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index position = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; position < view.root_count;
       position += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto node = view.roots[position];
    if (node >= view.node_count || view.indegrees[node] != 0
        || atomicCAS(reinterpret_cast<unsigned long long*>(view.node_levels + node), 0ULL, 1ULL) != 0ULL) {
      recordFailure(view.status, Status::invariant);
      continue;
    }
    view.order[position] = node;
  }
}
__global__ void checkRoots(DeviceView view)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index node = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; node < view.node_count;
       node += static_cast<Index>(gridDim.x) * blockDim.x)
    if ((view.indegrees[node] == 0) != (view.node_levels[node] != 0))
      recordFailure(view.status, Status::invariant);
}
__global__ void countFrontier(DeviceView view, Index head, Index count, Index* offsets)
{
  if (blockIdx.x == 0 && threadIdx.x == 0)
    offsets[count] = 0;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    offsets[i] = 0;
    if (readStatus(view.status) != Status::success)
      continue;
    const auto node = view.order[head + i];
    if (node >= view.node_count || view.out_offsets[node + 1] < view.out_offsets[node]) {
      recordFailure(view.status, Status::invariant);
      continue;
    }
    const auto base_count = view.out_offsets[node + 1] - view.out_offsets[node];
    const auto aux = view.aux_rows ? view.aux_rows[node] : AuxRow{};
    if ((aux.count && !aux.arcs) || aux.count > absent - base_count)
      recordFailure(view.status, Status::invariant);
    else
      offsets[i] = base_count + aux.count;
  }
}
__global__ void processFrontier(DeviceView view, Index head, Index count, const Index* offsets,
                                Index edges, Index ordinal_base, Index* wake_keys, Index* candidates)
{
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < edges;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    candidates[i] = absent;
    if (readStatus(view.status) != Status::success)
      continue;
    // Offsets retain frontier-node order, followed by Base then Aux row order.
    Index low = 0, high = count;
    while (low + 1 < high) {
      const Index middle = low + (high - low) / 2;
      if (offsets[middle] <= i)
        low = middle;
      else
        high = middle;
    }
    const auto node = view.order[head + low];
    const auto position = i - offsets[low];
    const auto base_count = view.out_offsets[node + 1] - view.out_offsets[node];
    const bool aux = position >= base_count;
    const auto id = aux ? view.aux_rows[node].arcs[position - base_count] : view.out_arcs[view.out_offsets[node] + position];
    if (id == absent)
      continue;
    if (id >= view.arc_count || !view.arcs[id].active || view.arcs[id].source != node || view.arcs[id].sink >= view.node_count) {
      recordFailure(view.status, Status::invariant);
      continue;
    }
    if constexpr (incregpusta_debug_checks_enabled)
      if (view.storage_visits)
        atomicAdd(reinterpret_cast<unsigned long long*>(view.storage_visits + (aux ? 1 : 0)), 1ULL);
    const auto& arc = view.arcs[id];
    if (!arc.levelization)
      continue;
    const auto old = atomicAdd(reinterpret_cast<unsigned long long*>(view.indegrees + arc.sink), ~0ULL);
    if (old == 0) {
      recordFailure(view.status, Status::invariant);
      continue;
    }
    atomicMax(reinterpret_cast<unsigned long long*>(wake_keys + arc.sink),
              static_cast<unsigned long long>(ordinal_base + i + 1));
    candidates[i] = arc.sink;
  }
}
__global__ void selectWakeCandidates(DeviceView view, Index count, Index ordinal_base, Index level,
                                     const Index* wake_keys, Index* candidates)
{
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    if (readStatus(view.status) != Status::success) {
      candidates[i] = absent;
      continue;
    }
    const auto node = candidates[i];
    if (node == absent)
      continue;
    if (view.indegrees[node] != 0 || wake_keys[node] != ordinal_base + i + 1)
      candidates[i] = absent;
    else if (view.node_levels[node] != 0) {
      candidates[i] = absent;
      recordFailure(view.status, Status::invariant);
    } else
      view.node_levels[node] = level;
  }
}
struct Present
{
  __host__ __device__ bool operator()(Index node) const { return node != absent; }
};
__global__ void finishLevels(DeviceView view, Index count, Index processed)
{
  if (blockIdx.x == 0 && threadIdx.x == 0) {
    *view.level_offset_count = count;
    if (processed != view.node_count)
      recordFailure(view.status, Status::cycle);
  }
}
}  // namespace

void CudaDualCsrSession::Impl::levelizeFull()
{
  const auto threads = storage->kernel_threads;
  // CUDA Toolkit 12.8's DeviceSelect item count is an int. Never truncate it.
  demand(view.root_count <= view.node_count, "CUDA full root count exceeds node count");
  if (view.node_count >= static_cast<Index>(std::numeric_limits<int>::max()))
    throw std::length_error("CUDA full frontier exceeds Toolkit item capacity");
  const auto ensure = [&]<class T>(DeviceBuffer<T>& buffer, Index count) {
    if (memory.ensure(buffer, count))
      ++telemetry.grown_buffers;
    return buffer.data;
  };
  auto* wake_keys = ensure(storage->wake_keys, view.node_count);
  auto* selected_count = ensure(storage->frontier_count, 1);
  memory.zero(wake_keys, view.node_count);
  initializeDegrees<<<kernelBlocks(view.node_count, threads), threads, 0, memory.stream()>>>(view);
  check(cudaGetLastError(), "CUDA full indegree launch");
  initializeRoots<<<kernelBlocks(view.root_count, threads), threads, 0, memory.stream()>>>(view);
  check(cudaGetLastError(), "CUDA full roots launch");
  checkRoots<<<kernelBlocks(view.node_count, threads), threads, 0, memory.stream()>>>(view);
  check(cudaGetLastError(), "CUDA full root coverage launch");
  telemetry.cuda.kernel_launches += 3;
  levels.level_offsets = {0};
  if (view.root_count)
    levels.level_offsets.push_back(view.root_count);
  Index head = 0, tail = view.root_count, ordinal_base = 0;
  while (head < tail) {
    const auto count = tail - head;
    auto* offsets = ensure(storage->frontier_offsets, count + 1);
    countFrontier<<<kernelBlocks(count, threads), threads, 0, memory.stream()>>>(view, head, count, offsets);
    check(cudaGetLastError(), "CUDA full frontier count launch");
    ++telemetry.cuda.kernel_launches;
    std::size_t bytes = 0;
    check(cub::DeviceScan::ExclusiveSum(nullptr, bytes, offsets, offsets, static_cast<int>(count + 1), memory.stream()),
          "CUDA full frontier scan scratch");
    auto* scratch = ensure(storage->algorithm_scratch, bytes);
    check(cub::DeviceScan::ExclusiveSum(scratch, bytes, offsets, offsets, static_cast<int>(count + 1), memory.stream()),
          "CUDA full frontier scan");
    ++telemetry.cuda.device_algorithm_calls;
    Index edges = 0;
    Status status = Status::success;
    try {
      memory.download(offsets + count, &edges, 1);
      memory.download(view.status, &status, 1);
      memory.synchronize();
    } catch (...) {
      // Stack-backed decision readbacks cannot outlive this scope.
      try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
      throw;
    }
    if (status != Status::success)
      return;
    if (edges > checkedAdd(host->out_offsets.back(), view.arc_count))
      throw std::runtime_error("CUDA full frontier physical edge count invalid");
    if (edges > static_cast<Index>(std::numeric_limits<int>::max()))
      throw std::length_error("CUDA full frontier edges exceed Toolkit item capacity");
    if (!edges)
      break;
    static_cast<void>(checkedAdd(ordinal_base, edges));
    auto* candidates = ensure(storage->frontier_candidates, edges);
    processFrontier<<<kernelBlocks(edges, threads), threads, 0, memory.stream()>>>(
        view, head, count, offsets, edges, ordinal_base, wake_keys, candidates);
    check(cudaGetLastError(), "CUDA full frontier edges launch");
    selectWakeCandidates<<<kernelBlocks(edges, threads), threads, 0, memory.stream()>>>(
        view, edges, ordinal_base, levels.level_offsets.size(), wake_keys, candidates);
    check(cudaGetLastError(), "CUDA full frontier wake launch");
    telemetry.cuda.kernel_launches += 2;
    // Stable compaction reproduces FIFO wake order without an Arc sort/index.
    bytes = 0;
    check(cub::DeviceSelect::If(nullptr, bytes, candidates, view.order + tail, selected_count,
                              static_cast<int>(edges), Present{}, memory.stream()), "CUDA full compact scratch");
    scratch = ensure(storage->algorithm_scratch, bytes);
    check(cub::DeviceSelect::If(scratch, bytes, candidates, view.order + tail, selected_count,
                              static_cast<int>(edges), Present{}, memory.stream()), "CUDA full compact frontier");
    ++telemetry.cuda.device_algorithm_calls;
    int next_count = 0;
    try {
      memory.download(selected_count, &next_count, 1);
      memory.download(view.status, &status, 1);
      memory.synchronize();
    } catch (...) {
      try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
      throw;
    }
    if (status != Status::success)
      return;
    if (next_count < 0 || static_cast<Index>(next_count) > view.node_count - tail)
      throw std::runtime_error("CUDA full frontier exceeds remaining node capacity");
    head = tail;
    tail += next_count;
    ordinal_base += edges;
    if (next_count)
      levels.level_offsets.push_back(tail);
  }
  // Member-owned upload storage stays alive through execute's delivery wait.
  memory.upload(levels.level_offsets, view.level_offsets);
  finishLevels<<<1, threads, 0, memory.stream()>>>(view, levels.level_offsets.size(), tail);
  check(cudaGetLastError(), "CUDA full frontier completion launch");
  ++telemetry.cuda.kernel_launches;
}
}  // namespace ista::csr
