#include "CsrCudaBackend.hpp"

#include <cuda_runtime.h>

#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "CsrCudaKernels.cuh"
#include "CsrCudaMemory.cuh"
#include "CsrCudaPacking.hpp"
#include "CsrReferenceValidator.hpp"

namespace ista::csr {
namespace {

using Clock = std::chrono::steady_clock;
using namespace cuda_detail;

double elapsed(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void describeDevice(CsrCudaTelemetry& telemetry)
{
  int device = 0;
  check(cudaGetDevice(&device), "cudaGetDevice");
  cudaDeviceProp properties{};
  check(cudaGetDeviceProperties(&properties, device), "cudaGetDeviceProperties");
  telemetry.device_name = properties.name;
  check(cudaDriverGetVersion(&telemetry.driver_version), "cudaDriverGetVersion");
  check(cudaRuntimeGetVersion(&telemetry.runtime_version), "cudaRuntimeGetVersion");
}

CsrComputationResult execute(HostData& host, CsrCudaTelemetry& telemetry)
{
  Runtime runtime(telemetry);
  DeviceView view;
  view.node_count = host.nodes.size();
  view.arc_count = host.arcs.size();
  view.child_count = host.children.size();
  view.tag_count = host.tag_count;
  view.clock_count = host.clock_count;
  view.root_count = host.roots.size();
  view.end_count = host.ends.size();
  view.endpoint_count = host.endpoints.size();
  view.out_offsets = runtime.allocate<Index>(host.out_offsets.size());
  view.out_arcs = runtime.allocate<Index>(host.out_arcs.size());
  view.in_offsets = runtime.allocate<Index>(host.in_offsets.size());
  view.in_arcs = runtime.allocate<Index>(host.in_arcs.size());
  view.roots = runtime.allocate<Index>(host.roots.size());
  view.ends = runtime.allocate<Index>(host.ends.size());
  view.endpoints = runtime.allocate<Endpoint>(host.endpoints.size());
  view.arcs = runtime.allocate<Arc>(host.arcs.size());
  view.children = runtime.allocate<Child>(host.children.size());
  view.table_data = runtime.allocate<double>(host.table_data.size());
  view.nodes = runtime.allocate<NodeState>(host.nodes.size());
  view.paths = runtime.allocate<PathState>(host.paths.size());
  view.arc_state = runtime.allocate<ArcState>(host.arc_state.size());
  view.child_state = runtime.allocate<ChildState>(host.child_state.size());
  view.indegrees = runtime.allocate<Index>(host.nodes.size());
  view.order = runtime.allocate<Index>(host.order.size());
  view.node_levels = runtime.allocate<Index>(host.node_levels.size());
  view.level_offsets = runtime.allocate<Index>(host.level_offsets.size());
  view.level_offset_count = runtime.allocate<Index>(1);
  view.status = runtime.allocate<Status>(1);
  Status status = Status::success;
  Index offset_count = 0;

  runtime.record(0);
  runtime.upload(host.out_offsets, view.out_offsets);
  runtime.upload(host.out_arcs, view.out_arcs);
  runtime.upload(host.in_offsets, view.in_offsets);
  runtime.upload(host.in_arcs, view.in_arcs);
  runtime.upload(host.roots, view.roots);
  runtime.upload(host.ends, view.ends);
  runtime.upload(host.endpoints, view.endpoints);
  runtime.upload(host.arcs, view.arcs);
  runtime.upload(host.children, view.children);
  runtime.upload(host.table_data, view.table_data);
  runtime.upload(host.nodes, view.nodes);
  runtime.upload(host.paths, view.paths);
  runtime.upload(host.arc_state, view.arc_state);
  runtime.upload(host.child_state, view.child_state);
  runtime.upload(&status, view.status, 1);
  runtime.upload(&offset_count, view.level_offset_count, 1);
  runtime.record(1);

  launchLevelization(view, runtime.stream());
  ++telemetry.kernel_launches;
  check(cudaGetLastError(), "CUDA FIFO levelization launch");
  runtime.record(2);
  telemetry.kernel_launches += launchPropagation(view, runtime.stream());
  check(cudaGetLastError(), "CUDA full propagation launch");
  runtime.record(3);

  runtime.download(view.status, &status, 1);
  runtime.download(view.level_offset_count, &offset_count, 1);
  // Inspect completion before downloading result storage, which may not have
  // been initialized by a kernel that rejected a cycle or a numeric failure.
  runtime.synchronize();
  if (status == Status::success) {
    if (offset_count == 0 || offset_count > host.level_offsets.size())
      throw std::runtime_error("CUDA returned invalid level offset count");
    host.level_offsets.resize(offset_count);
    runtime.download(view.nodes, host.nodes);
    runtime.download(view.paths, host.paths);
    runtime.download(view.arc_state, host.arc_state);
    runtime.download(view.child_state, host.child_state);
    runtime.download(view.order, host.order);
    runtime.download(view.node_levels, host.node_levels);
    runtime.download(view.level_offsets, host.level_offsets);
  }
  runtime.record(4);
  runtime.synchronize();
  telemetry.h2d_ms = runtime.milliseconds(0, 1);
  telemetry.levelization_ms = runtime.milliseconds(1, 2);
  telemetry.propagation_ms = runtime.milliseconds(2, 3);
  telemetry.d2h_ms = runtime.milliseconds(3, 4);
  switch (status) {
    case Status::success: return CsrComputationResult{unpack(std::move(host))};
    case Status::cycle: return CsrComputationResult{CsrComputationCode::kCycle};
    case Status::numeric: return CsrComputationResult{CsrComputationCode::kNumericFailure};
    case Status::invariant: return CsrComputationResult{CsrComputationCode::kInvariantViolation};
  }
  throw std::runtime_error("CUDA returned an unknown computation status");
}

}  // namespace

void CsrCudaBackend::requireDevice()
{
  int count = 0;
  check(cudaGetDeviceCount(&count), "cudaGetDeviceCount");
  if (count == 0)
    throw std::runtime_error("CUDA STA requested but no CUDA device is available");
  CsrCudaTelemetry telemetry;
  describeDevice(telemetry);
}

CsrCudaComputation CsrCudaBackend::computeWithTelemetry(const CsrReferenceInput& input) const
{
  const auto started = Clock::now();
  CsrCudaComputation output;
  requireDevice();
  describeDevice(output.telemetry);
  if (!validateReferenceInput(input).empty()) {
    output.computation = CsrComputationResult{CsrComputationCode::kInvariantViolation};
  } else if (!input.unsupported_reasons.empty()) {
    output.computation = CsrComputationResult{CsrReferenceSupportResult{input.unsupported_reasons}};
  } else {
    const auto encode_started = Clock::now();
    auto host = pack(input);
    output.telemetry.encode_ms = elapsed(encode_started);
    output.computation = execute(host, output.telemetry);
  }
  output.telemetry.end_to_end_ms = elapsed(started);
  return output;
}

}  // namespace ista::csr
