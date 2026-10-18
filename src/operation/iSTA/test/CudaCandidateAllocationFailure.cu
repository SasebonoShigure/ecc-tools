// Test executable only: no failpoint or branch is linked into production.
#include <cuda_runtime_api.h>
#include <limits>
#include "AllocationFailureSupport.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista_test_cuda_allocation {
namespace {
thread_local bool enabled = false, injected = false;
thread_local std::size_t count = 0, failure = std::numeric_limits<std::size_t>::max();
}
void armCountOnly() noexcept
{
  enabled = true;
  injected = false;
  count = 0;
  failure = std::numeric_limits<std::size_t>::max();
}
void armFailure(std::size_t ordinal) noexcept { armCountOnly(); failure = ordinal; }
bool failureInjected() noexcept { return injected; }
std::size_t disable() noexcept { enabled = false; return count; }
bool failAllocation() noexcept
{
  if (!enabled) return false;
  if (count++ != failure) return false;
  injected = true;
  failure = std::numeric_limits<std::size_t>::max();
  return true;
}
}  // namespace ista_test_cuda_allocation
extern "C" cudaError_t __real_cudaMalloc(void**, std::size_t);
extern "C" cudaError_t __wrap_cudaMalloc(void** pointer, std::size_t size)
{
  if constexpr (ista::incregpusta_debug_checks_enabled)
    if (ista_test_cuda_allocation::failAllocation()) return cudaErrorMemoryAllocation;
  return __real_cudaMalloc(pointer, size);
}
