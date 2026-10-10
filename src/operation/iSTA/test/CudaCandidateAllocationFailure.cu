// Test executable only: no failpoint or branch is linked into production.
#include <cuda_runtime_api.h>
#include <utility>
#include "IncreGpuStaBuildConfig.hpp"

thread_local bool ista_test_fail_cuda_allocation = false;
extern "C" cudaError_t __real_cudaMalloc(void**, std::size_t);
extern "C" cudaError_t __wrap_cudaMalloc(void** pointer, std::size_t size)
{
  if constexpr (ista::incregpusta_debug_checks_enabled) {
    if (std::exchange(ista_test_fail_cuda_allocation, false)) return cudaErrorMemoryAllocation;
  }
  return __real_cudaMalloc(pointer, size);
}
