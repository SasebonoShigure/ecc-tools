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
#pragma once

#include <cstdint>
#include <memory>

namespace ista {
class Database;
}

namespace ista::csr::test {

enum class NativeGraphFixtureCase : std::uint8_t
{
  kEmpty,
  kSingleton,
  kChain,
  kDiamond,
  kMultiRoot,
  kParallel,
  kFlags,
  kSparseAdjacency,
  kUnboundFallback,
  kMixedFallback,
  kBoundSignature,
  kActiveCycle,
  kUtf8
};

class CsrNativeGraphFixture
{
 public:
  CsrNativeGraphFixture();
  ~CsrNativeGraphFixture();
  CsrNativeGraphFixture(CsrNativeGraphFixture&&) noexcept;
  CsrNativeGraphFixture& operator=(CsrNativeGraphFixture&&) noexcept;

  CsrNativeGraphFixture(const CsrNativeGraphFixture&) = delete;
  CsrNativeGraphFixture& operator=(const CsrNativeGraphFixture&) = delete;

  [[nodiscard]] ista::Database& database();
  [[nodiscard]] const ista::Database& database() const;

 private:
  std::unique_ptr<ista::Database> _database;
};

[[nodiscard]] CsrNativeGraphFixture makeNativeGraphFixture(NativeGraphFixtureCase fixture_case);
[[nodiscard]] CsrNativeGraphFixture makePropertyGraphFixture(std::uint64_t seed);

}  // namespace ista::csr::test
