// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Arc.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrTimingScatterTestFixture.hpp"
#include "Database.hpp"

namespace ista::csr::test {
namespace {

[[nodiscard]] std::uint64_t parseSeed(int argc, char* argv[])
{
  if (argc != 3 || std::string{argv[1]} != "--seed") {
    throw std::invalid_argument("usage: ista_csr_timing_scatter_property_test --seed <value>");
  }
  std::size_t consumed = 0;
  const unsigned long long parsed = std::stoull(argv[2], &consumed, 0);
  if (consumed != std::string{argv[2]}.size()) {
    throw std::invalid_argument("property seed contains trailing characters");
  }
  return static_cast<std::uint64_t>(parsed);
}

void requireSuccessfulScatter(CsrTimingScatterTestFixture& fixture, const std::string& name)
{
  const CsrTimingResultScatterAttempt attempt
      = scatterCsrTimingResult(fixture.database(), fixture.committed_input, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation);
  requireScatterCode(attempt, CsrScatterCode::kSuccess, expectedTimingScatterWrites(fixture.committed_input), std::nullopt, name);
  requireTimingScatter(!attempt.current_capture_error.has_value(), name + " retained current-capture evidence");
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), fixture.expected_oracle, name);
}

void runProperty(std::uint64_t seed)
{
  CsrTimingScatterTestFixture fixture = makeTimingScatterPropertyFixture(seed);
  fixture.result = materializeTimingResultFromNativeOracle(fixture.committed_input, fixture.expected_oracle);
  const std::vector<CsrContractViolation> result_violations = validateTimingResult(fixture.committed_input, fixture.result);
  if (!result_violations.empty()) {
    throw std::runtime_error("materialized property result violation at " + result_violations.front().field + ": " + result_violations.front().message);
  }

  const std::vector<std::uint8_t> input_before = canonicalReferenceInputBytes(fixture.committed_input);
  const std::vector<std::uint8_t> forbidden_before = timingScatterForbiddenBytes(fixture.database());
  requireTimingScatter(forbidden_before == fixture.forbidden_bytes, "property setup changed forbidden source bytes");

  poisonTimingScatterFixture(fixture, seed);
  requireTimingScatter(readNativeTimingOracle(fixture.database()) != fixture.expected_oracle, "property poison did not change native timing");
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database()) == forbidden_before, "property poison changed forbidden source bytes");

  requireSuccessfulScatter(fixture, "property poison recovery");
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database()) == forbidden_before, "property success changed forbidden source bytes");
  requireTimingScatter(canonicalReferenceInputBytes(fixture.committed_input) == input_before, "property success mutated committed input");

  requireSuccessfulScatter(fixture, "property idempotence");
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database()) == forbidden_before, "property idempotence changed forbidden source bytes");
  requireTimingScatter(canonicalReferenceInputBytes(fixture.committed_input) == input_before, "property idempotence mutated committed input");

  fixture.context.temperature = (seed & 1U) == 0U ? "-40" : "125";
  poisonTimingScatterFixture(fixture, seed ^ 0xa5a5a5a5U);
  const CsrNativeTimingOracle stale_oracle = readNativeTimingOracle(fixture.database());
  const std::vector<std::uint8_t> stale_forbidden = timingScatterForbiddenBytes(fixture.database());

  const CsrTimingResultScatterAttempt stale
      = scatterCsrTimingResult(fixture.database(), fixture.committed_input, fixture.result, fixture.context, fixture.session_token, fixture.graph_generation);
  requireScatterCode(stale, CsrScatterCode::kSnapshotInvalidated, 0, CsrInvalidationReason::kCornerChanged, "property randomized stale");
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), stale_oracle, "property randomized stale zero-write");
  requireTimingScatter(timingScatterForbiddenBytes(fixture.database()) == stale_forbidden, "property stale failure changed forbidden source bytes");
  requireTimingScatter(canonicalReferenceInputBytes(fixture.committed_input) == input_before, "property stale failure mutated committed input");
}

}  // namespace
}  // namespace ista::csr::test

int main(int argc, char* argv[])
{
  try {
    ista::csr::test::runProperty(ista::csr::test::parseSeed(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "CsrTimingScatterPropertyTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
