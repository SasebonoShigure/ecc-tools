// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr::test {

void requireCpuReference(bool condition, const std::string& message);

[[nodiscard]] CsrReferenceInput makeCpuReferenceNetFixture();
[[nodiscard]] CsrReferenceInput makeCpuReferenceCellFixture();
[[nodiscard]] CsrReferenceInput makeCpuReferenceCycleFixture();
[[nodiscard]] CsrReferenceInput makeCpuReferencePropertyFixture(std::uint64_t seed);
void refreshCpuReferenceIdentity(CsrReferenceInput& input);

[[nodiscard]] std::vector<std::uint8_t> cpuReferenceCanonicalBytes(const CsrReferenceInput& input);
[[nodiscard]] bool exactTimingResultEqual(const CsrTimingResult& lhs, const CsrTimingResult& rhs, std::string* mismatch = nullptr);
void requireExactTimingResult(const CsrTimingResult& actual, const CsrTimingResult& expected, const std::string& name);
void requireExactComputationResult(const CsrComputationResult& actual, const CsrComputationResult& expected, const std::string& name);
void runExactTimingResultComparatorCanaries();

// The CPU and CUDA contract executables provide their own compute entrypoint
// so the same literal arc/path expectations exercise both implementations.
[[nodiscard]] CsrComputationResult computeContractTiming(const CsrReferenceInput& input);
void runWholeResultLiteralContract();
void runCpuReferenceOutcomeContracts();
void runComputedNumericFailureContracts();
void runCpuReferenceArcContracts();
void runCpuReferencePropagationContracts();

}  // namespace ista::csr::test
