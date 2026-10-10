// Shared native-test allocation interception. Never linked into production.
#include <cstddef>
#include <cstdlib>
#include <new>
#include "AllocationFailureSupport.hpp"

namespace ista_test_allocation {
namespace {

struct AllocationState
{
  bool enabled{false};
  bool inject_failure{false};
  std::size_t failure_ordinal{0};
  std::size_t allocation_count{0};
  bool (*condition)(void*) noexcept {nullptr};
  void* context{nullptr};
  bool failure_injected{false};
};

thread_local AllocationState state;

void beforeAllocation()
{
  if (!state.enabled)
    return;
  const std::size_t ordinal = state.allocation_count++;
  if ((state.inject_failure && ordinal == state.failure_ordinal) || (state.condition && state.condition(state.context))) {
    state.failure_injected = true;
    state.inject_failure = false;
    state.condition = nullptr;
    throw std::bad_alloc{};
  }
}

}  // namespace

void armCountOnly() noexcept
{
  state = AllocationState{true, false, 0, 0};
}

void armFailure(std::size_t ordinal) noexcept
{
  state = AllocationState{true, true, ordinal, 0};
}

void armFailureWhen(bool (*condition)(void*) noexcept, void* context) noexcept
{
  state = AllocationState{};
  state.enabled = true;
  state.condition = condition;
  state.context = context;
}

[[nodiscard]] bool failureInjected() noexcept { return state.failure_injected; }

[[nodiscard]] std::size_t disable() noexcept
{
  const std::size_t result = state.allocation_count;
  state = AllocationState{};
  return result;
}

[[nodiscard]] void* allocate(std::size_t size)
{
  beforeAllocation();
  if (void* allocation = std::malloc(size == 0 ? 1 : size))
    return allocation;
  throw std::bad_alloc{};
}

[[nodiscard]] void* allocateAligned(std::size_t size, std::size_t alignment)
{
  beforeAllocation();
  void* allocation = nullptr;
  if (::posix_memalign(&allocation, alignment, size == 0 ? 1 : size) == 0)
    return allocation;
  throw std::bad_alloc{};
}

}  // namespace ista_test_allocation

void* operator new(std::size_t size)
{
  return ista_test_allocation::allocate(size);
}

void* operator new[](std::size_t size)
{
  return ista_test_allocation::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return ista_test_allocation::allocate(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return ista_test_allocation::allocate(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
  return ista_test_allocation::allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
  return ista_test_allocation::allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  try {
    return ista_test_allocation::allocateAligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  try {
    return ista_test_allocation::allocateAligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* allocation) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, const std::nothrow_t&) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, const std::nothrow_t&) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::align_val_t) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, std::align_val_t) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(allocation);
}
