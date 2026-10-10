// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"

namespace ista::csr {
namespace {
__global__ void collectCandidateTiming(cuda_detail::DeviceView view, BufferCandidateTiming* output)
{
  using namespace cuda_detail;
  for (Index id = blockIdx.x * blockDim.x + threadIdx.x; id < view.node_count; id += blockDim.x * gridDim.x) {
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
      for (Index slot = view.path_offsets[row]; slot < view.path_offsets[row + kPathSourceCount * kTransitionCount]; ++slot) {
        if (view.paths[slot].present && isfinite(view.paths[slot].arrival)) {
          result.setup_slack = node.slack;
          break;
        }
      }
    }
    output[id] = result;
  }
}
}  // namespace

const std::vector<BufferCandidateTiming>& CudaDualCsrSession::Impl::readCandidateTiming()
{
  using namespace cuda_detail;
  if (candidate_timing) return *candidate_timing;
  // Allocate host storage before issuing any asynchronous work.
  std::vector<BufferCandidateTiming> result(view.node_count);
  memory.bindTelemetry(telemetry.cuda);
  try {
    telemetry.grown_buffers += memory.ensure(storage->candidate_timing, view.node_count);
    collectCandidateTiming<<<kernelBlocks(view.node_count, storage->kernel_threads), storage->kernel_threads, 0, memory.stream()>>>(
        view, storage->candidate_timing.data);
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "collect buffer candidate timing");
    memory.record(6);
    memory.download(storage->candidate_timing.data, result);
    memory.record(7);
    memory.synchronize();
    telemetry.cuda.d2h_ms += memory.milliseconds(6, 7);
    candidate_timing.emplace(std::move(result));
    return *candidate_timing;
  } catch (const CudaAllocationError&) {
    // ensure failed before any kernel/download; the committed result is intact.
    throw;
  } catch (const CudaError&) {
    // result remains alive until pending downloads have completed or failed.
    try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
    poisoned = true;
    storage->poisoned = true;
    throw;
  }
}
const std::vector<BufferCandidateTiming>& CudaDualCsrSession::candidateTiming() const
{
  requireCommitted();
  return _impl->readCandidateTiming();
}
}  // namespace ista::csr
