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

#include "TimingGraphCsrAdapter.hpp"

namespace ista {
class Database;
}

namespace ista::csr {

struct CsrTimingResultScatterAttempt
{
  CsrScatterResult scatter_result;
  CsrCurrentFingerprints current_fingerprints;
  std::optional<CsrReferenceInputExportFailure> current_capture_error;

  friend bool operator==(const CsrTimingResultScatterAttempt&, const CsrTimingResultScatterAttempt&) = default;
};

[[nodiscard]] CsrTimingResultScatterAttempt scatterCsrTimingResult(Database& database, const CsrReferenceInput& committed_input, const CsrTimingResult& result,
                                                                   const StaAnalysisContext& current_context, std::uint64_t active_session_token,
                                                                   std::uint64_t active_graph_generation);

}  // namespace ista::csr
