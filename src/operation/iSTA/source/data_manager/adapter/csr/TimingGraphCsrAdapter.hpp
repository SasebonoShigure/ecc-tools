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
#include <variant>

#include "CsrGraphTypes.hpp"
#include "NativeGraphProjection.hpp"

namespace ista {
class Database;
}

namespace ista::csr {

[[nodiscard]] CsrGraphExportResult exportCsrGraphSnapshot(const ista::Database& database);
[[nodiscard]] NativeGraphProjectionResult decodeNativeGraphProjection(const CsrGraphSnapshot& snapshot);

enum class CsrReferenceInputExportPhase : std::uint8_t
{
  kAnalysisContextPreflight,
  kTopology,
  kClockConstraint,
  kLibrary,
  kCorner,
  kParasitic,
  kTimingInput
};

struct CsrReferenceInputExportFailure
{
  CsrCaptureCode code{CsrCaptureCode::kMalformedTopology};
  CsrReferenceInputExportPhase failed_phase{CsrReferenceInputExportPhase::kTopology};
  CsrCurrentFingerprints current_fingerprints;

  friend bool operator==(const CsrReferenceInputExportFailure&, const CsrReferenceInputExportFailure&) = default;
};

using CsrReferenceInputExportResult = std::variant<CsrReferenceInput, CsrReferenceInputExportFailure>;

[[nodiscard]] CsrReferenceInputExportResult exportCsrReferenceInputCandidate(const ista::Database& database, const StaAnalysisContext& canonical_context,
                                                                             std::uint64_t caller_session_token);

}  // namespace ista::csr
