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

#include <vector>

#include "CsrReferenceValidator.hpp"

namespace ista::csr::detail {

void validateTransferScalars(const CellTransferModel& transfer, std::vector<CsrContractViolation>& errors, const std::string& field);
void validateTimingChildren(const std::vector<ChildTimingArcModel>& children, std::vector<CsrContractViolation>& errors, const std::string& field);
[[nodiscard]] bool hasUniqueChildArcIndices(const std::vector<ChildTimingArcModel>& children);

// Intrinsic checks for one external transfer, without rebuilding a full input.
void validateTransfer(const CellTransferModel& transfer, std::vector<CsrContractViolation>& errors, const std::string& field);

void validateLibraryCell(const ReferencedLibraryCellModel& cell, std::vector<CsrContractViolation>& errors);

void validateParasiticNet(const ParasiticNetModel& net, std::vector<CsrContractViolation>& errors);

void validateReferenceNativeModels(const CsrReferenceInput& input, std::vector<CsrContractViolation>& errors);

[[nodiscard]] std::vector<CsrContractViolation> validateParasiticContextCrossField(const CsrReferenceInput& input);

}  // namespace ista::csr::detail
