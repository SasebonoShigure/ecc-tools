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
#include <variant>
#include <vector>

#include "CsrCaptureMetadata.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrSnapshotObservation.hpp"

namespace ista::csr {

struct CsrSnapshotStorageReport
{
  CsrSnapshotBufferMetrics buffers;
  CsrModeledHostBytes host_bytes;

  friend bool operator==(const CsrSnapshotStorageReport&, const CsrSnapshotStorageReport&) = default;
};

using CsrCheckedStorageValue = std::variant<std::uint64_t, std::vector<CsrContractViolation>>;
using CsrSnapshotStorageResult = std::variant<CsrSnapshotStorageReport, std::vector<CsrContractViolation>>;

[[nodiscard]] CsrCheckedStorageValue checkedStorageAdd(std::uint64_t lhs, std::uint64_t rhs, std::string_view field);
[[nodiscard]] CsrCheckedStorageValue checkedStorageMultiply(std::uint64_t lhs, std::uint64_t rhs, std::string_view field);
[[nodiscard]] CsrCheckedStorageValue checkedStorageSum(std::span<const std::uint64_t> values, std::string_view field);

// This schema-v1 estimate models vector slots and string backing bytes. It is
// neither an allocator/RSS measurement nor a cross-ABI golden.
[[nodiscard]] CsrSnapshotStorageResult snapshotStorage(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata);

}  // namespace ista::csr
