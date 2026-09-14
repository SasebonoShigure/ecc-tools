#include <iostream>

#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCudaMemory.cuh"
#include "CudaDualCsrDebugState.hpp"
#include "CudaDualCsrSession.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"
namespace {
using namespace ista;
using namespace csr;
using timing_dual_csr::test::require;
CudaDualCsrPatch patchFor(const CsrReferenceInput& input, std::uint64_t generation, timing_edit::AppendOnlyHighWater before,
                          timing_edit::AppendOnlyHighWater after)
{
  CudaDualCsrPatch patch;
  patch.session_token = timing_edit::TimingEditSessionToken{input.source_identity.session_token};
  patch.expected_generation = timing_edit::TransactionGeneration{generation};
  patch.target_generation = timing_edit::TransactionGeneration{generation + 1};
  patch.before = before;
  patch.after = after;
  return patch;
}
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
  auto candidate = owner.stage(patch, levelization), sibling = owner.stage(patch, levelization);
  invalidDeviceAddress<<<1, 1>>>();
  require(cudaGetLastError() == cudaSuccess, "fault kernel did not launch");
  require(cudaDeviceSynchronize() == cudaErrorIllegalAddress, "device fault did not reach synchronization");
  bool failed = false;
  try {
    if (operation == "--debug-context-fault")
      static_cast<void>(candidate.debugState());
    else
      static_cast<void>(owner.stage(patch, levelization));
  } catch (const cuda_detail::CudaError&) {
    failed = true;
  }
  require(failed && owner.poisoned() && !owner.publish(std::move(sibling)), "real CUDA failure left originating owner publishable");
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
