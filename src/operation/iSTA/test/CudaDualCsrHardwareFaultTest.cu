#include <iostream>

#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCudaMemory.cuh"
#include "CudaDualCsrDebugState.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"
namespace {
using namespace ista;
using namespace csr;
using csr::test::patchFor;
using timing_dual_csr::test::require;
// Isolated development process: create a real asynchronous device failure
// without resetting live stream handles (unsupported by this WSL driver).
__global__ void invalidDeviceAddress()
{
  *static_cast<volatile int*>(nullptr) = 1;
}
void actualCudaFault(const std::string& operation, CudaLevelizationMode levelization)
{
  if constexpr (!incregpusta_debug_checks_enabled)
    throw std::invalid_argument("fault experiments require debug checks ON");
  const auto input = csr::test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  CudaDualCsrSession owner(input, water);
  auto patch = patchFor(input, 0, water, water);
  std::optional<CudaDualCsrSession> candidate;
  if (operation == "--debug-context-fault")
    candidate.emplace(owner.stage(patch, levelization));
  invalidDeviceAddress<<<1, 1>>>();
  require(cudaGetLastError() == cudaSuccess, "fault kernel did not launch");
  require(cudaDeviceSynchronize() == cudaErrorIllegalAddress, "device fault did not reach synchronization");
  bool failed = false;
  try {
    if (operation == "--debug-context-fault")
      static_cast<void>(candidate->debugState());
    else
      static_cast<void>(owner.stage(patch, levelization));
  } catch (const cuda_detail::CudaError&) {
    failed = true;
  }
  require(failed && owner.poisoned() && (!candidate || !owner.publish(std::move(*candidate))), "real CUDA failure left originating owner publishable");
  require(!owner.close(), "real CUDA cleanup failure was reported as success");
  candidate.reset();  // Abandoning a candidate after owner close must not touch freed device memory.
}
void actualAllocationFailure()
{
  CsrCudaBackend::requireDevice();
  std::size_t free_bytes = 0, total_bytes = 0;
  require(cudaMemGetInfo(&free_bytes, &total_bytes) == cudaSuccess, "CUDA memory query failed");
  CsrCudaTelemetry telemetry;
  cuda_detail::Runtime memory(telemetry);
  bool overflow = false;
  try {
    static_cast<void>(memory.allocate<cuda_detail::Arc>(std::numeric_limits<std::size_t>::max()));
  } catch (const std::length_error&) {
    overflow = true;
  }
  require(overflow && telemetry.allocations == 0, "byte capacity overflow reached cudaMalloc");
  bool failed = false;
  try {
    static_cast<void>(memory.allocate<std::uint8_t>(std::numeric_limits<std::size_t>::max() / 2));
  } catch (const cuda_detail::CudaError&) {
    failed = true;
  }
  require(failed, "oversized real device allocation unexpectedly succeeded");
  require(cudaGetLastError() == cudaSuccess || cudaPeekAtLastError() == cudaSuccess, "CUDA allocation failure remained sticky");
  auto input = csr::test::makeCpuReferenceNetFixture();
  CudaDualCsrSession healthy(input, {2, 2, 1});
  require(!healthy.computationCode(), "fresh GPU session after actual allocation failure failed");
  require(healthy.close() && healthy.close() && memory.close() == cudaSuccess, "healthy or repeated cleanup failed");
}

}  // namespace
int main(int argc, char** argv)
{
  try {
    require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--mixed"), "one hardware fault mode and optional --mixed are required");
    const auto levelization = argc == 3 ? CudaLevelizationMode::kMixed : CudaLevelizationMode::kFull;
    const std::string mode = argv[1];
    if (mode == "--debug-context-fault" || mode == "--stage-context-fault")
      actualCudaFault(mode, levelization);
    else if (mode == "--allocation-failure")
      actualAllocationFailure();
    else
      throw std::invalid_argument("unknown hardware fault mode");
    std::cout << "Actual CUDA failure contract passed: " << mode << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
