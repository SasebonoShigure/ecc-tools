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

#include <array>
#include <cstdint>
#include <span>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferenceCanonicalEncoder.hpp"

namespace ista::csr {

// Each member is raw canonical semantic payload. makeSnapshotIdentity()
// supplies a distinct component domain before hashing it.
struct FingerprintPayloads
{
  std::vector<std::uint8_t> topology;
  std::vector<std::uint8_t> clock_constraint;
  std::vector<std::uint8_t> library;
  std::vector<std::uint8_t> corner;
  std::vector<std::uint8_t> parasitic;
  std::vector<std::uint8_t> timing_input;
};

[[nodiscard]] std::string componentFingerprint(std::string_view domain, std::span<const std::uint8_t> payload);

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> value);
[[nodiscard]] std::string sha256Hex(std::span<const std::uint8_t> value);
// Reads exactly size bytes and rejects short, long, or failed input.
[[nodiscard]] std::string sha256Hex(std::istream& input, std::uint64_t size);
[[nodiscard]] bool isLowercaseSha256(std::string_view value);
[[nodiscard]] CsrSnapshotIdentity makeSnapshotIdentity(std::uint64_t session_token, std::uint64_t graph_generation, const FingerprintPayloads& payloads);

}  // namespace ista::csr
