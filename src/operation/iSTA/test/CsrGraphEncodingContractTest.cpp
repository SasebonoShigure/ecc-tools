// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "CsrGraphEncoding.hpp"
#include "CsrNativeGraphFixture.hpp"
#include "CsrNativeGraphTestSupport.hpp"
#include "CsrNativeGraphWireDecoder.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::uint8_t hexNibble(char value)
{
  if (value >= '0' && value <= '9')
    return static_cast<std::uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f')
    return static_cast<std::uint8_t>(value - 'a' + 10);
  throw std::runtime_error("known-answer hex contains a non-lowercase-hex character");
}

std::vector<std::uint8_t> fromHex(std::string_view hex)
{
  require(hex.size() % 2 == 0, "known-answer hex length is odd");
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    bytes.push_back(static_cast<std::uint8_t>((hexNibble(hex[index]) << 4) | hexNibble(hex[index + 1])));
  }
  return bytes;
}

template <typename Callable>
void requireDecodeFailure(Callable callable, const std::string& message)
{
  try {
    callable();
  } catch (const std::runtime_error&) {
    return;
  }
  throw std::runtime_error(message);
}

void verifyEmptyKnownAnswers()
{
  constexpr std::string_view kProjectionHex
      = "0000000000000008697374612d63737200000000000000176e61746976652d67726170682d70726f6a656374696f6e00"
        "00000000000005312e302e300000000000000026697374612d6373722d6e61746976652d67726170682d70726f6a6563"
        "74696f6e2f312e302e300000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000";
  constexpr std::string_view kOracleHex
      = "0000000000000008697374612d63737200000000000000136e61746976652d67726170682d6f7261636c650000000000"
        "000005312e302e300000000000000022697374612d6373722d6e61746976652d67726170682d6f7261636c652f312e30"
        "2e3000000000000000000000000000000000";
  const auto expected_projection = fromHex(kProjectionHex);
  const auto expected_oracle = fromHex(kOracleHex);
  require(expected_projection.size() == 170, "projection known-answer size is wrong");
  require(expected_oracle.size() == 114, "oracle known-answer size is wrong");

  const NativeGraphProjection projection;
  const CsrNativeGraphOracle oracle;
  require(canonicalNativeGraphProjectionBytes(projection) == expected_projection, "production empty projection bytes changed");
  require(canonicalIndependentNativeGraphProjectionBytes(projection) == expected_projection, "independent empty projection bytes changed");
  require(canonicalNativeGraphOracleBytes(oracle) == expected_oracle, "production empty oracle bytes changed");
  require(canonicalIndependentNativeGraphOracleBytes(oracle) == expected_oracle, "independent empty oracle bytes changed");
  require(decodeIndependentNativeGraphProjectionBytes(expected_projection) == projection, "empty projection known-answer did not decode");
  require(decodeIndependentNativeGraphOracleBytes(expected_oracle) == oracle, "empty oracle known-answer did not decode");
}

void verifyRichRoundTrip()
{
  auto fixture = makeNativeGraphFixture(NativeGraphFixtureCase::kBoundSignature);
  const NativeGraphProjection projection = captureDirectNativeGraphProjection(fixture.database());
  const auto production_bytes = canonicalNativeGraphProjectionBytes(projection);
  const auto independent_bytes = canonicalIndependentNativeGraphProjectionBytes(projection);
  require(production_bytes == independent_bytes, "rich projection encoders disagree");
  require(decodeIndependentNativeGraphProjectionBytes(production_bytes) == projection, "rich projection encode/decode round-trip failed");

  const CsrNativeGraphOracle native_oracle = readNativeGraphOracle(fixture.database());
  const auto production_oracle = canonicalNativeGraphOracleBytes(native_oracle);
  const auto independent_oracle = canonicalIndependentNativeGraphOracleBytes(native_oracle);
  require(production_oracle == independent_oracle, "rich oracle encoders disagree");
  require(decodeIndependentNativeGraphOracleBytes(production_oracle) == native_oracle, "rich oracle encode/decode round-trip failed");

  CsrNativeGraphOracle signed_oracle;
  signed_oracle.native_node_levels = {CsrNativeNodeLevel{NodeKey{NodeKind::kPort, "negative", {}, {}}, -7}};
  const auto signed_bytes = canonicalNativeGraphOracleBytes(signed_oracle);
  require(decodeIndependentNativeGraphOracleBytes(signed_bytes) == signed_oracle, "signed I32 oracle level did not round-trip");

  auto trailing = production_bytes;
  trailing.push_back(0);
  requireDecodeFailure([&] { static_cast<void>(decodeIndependentNativeGraphProjectionBytes(trailing)); }, "projection decoder accepted trailing bytes");
  auto truncated = production_oracle;
  truncated.pop_back();
  requireDecodeFailure([&] { static_cast<void>(decodeIndependentNativeGraphOracleBytes(truncated)); }, "oracle decoder accepted truncated bytes");
}

}  // namespace

int main()
{
  try {
    verifyEmptyKnownAnswers();
    verifyRichRoundTrip();
  } catch (const std::exception& error) {
    std::cerr << "CsrGraphEncodingContractTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
