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
#include <string>

#include "CsrFingerprint.hpp"

namespace ista::csr {

[[nodiscard]] std::string makeReferenceTopologyFingerprint(const CsrReferenceInput& input);
[[nodiscard]] std::string makeReferenceClockConstraintFingerprint(const CsrReferenceInput& input);
[[nodiscard]] std::string makeReferenceLibraryFingerprint(const CsrReferenceInput& input);
[[nodiscard]] std::string makeReferenceCornerFingerprint(const CsrReferenceInput& input);
[[nodiscard]] std::string makeReferenceParasiticFingerprint(const CsrReferenceInput& input);
[[nodiscard]] std::string makeReferenceTimingInputFingerprint(const CsrReferenceInput& input);

[[nodiscard]] FingerprintPayloads buildReferenceFingerprintPayloads(const CsrReferenceInput& input);

[[nodiscard]] CsrSnapshotIdentity makeReferenceInputIdentity(const CsrReferenceInput& input, std::uint64_t session_token, std::uint64_t graph_generation);

}  // namespace ista::csr
