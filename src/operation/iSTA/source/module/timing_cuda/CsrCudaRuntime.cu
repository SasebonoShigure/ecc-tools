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
#include "CudaDualCsrInternal.cuh"
#include "CsrReferenceValidator.hpp"

namespace ista::csr {
namespace {

using Clock = std::chrono::steady_clock;
using namespace cuda_detail;

double elapsed(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}



}  // namespace

void CsrCudaBackend::requireDevice()
{
  int count = 0;
  check(cudaGetDeviceCount(&count), "cudaGetDeviceCount");
  if (count == 0)
    throw std::runtime_error("CUDA STA requested but no CUDA device is available");
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
    // This public entry already validated the external input above.
    CudaDualCsrSession session(std::make_shared<CudaDualCsrSession::Impl>(
        input, timing_edit::AppendOnlyHighWater{input.nodes.size(), 0, input.arcs.size()}));
    if (const auto code = session.computationCode()) {
      output.computation = CsrComputationResult{*code};
    } else {
      CsrTimingResult result;
      result.schema_version = input.schema_version;
      result.source_identity = input.source_identity;
      static_cast<void>(session.timingValues());
      static_cast<CsrTimingValues&>(result) = std::move(*session._impl->values);
      output.computation = CsrComputationResult{std::move(result)};
    }
    output.telemetry = session.telemetry().cuda;
    if (!session.close())
      throw CudaError("CUDA STA cleanup failed");
  }
  output.telemetry.end_to_end_ms = elapsed(started);
  return output;
}

}  // namespace ista::csr
