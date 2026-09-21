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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

[[nodiscard]] std::string_view token(NodeKind value);
[[nodiscard]] std::string_view token(ArcKind value);
[[nodiscard]] std::string_view token(PinDirection value);
[[nodiscard]] std::string_view token(AnalysisMode value);
[[nodiscard]] std::string_view token(Transition value);
[[nodiscard]] std::string_view token(PathSource value);
[[nodiscard]] std::string_view token(StartSeedAvailability value);
[[nodiscard]] std::string_view token(TimingSense value);
[[nodiscard]] std::string_view token(TableVariable value);
[[nodiscard]] std::string_view token(TimingCheckKind value);
[[nodiscard]] std::string_view token(CsrCaptureCode value);
[[nodiscard]] std::string_view token(CsrReferenceUnsupportedReason value);
[[nodiscard]] std::string_view token(CsrComputationCode value);
[[nodiscard]] std::string_view token(CsrScatterCode value);
[[nodiscard]] std::string_view token(CsrInvalidationReason value);
[[nodiscard]] std::string_view token(CsrSnapshotState value);
[[nodiscard]] std::string_view token(CapacityAction value);

[[nodiscard]] std::optional<std::string> validateNodeKey(const NodeKey& key);
[[nodiscard]] std::optional<std::string> validateArcKeyBase(const ArcKeyBase& key);

// These comparators order valid keys exactly as their canonical encoded bytes.
// Unknown enum values throw std::invalid_argument through the token functions.
[[nodiscard]] bool nodeKeyLess(const NodeKey& lhs, const NodeKey& rhs);
[[nodiscard]] bool arcKeyBaseLess(const ArcKeyBase& lhs, const ArcKeyBase& rhs);
[[nodiscard]] bool arcKeyLess(const ArcKey& lhs, const ArcKey& rhs);

// Validates every key, returns canonical-byte order, and rejects any canonical
// NodeKey collision with std::invalid_argument.
[[nodiscard]] std::vector<NodeKey> sortedUniqueNodeKeys(std::vector<NodeKey> keys);

// The returned vector preserves arcs_in_native_order. Ordinals are zero-based
// within each canonical ArcKeyBase-equivalence class and follow input order.
// Invalid bases throw std::invalid_argument before any ordinal is assigned.
[[nodiscard]] std::vector<ArcKey> assignParallelOrdinals(const std::vector<ArcKeyBase>& arcs_in_native_order);

}  // namespace ista::csr
