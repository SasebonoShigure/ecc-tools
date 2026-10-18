// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"
#include <algorithm>

namespace ista::csr {
namespace {
__global__ void collectCandidateTiming(cuda_detail::DeviceView view, const cuda_detail::Index* nodes,
    cuda_detail::Index count, BufferCandidateTiming* output)
{
  using namespace cuda_detail;
  for (Index slot = blockIdx.x * blockDim.x + threadIdx.x; slot < count; slot += blockDim.x * gridDim.x) {
    const auto id = nodes[slot];
    const auto& node = view.nodes[id];
    BufferCandidateTiming result;
    result.clock_point = node.clock_point;
    for (unsigned transition = 0; transition < kTransitionCount; ++transition) {
      const auto data = node.slew[transition], clock = node.physical_clock[transition].slew;
      if (data.present && isfinite(data.value) && data.value >= 0.0)
        result.data_slew = fmax(fmax(0.0, result.data_slew), data.value);
      if (clock.present && isfinite(clock.value) && clock.value >= 0.0)
        result.clock_slew = fmax(fmax(0.0, result.clock_slew), clock.value);
    }
    if (!node.clock_point && isfinite(node.arrival) && isfinite(node.required) && isfinite(node.slack)) {
      const auto row = id * kAnalysisCount * kPathSourceCount * kTransitionCount;
      for (Index lane = 0; lane < kPathSourceCount * kTransitionCount; ++lane)
        for (Index slot = view.path_rows[row + lane].begin; slot < view.path_rows[row + lane].end; ++slot)
          if (view.paths[slot].present && isfinite(view.paths[slot].arrival)) {
            result.setup_slack = node.slack;
            break;
          }
    }
    output[slot] = result;
  }
}
}  // namespace

const std::vector<BufferCandidateTiming>& CudaDualCsrSession::Impl::readCandidateTiming(std::span<const NodeId> nodes)
{
  using namespace cuda_detail;
  static const std::vector<BufferCandidateTiming> empty;
  if (nodes.empty()) return empty;
  if (candidate_timing && std::ranges::equal(nodes, candidate_timing->nodes,
      [](NodeId id, Index cached) { return id.value() == cached; })) return candidate_timing->values;
  // Both upload source and download destination outlive the delivery wait.
  CandidateTiming result;
  result.nodes.reserve(nodes.size());
  for (const auto id : nodes) {
    demand(id.value() < view.node_count, "candidate node is out of range");
    result.nodes.push_back(id.value());
  }
  result.values.resize(nodes.size());
  if (values) {
    for (std::size_t slot = 0; slot < nodes.size(); ++slot)
      result.values[slot] = bufferCandidateTiming(*values, nodes[slot]);
    candidate_timing.emplace(std::move(result));
    return candidate_timing->values;
  }
  memory.bindTelemetry(telemetry.cuda);
  try {
    telemetry.grown_buffers += memory.ensure(storage->candidate_nodes, nodes.size());
    telemetry.grown_buffers += memory.ensure(storage->candidate_timing, nodes.size());
    memory.upload(result.nodes, storage->candidate_nodes.data);
    collectCandidateTiming<<<kernelBlocks(nodes.size(), storage->kernel_threads), storage->kernel_threads, 0, memory.stream()>>>(
        view, storage->candidate_nodes.data, nodes.size(), storage->candidate_timing.data);
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "collect buffer candidate timing");
    const auto prior_readback = telemetry.cuda.d2h_bytes;
    const auto prior_wait = telemetry.cuda.stream_wait_ms;
    memory.record(6);
    memory.download(storage->candidate_timing.data, result.values);
    memory.record(7);
    memory.synchronize();
    telemetry.cuda.delivery_d2h_ms += memory.milliseconds(6, 7);
    telemetry.cuda.candidate_d2h_bytes += telemetry.cuda.d2h_bytes - prior_readback;
    telemetry.cuda.candidate_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
    candidate_timing.emplace(std::move(result));
    return candidate_timing->values;
  } catch (const CudaAllocationError&) {
    // All growth precedes upload; the committed result and previous cache remain intact.
    throw;
  } catch (const CudaError&) {
    try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
    poisoned = true;
    storage->poisoned = true;
    throw;
  }
}
const std::vector<BufferCandidateTiming>& CudaDualCsrSession::candidateTiming(std::span<const NodeId> nodes) const
{
  requireCommitted();
  return _impl->readCandidateTiming(nodes);
}
}  // namespace ista::csr
