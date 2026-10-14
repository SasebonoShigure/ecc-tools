// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <atomic>
#include <cstddef>
#include <omp.h>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <string>

#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrTimingScatterTestFixture.hpp"

namespace csr_scatter_allocation_failpoint {
std::atomic<bool> fail_worker{false};
std::atomic<bool> worker_failed{false};
namespace {

struct AllocationState
{
  bool enabled{false};
  bool inject_failure{false};
  std::size_t failure_ordinal{0};
  std::size_t allocation_count{0};
};

thread_local AllocationState state;

void beforeAllocation()
{
  if (fail_worker.load(std::memory_order_relaxed) && omp_in_parallel() && omp_get_thread_num() != 0
      && fail_worker.exchange(false, std::memory_order_relaxed)) {
    worker_failed.store(true, std::memory_order_relaxed);
    throw std::bad_alloc{};
  }
  if (!state.enabled)
    return;
  const std::size_t ordinal = state.allocation_count++;
  if (state.inject_failure && ordinal == state.failure_ordinal)
    throw std::bad_alloc{};
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

}  // namespace csr_scatter_allocation_failpoint

void* operator new(std::size_t size)
{
  return csr_scatter_allocation_failpoint::allocate(size);
}

void* operator new[](std::size_t size)
{
  return csr_scatter_allocation_failpoint::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return csr_scatter_allocation_failpoint::allocate(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return csr_scatter_allocation_failpoint::allocate(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
  return csr_scatter_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
  return csr_scatter_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  try {
    return csr_scatter_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  try {
    return csr_scatter_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
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

namespace ista::csr::test {
namespace {

[[nodiscard]] CsrTimingScatterTestFixture makePoisonedAtomicFixture()
{
  CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(ReferenceNativeFixtureCase::kDiamond);
  fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
  poisonTimingScatterFixture(fixture);
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database()) == fixture.forbidden_bytes, "atomicity setup changed forbidden native state");
  requireTimingScatter(!(readNativeTimingOracle(fixture.database()) == fixture.expected_oracle), "atomicity poison did not change the semantic timing oracle");
  return fixture;
}

void requireForbiddenState(const CsrTimingScatterTestFixture& fixture, const std::vector<std::uint8_t>& expected, const std::string& name)
{
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database()) == expected, name + ": forbidden native state changed");
}

[[nodiscard]] std::size_t measureScatterAllocations()
{
  CsrTimingScatterTestFixture fixture = makePoisonedAtomicFixture();
  const std::uint64_t expected_writes = expectedTimingScatterWrites(fixture.committed_input);
  const std::vector<std::uint8_t> forbidden_before = timingScatterForbiddenBytes(fixture.database());

  std::size_t allocation_count = 0;
  csr_scatter_allocation_failpoint::armCountOnly();
  try {
    auto attempt
        = scatterCsrTimingResult(fixture.database(), fixture.committed_input, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation);
    allocation_count = csr_scatter_allocation_failpoint::disable();
    requireScatterCode(attempt, CsrScatterCode::kSuccess, expected_writes, std::nullopt, "allocation count-only scatter");
  } catch (...) {
    static_cast<void>(csr_scatter_allocation_failpoint::disable());
    throw;
  }

  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), fixture.expected_oracle, "allocation count-only semantic result");
  requireForbiddenState(fixture, forbidden_before, "allocation count-only scatter");
  return allocation_count;
}

void verifyInjectedFailure(std::size_t ordinal)
{
  CsrTimingScatterTestFixture fixture = makePoisonedAtomicFixture();
  const CsrNativeTimingOracle oracle_before = readNativeTimingOracle(fixture.database());
  const std::vector<std::uint8_t> forbidden_before = timingScatterForbiddenBytes(fixture.database());
  const std::string name = "allocation failure ordinal " + std::to_string(ordinal);

  bool caught_bad_alloc = false;
  bool returned = false;
  std::uint64_t returned_writes = 0;
  std::size_t observed_allocations = 0;
  std::exception_ptr unexpected_exception;
  csr_scatter_allocation_failpoint::armFailure(ordinal);
  try {
    auto attempt
        = scatterCsrTimingResult(fixture.database(), fixture.committed_input, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation);
    observed_allocations = csr_scatter_allocation_failpoint::disable();
    returned = true;
    returned_writes = attempt.scatter_result.writes_applied;
  } catch (const std::bad_alloc&) {
    observed_allocations = csr_scatter_allocation_failpoint::disable();
    caught_bad_alloc = true;
  } catch (...) {
    observed_allocations = csr_scatter_allocation_failpoint::disable();
    unexpected_exception = std::current_exception();
  }

  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), oracle_before, name + " semantic zero-write");
  requireForbiddenState(fixture, forbidden_before, name);
  if (unexpected_exception)
    std::rethrow_exception(unexpected_exception);
  requireTimingScatter(!returned || returned_writes == 0, name + ": returned failure reported nonzero writes");
  requireTimingScatter(caught_bad_alloc && !returned, name + ": did not propagate std::bad_alloc");
  requireTimingScatter(observed_allocations == ordinal + 1, name + ": allocation ordinal was not reached exactly");
}

void verifyBeyondEndSuccess(std::size_t allocation_count)
{
  CsrTimingScatterTestFixture fixture = makePoisonedAtomicFixture();
  const std::uint64_t expected_writes = expectedTimingScatterWrites(fixture.committed_input);
  const std::vector<std::uint8_t> forbidden_before = timingScatterForbiddenBytes(fixture.database());
  const std::size_t success_ordinal = allocation_count + 1;

  std::size_t observed_allocations = 0;
  csr_scatter_allocation_failpoint::armFailure(success_ordinal);
  try {
    auto attempt
        = scatterCsrTimingResult(fixture.database(), fixture.committed_input, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation);
    observed_allocations = csr_scatter_allocation_failpoint::disable();
    requireScatterCode(attempt, CsrScatterCode::kSuccess, expected_writes, std::nullopt, "allocation beyond-end scatter");
  } catch (...) {
    static_cast<void>(csr_scatter_allocation_failpoint::disable());
    throw;
  }

  requireTimingScatter(observed_allocations == allocation_count, "allocation beyond-end success count changed");
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), fixture.expected_oracle, "allocation beyond-end semantic result");
  requireForbiddenState(fixture, forbidden_before, "allocation beyond-end scatter");
}

void verifyWorkerAllocationFailure()
{
  auto fixture = makePoisonedAtomicFixture();
  const auto before = readNativeTimingOracle(fixture.database());
  const auto forbidden = timingScatterForbiddenBytes(fixture.database());
  bool caught = false;
  omp_set_dynamic(0);
  omp_set_num_threads(8);
  csr_scatter_allocation_failpoint::fail_worker = true;
  try {
    static_cast<void>(scatterCsrTimingResult(fixture.database(), fixture.committed_input, fixture.result,
                                           fixture.context, fixture.session_token, fixture.graph_generation));
  } catch (const std::bad_alloc&) {
    caught = true;
  } catch (...) {
    csr_scatter_allocation_failpoint::fail_worker = false;
    throw;
  }
  csr_scatter_allocation_failpoint::fail_worker = false;
  requireTimingScatter(caught && csr_scatter_allocation_failpoint::worker_failed, "worker allocation failure did not reach the caller");
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), before, "worker failure changed native timing");
  requireForbiddenState(fixture, forbidden, "worker allocation failure");
}

}  // namespace
}  // namespace ista::csr::test

int main()
{
  try {
    omp_set_num_threads(1);
    const std::size_t allocation_count = ista::csr::test::measureScatterAllocations();
    ista::csr::test::requireTimingScatter(allocation_count > 0, "scatter performed no observable allocations");
    ista::csr::test::requireTimingScatter(allocation_count < std::numeric_limits<std::size_t>::max(),
                                          "scatter allocation count cannot represent the total+1 success ordinal");
    for (std::size_t ordinal = 0; ordinal < allocation_count; ++ordinal)
      ista::csr::test::verifyInjectedFailure(ordinal);
    ista::csr::test::verifyBeyondEndSuccess(allocation_count);
    ista::csr::test::verifyWorkerAllocationFailure();
  } catch (const std::exception& error) {
    std::cerr << "CsrTimingScatterAtomicityContract failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
