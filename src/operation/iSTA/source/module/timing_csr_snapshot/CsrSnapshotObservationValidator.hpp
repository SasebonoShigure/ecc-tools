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

#include <variant>
#include <vector>

#include "CsrCaptureMetadata.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrSnapshotObservation.hpp"

namespace ista::csr {

using CsrObservationResult = std::variant<CsrSnapshotObservation, std::vector<CsrContractViolation>>;
using CsrOptionalObservationResult = std::variant<CsrOptionalObservation, std::vector<CsrContractViolation>>;

[[nodiscard]] std::vector<CsrContractViolation> validateCaptureMetadata(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata);
[[nodiscard]] std::vector<CsrContractViolation> validateSnapshotObservation(const CsrSnapshotObservation& observation);

// Construction is atomic: the error alternative is always nonempty and never
// contains a partially initialized observation.
[[nodiscard]] CsrObservationResult makeReadySnapshotObservation(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata, double export_ms);

}  // namespace ista::csr
