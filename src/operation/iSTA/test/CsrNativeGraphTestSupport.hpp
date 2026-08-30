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
#include <vector>

#include "CsrNativeGraphOracle.hpp"
#include "NativeGraphProjection.hpp"

namespace ista {
class Database;
}

namespace ista::csr::test {

struct IndependentFifoOracle
{
  CsrNativeGraphOracle oracle;
  bool has_active_cycle{false};

  friend bool operator==(const IndependentFifoOracle&, const IndependentFifoOracle&) = default;
};

// These observers intentionally do not include or call the production adapter,
// decoder, validator, record builders, or production parallel-key assignment helper.
// Raw projection and native graph observation share only the test-only semantic-key
// construction; FIFO traversal and wire encoding remain independent.
[[nodiscard]] NativeGraphProjection captureDirectNativeGraphProjection(const ista::Database& database);
[[nodiscard]] CsrNativeGraphOracle readNativeGraphOracle(const ista::Database& database);
[[nodiscard]] IndependentFifoOracle runIndependentFifoOracle(const ista::Database& database);

// Independent spellings of the Step 2 wire contract. They share only the
// Step 1 primitive encoder and semantic-key byte encoders with production.
[[nodiscard]] std::vector<std::uint8_t> canonicalIndependentNativeGraphProjectionBytes(const NativeGraphProjection& projection);
[[nodiscard]] std::vector<std::uint8_t> canonicalIndependentNativeGraphOracleBytes(const CsrNativeGraphOracle& oracle);

}  // namespace ista::csr::test
