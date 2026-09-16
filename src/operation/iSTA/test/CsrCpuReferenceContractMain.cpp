#include <exception>
#include <iostream>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"

namespace ista::csr::test {
void runCpuReferenceClockContracts();
void runCpuReferenceSequentialContracts();
void runCpuReferenceCheckContracts();
CsrComputationResult computeContractTiming(const CsrReferenceInput& input)
{
  return CsrCpuReferenceBackend{}.compute(input);
}
}  // namespace ista::csr::test

int main()
{
  try {
    ista::csr::test::runExactTimingResultComparatorCanaries();
    ista::csr::test::runCpuReferenceOutcomeContracts();
    ista::csr::test::runWholeResultLiteralContract();
    ista::csr::test::runCpuReferenceArcContracts();
    ista::csr::test::runCpuReferencePropagationContracts();
    ista::csr::test::runCpuReferenceClockContracts();
    ista::csr::test::runCpuReferenceSequentialContracts();
    ista::csr::test::runCpuReferenceCheckContracts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
