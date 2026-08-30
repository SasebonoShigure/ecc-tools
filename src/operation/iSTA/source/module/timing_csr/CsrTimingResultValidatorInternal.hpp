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

#include "CsrReferenceValidatorDetail.hpp"

namespace ista::csr::detail {

[[nodiscard]] TimingResultViolations validateSnapshotIdentityClassified(const CsrSnapshotIdentity& identity, std::string_view prefix,
                                                                        bool require_stage2_generation);

// Precondition: input has passed `validateReferenceInputForSession` or
// validateReferenceInput(), and input.unsupported_reasons is empty.
[[nodiscard]] TimingResultViolations validateTimingResultClassified(const CsrReferenceInput& input, const CsrTimingResult& result);

void validateArcResultSoa(const CsrReferenceInput& input, const ArcResultSoa& result, const OptionalSoa<double>& data_slews, TimingResultViolations& errors);

}  // namespace ista::csr::detail
