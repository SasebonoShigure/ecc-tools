#include <new>
#include <stdexcept>
#include <utility>

#include "AllocationFailureSupport.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"

namespace ista::csr::test {
using namespace timing_dual_csr;
using namespace timing_dual_csr::test;

void runCudaPathDomainAllocationContracts(CudaLevelizationMode mode)
{
  if constexpr (!incregpusta_debug_checks_enabled) return;
  const auto input = makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  const auto& arc = input.arcs.front();
  const ArcSemanticPayload original{arc.key, arc.source, arc.sink, arc.kind, arc.is_clock_arc, arc.is_disabled,
                                   arc.is_loop_disabled, arc.eligibility, arc.transfer, arc.output_load, arc.native_cell_arc_ordinal};
  auto disabled = original;
  disabled.eligibility.data_arrival = false;
  const auto makeOwner = [&] {
    CudaDualCsrSession owner(input, water);
    for (std::uint64_t generation = 0; generation < 2; ++generation) {
      auto patch = patchFor(input, generation, water, water);
      patch.payload_changes = {{arc.id, generation ? disabled : original, generation ? original : disabled}};
      auto candidate = owner.stage(patch, mode);
      require(!candidate.computationCode() && owner.publish(std::move(candidate)), "compact allocation prefix failed");
    }
    return owner;
  };
  auto patch = patchFor(input, 2, water, {3, 3, 1});
  patch.payload_changes = {{arc.id, original, disabled}};
  const auto node = appendNode(2, "path_capacity", AppendedPortRole::kSource);
  patch.node_appends = {node};
  patch.appended_base_out_rows = {{node.id, {}}};
  patch.appended_base_in_rows = {{node.id, {}}};
  patch.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, {NodeId{0}, node.id}};
  patch.ends = ValueReplacement<std::vector<NodeId>>{{NodeId{1}}, {NodeId{1}, node.id}};
  // The third edit compacts tags; the appended empty node also grows result
  // buffers after the compact tag-owner swap. Enumerate actual allocations.
  for (const bool device : {false, true}) {
    std::size_t count{};
    CsrTimingValues expected;
    {
      auto owner = makeOwner();
      static_cast<void>(owner.timingValues());
      if (device) ista_test_cuda_allocation::armCountOnly();
      else ista_test_allocation::armCountOnly();
      auto candidate = owner.stage(patch, mode);
      count = device ? ista_test_cuda_allocation::disable() : ista_test_allocation::disable();
      require(!candidate.computationCode() && count > 0, "compact allocation fixture did not compute/allocate");
      expected = candidate.timingValues();
    }
    for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
      auto owner = makeOwner();
      const auto before = owner.timingValues();
      bool failed = false;
      if (device) ista_test_cuda_allocation::armFailure(ordinal);
      else ista_test_allocation::armFailure(ordinal);
      try { auto candidate = owner.stage(patch, mode); }
      catch (const std::bad_alloc&) { failed = !device; }
      catch (const std::runtime_error&) { failed = device && ista_test_cuda_allocation::failureInjected(); }
      catch (...) {
        static_cast<void>(ista_test_allocation::disable());
        static_cast<void>(ista_test_cuda_allocation::disable());
        throw;
      }
      static_cast<void>(ista_test_allocation::disable());
      static_cast<void>(ista_test_cuda_allocation::disable());
      require(failed && !owner.poisoned() && owner.generation().value() == 2 && owner.timingValues() == before,
              "compact allocation failure changed the committed generation at " + std::to_string(ordinal));
      {
        auto verify = owner.stage(patchFor(input, 2, water, water), mode);
        require(!verify.computationCode() && verify.timingValues() == before,
                "compact allocation failure left stale device rows/tags");
      }
      auto retry = owner.stage(patch, mode);
      require(!retry.computationCode() && retry.timingValues() == expected && owner.publish(std::move(retry)),
              "compact allocation failure prevented correct retry/commit");
    }
  }
}
}  // namespace ista::csr::test
