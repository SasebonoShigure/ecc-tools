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
#include <span>
#include <string_view>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

class CanonicalEncoder
{
 public:
  void appendDomain(std::string_view domain, std::string_view version = kCsrSchemaVersion);
  void appendBool(bool value);
  void appendU8(std::uint8_t value);
  void appendU32(std::uint32_t value);
  void appendI32(std::int32_t value);
  void appendU64(std::uint64_t value);
  void appendString(std::string_view value);
  void appendBytes(std::span<const std::uint8_t> value);
  void appendDouble(double value);
  void appendOptionalDouble(const std::optional<double>& value);

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return _bytes; }

 private:
  std::vector<std::uint8_t> _bytes;
};

[[nodiscard]] std::vector<std::uint8_t> canonicalNodeKeyBytes(const NodeKey& key);
[[nodiscard]] std::vector<std::uint8_t> canonicalArcKeyBytes(const ArcKey& key);
[[nodiscard]] std::vector<std::uint8_t> canonicalLibraryArcSignatureBytes(const LibraryArcSignature& signature);
[[nodiscard]] std::vector<std::uint8_t> canonicalAnalysisContextBytes(const StaAnalysisContext& context);

// Encodes logical/content fields only. Process-local source_identity and vector
// capacity are excluded; canonicalSnapshotIdentityBytes() encodes the immutable
// envelope identity separately.
[[nodiscard]] std::vector<std::uint8_t> canonicalReferenceInputBytes(const CsrReferenceInput& input);
[[nodiscard]] std::vector<std::uint8_t> canonicalSnapshotIdentityBytes(const CsrSnapshotIdentity& identity);

}  // namespace ista::csr
