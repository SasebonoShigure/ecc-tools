#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCudaBackend.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::csr::test {

CsrComputationResult computeContractTiming(const CsrReferenceInput& input)
{
  auto actual = CsrCudaBackend{}.computeWithTelemetry(input);
  if constexpr (incregpusta_debug_checks_enabled)
    requireExactComputationResult(actual.computation, CsrCpuReferenceBackend{}.compute(input), "CUDA full all-field differential");
  return std::move(actual.computation);
}

void runCudaCases()
{
  runWholeResultLiteralContract();
  runCpuReferenceArcContracts();
  runCpuReferencePropagationContracts();
  runComputedNumericFailureContracts();
  const auto cycle = computeContractTiming(makeCpuReferenceCycleFixture());
  requireCpuReference(cycle.outcome == CsrComputationResult{CsrComputationCode::kCycle}.outcome, "CUDA must reject cycle");
  auto malformed = makeCpuReferenceNetFixture();
  malformed.out_row_offsets.pop_back();
  const auto invalid = computeContractTiming(malformed);
  requireCpuReference(invalid.outcome == CsrComputationResult{CsrComputationCode::kInvariantViolation}.outcome, "CUDA must reject malformed CSR");
  for (std::uint64_t seed : {1ULL, 7ULL, 42ULL, 20260901ULL, 0xdeadbeefULL})
    for (std::uint64_t sample = 0; sample < 16; ++sample) {
      const auto input = makeCpuReferencePropertyFixture(seed + sample);
      const auto first = computeContractTiming(input);
      requireCpuReference(std::holds_alternative<CsrTimingResult>(first.outcome), "CUDA property fixture must succeed");
      if constexpr (incregpusta_debug_checks_enabled)
        requireExactComputationResult(computeContractTiming(input), first, "CUDA cold/warm repeat");
    }
}

void runPerformance()
{
  if constexpr (incregpusta_debug_checks_enabled)
    throw std::invalid_argument("CUDA performance requires ECC_INCREGPUSTA_DEBUG_CHECKS=OFF");
  CsrCudaBackend backend;
  const auto input = makeCpuReferenceCellFixture();
  (void) backend.computeWithTelemetry(input);
  for (int sample = 0; sample < 3; ++sample) {
    const auto output = backend.computeWithTelemetry(input);
    if (!std::holds_alternative<CsrTimingResult>(output.computation.outcome))
      throw std::runtime_error("CUDA performance workload failed");
    const auto& t = output.telemetry;
    std::cout << "{\"sample\":" << sample << ",\"debug_checks_enabled\":false,\"correctness_comparison\":\"not_run\","
              << "\"end_to_end_ms\":" << t.end_to_end_ms << ",\"h2d_bytes\":" << t.h2d_bytes << ",\"d2h_bytes\":" << t.d2h_bytes
              << ",\"allocations\":" << t.allocations << ",\"kernel_launches\":" << t.kernel_launches << "}\n";
  }
}

}  // namespace ista::csr::test

int main(int argc, char** argv)
{
  try {
    ista::csr::CsrCudaBackend::requireDevice();
    if (argc == 2 && std::string(argv[1]) == "--performance")
      ista::csr::test::runPerformance();
    else if (argc == 1)
      ista::csr::test::runCudaCases();
    else
      throw std::invalid_argument("usage: ista_csr_cuda_contract_test [--performance]");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
