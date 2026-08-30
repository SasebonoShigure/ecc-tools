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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

struct CsrContractViolation
{
  std::string field;
  std::string message;
};

// Identity field names use exactly one dot after prefix. An empty prefix emits
// bare field names; trailing dots on a nonempty prefix are ignored.
[[nodiscard]] std::vector<CsrContractViolation> validateSnapshotIdentitySyntax(const CsrSnapshotIdentity& identity, std::string_view prefix);

// Stage 2 committed identities first satisfy the complete syntax contract and
// then additionally require graph_generation == 0.
[[nodiscard]] std::vector<CsrContractViolation> validateStage2CommittedIdentity(const CsrSnapshotIdentity& identity, std::string_view prefix);

[[nodiscard]] std::optional<CsrInvalidationReason> firstCsrInvalidationReason(const CsrSnapshotIdentity& committed,
                                                                              const CsrCurrentFingerprints& current) noexcept;

[[nodiscard]] std::vector<CsrContractViolation> validateAnalysisContextIntrinsic(const StaAnalysisContext& context);
[[nodiscard]] std::vector<CsrContractViolation> validateReferenceInputIntrinsic(const CsrReferenceInput& input);
[[nodiscard]] std::vector<CsrContractViolation> validateReferenceInput(const CsrReferenceInput& input);
[[nodiscard]] std::vector<CsrContractViolation> validateReferenceInputForSession(const CsrReferenceInput& input, std::uint64_t expected_session_token);

[[nodiscard]] std::vector<CsrContractViolation> validateTimingResult(const CsrReferenceInput& input, const CsrTimingResult& result);
[[nodiscard]] std::vector<CsrContractViolation> validateCaptureResult(const CsrCaptureResult& result);
[[nodiscard]] std::vector<CsrContractViolation> validateReferenceSupportResult(const CsrReferenceSupportResult& result);
[[nodiscard]] std::vector<CsrContractViolation> validateComputationResult(const CsrReferenceInput& input, const CsrComputationResult& result);
[[nodiscard]] std::vector<CsrContractViolation> validateScatterResult(const CsrScatterResult& result);

}  // namespace ista::csr
