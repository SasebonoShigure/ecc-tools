#include <string>
#include <utility>
#include <variant>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrValidatorTestFixture.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr::test {
using namespace timing_dual_csr;

void runCudaPathDomainContracts(CudaLevelizationMode mode)
{
  auto input = makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  CudaDualCsrSession owner(input, water);
  const auto baseline = owner.timingValues();
  const auto& arc = input.arcs.front();
  ArcSemanticPayload expected{arc.key, arc.source, arc.sink, arc.kind, arc.is_clock_arc, arc.is_disabled,
                             arc.is_loop_disabled, arc.eligibility, arc.transfer, arc.output_load, arc.native_cell_arc_ordinal};
  CsrTimingValues disabled;
  // Removing/reinstating the only path repeatedly exercises append, compact,
  // both device payload owners, abandonment and querying after another edit.
  for (std::uint64_t generation = 0; generation < 24; ++generation) {
    auto replacement = expected;
    replacement.eligibility.data_arrival = !expected.eligibility.data_arrival;
    auto patch = patchFor(input, generation, water, water);
    patch.payload_changes = {{arc.id, expected, replacement}};
    const auto before = owner.timingValues();
    {
      auto abandoned = owner.stage(patch, mode);
      requireCpuReference(!abandoned.computationCode(), "path domain candidate failed before abandonment");
      static_cast<void>(abandoned.timingValues());
    }
    auto restored = owner.stage(patchFor(input, generation, water, water), mode);
    requireCpuReference(!restored.computationCode() && restored.timingValues() == before,
            "abandoned path compact/append corrupted device rows or committed path slots");
    requireCpuReference(restored.close(), "abandoned verification could not restore the owner");

    if constexpr (incregpusta_debug_checks_enabled) {
      auto invalid = patch;
      invalid.payload_changes.front().expected.output_load = outputLoad(123.0);
      auto failed = owner.stage(invalid, mode);
      requireCpuReference(failed.computationCode() == CsrComputationCode::kInvariantViolation && !owner.poisoned(),
              "path update preceding device expected-value failure was not restored");
      auto verify = owner.stage(patchFor(input, generation, water, water), mode);
      requireCpuReference(!verify.computationCode() && verify.timingValues() == before,
              "failed path compact/append left device rows changed");
    }
    auto candidate = owner.stage(patch, mode);
    requireCpuReference(!candidate.computationCode() && owner.publish(std::move(candidate)),
            "path domain update after rollback could not commit");
    const auto& timing = owner.timingValues();
    if (!generation) disabled = timing;
    requireCpuReference(timing == (replacement.eligibility.data_arrival ? baseline : disabled),
            "long path-domain sequence changed the result of the same graph");
    const auto readback = owner.telemetry().cuda.result_d2h_bytes;
    requireCpuReference(&owner.timingValues() == &timing && owner.telemetry().cuda.result_d2h_bytes == readback,
            "path-domain query cache did not retain the current generation");

    if constexpr (incregpusta_debug_checks_enabled) {
      input.arcs.front().eligibility = replacement.eligibility;
      const auto cpu = CsrCpuReferenceBackend{}.compute(FlatCsrEdgeView(input), FlatTimingPayloadView(input));
      const auto* reference = std::get_if<CsrTimingResult>(&cpu.outcome);
      requireCpuReference(reference != nullptr, "path-domain independent CPU computation failed");
      CsrTimingResult actual;
      static_cast<CsrTimingValues&>(actual) = timing;
      actual.schema_version = reference->schema_version;
      actual.source_identity = reference->source_identity;
      std::string mismatch;
      requireCpuReference(exactTimingResultEqual(actual, *reference, &mismatch), "path domain differs from CPU: " + mismatch);
    }
    expected = std::move(replacement);
  }
}
}  // namespace ista::csr::test
