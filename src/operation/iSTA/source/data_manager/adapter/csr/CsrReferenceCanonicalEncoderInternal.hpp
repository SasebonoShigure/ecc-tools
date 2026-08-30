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

#include "CsrReferenceCanonicalEncoder.hpp"

namespace ista::csr::detail {

void appendLibraryArcSignatureFields(CanonicalEncoder& encoder, const LibraryArcSignature& signature);

void appendOptionalNodeIdFields(CanonicalEncoder& encoder, const std::optional<NodeId>& node);

void appendOptionalLaneMatrixFields(CanonicalEncoder& encoder, const OptionalLaneMatrix& matrix);

void appendTimingTableModelFields(CanonicalEncoder& encoder, const TimingTableModel& table);

void appendChildTimingArcModelFields(CanonicalEncoder& encoder, const ChildTimingArcModel& child);

void appendCellTransferModelFields(CanonicalEncoder& encoder, const CellTransferModel& transfer);

void appendReferencedLibraryCellModelFields(CanonicalEncoder& encoder, const ReferencedLibraryCellModel& cell);

void appendParasiticModelFields(CanonicalEncoder& encoder, const ParasiticModel& parasitics);

}  // namespace ista::csr::detail
