// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "CsrNativeTimingOracle.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "TimingResultScatter.hpp"

namespace ista::csr::test {

struct CsrTimingScatterTestFixture
{
  CsrReferenceInputNativeFixture native;
  StaAnalysisContext context;
  std::uint64_t session_token{0};
  std::uint64_t graph_generation{0};
  CsrReferenceInput committed_input;
  CsrTimingResult result;
  CsrNativeTimingOracle expected_oracle;
  std::vector<std::uint8_t> forbidden_bytes;

  [[nodiscard]] ista::Database& database();
  [[nodiscard]] const ista::Database& database() const;
};

[[nodiscard]] CsrTimingScatterTestFixture makeTimingScatterFixture(ReferenceNativeFixtureCase fixture_case, std::uint64_t session_token = 0x6001U);
[[nodiscard]] CsrTimingScatterTestFixture makeTimingScatterPropertyFixture(std::uint64_t seed);

[[nodiscard]] CsrTimingResult materializeTimingResultFromNativeOracle(const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle);

void poisonTimingScatterFixture(CsrTimingScatterTestFixture& fixture, std::uint64_t repeat_index = 0);
void reverseTimingScatterNativeArcStorage(CsrTimingScatterTestFixture& fixture);

[[nodiscard]] std::vector<std::uint8_t> timingScatterForbiddenBytes(const ista::Database& database);
[[nodiscard]] std::vector<std::uint8_t> timingScatterForbiddenBytes(const ista::Database& database, const StaAnalysisContext& context);
[[nodiscard]] inline std::uint64_t expectedTimingScatterWrites(const CsrReferenceInput& input)
{
  constexpr std::uint64_t kNodeFamilies = 21;
  constexpr std::uint64_t kArcFamilies = 8;
  if (input.nodes.size() > (std::numeric_limits<std::uint64_t>::max() - 1) / kNodeFamilies)
    throw std::overflow_error("timing scatter fixture: node write count overflow");
  const std::uint64_t node_writes = kNodeFamilies * input.nodes.size();
  if (input.arcs.size() > (std::numeric_limits<std::uint64_t>::max() - node_writes - 1) / kArcFamilies)
    throw std::overflow_error("timing scatter fixture: Arc write count overflow");
  return node_writes + kArcFamilies * input.arcs.size() + 1;
}

void requireTimingScatter(bool condition, const std::string& message);
void requireScatterCode(const CsrTimingResultScatterAttempt& attempt, CsrScatterCode code, std::uint64_t writes_applied,
                        std::optional<CsrInvalidationReason> invalidation_reason, const std::string& name);
void requireNativeTimingOracleEqual(const CsrNativeTimingOracle& actual, const CsrNativeTimingOracle& expected, const std::string& name);

}  // namespace ista::csr::test
