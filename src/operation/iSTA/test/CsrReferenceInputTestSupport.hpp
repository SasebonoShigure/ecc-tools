// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista {
class Database;
}

namespace ista::csr::test {

enum class ReferenceNativeFixtureCase : std::uint8_t
{
  kEmpty,
  kChain,
  kDiamond,
  kSingleton,
  kParallel,
  kLoopDisabled,
  kDisabled,
  kMalformedAdjacency
};

class CsrReferenceInputNativeFixture
{
 public:
  CsrReferenceInputNativeFixture();
  ~CsrReferenceInputNativeFixture();
  CsrReferenceInputNativeFixture(CsrReferenceInputNativeFixture&&) noexcept;
  CsrReferenceInputNativeFixture& operator=(CsrReferenceInputNativeFixture&&) noexcept;

  CsrReferenceInputNativeFixture(const CsrReferenceInputNativeFixture&) = delete;
  CsrReferenceInputNativeFixture& operator=(const CsrReferenceInputNativeFixture&) = delete;

  [[nodiscard]] ista::Database& database();
  [[nodiscard]] const ista::Database& database() const;

 private:
  std::unique_ptr<ista::Database> _database;
};

[[nodiscard]] StaAnalysisContext referenceInputTestContext();
[[nodiscard]] CsrReferenceInput makeReferenceInputTestFixture(std::uint64_t session_token = 1);
[[nodiscard]] CsrReferenceInputNativeFixture makeReferenceNativeFixture(ReferenceNativeFixtureCase fixture_case);
[[nodiscard]] CsrReferenceInputNativeFixture makeReferencePropertyFixture(std::uint64_t seed);
[[nodiscard]] std::vector<std::uint8_t> canonicalIndependentReferenceInputBytes(const CsrReferenceInput& input);

}  // namespace ista::csr::test
