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
#include <memory>
#include <variant>

#include "CsrCaptureMetadata.hpp"
#include "CsrSnapshotObservation.hpp"
#include "TimingResultScatter.hpp"
#include "TimingSemanticKey.hpp"

namespace ista {
class Database;
}

namespace ista::csr {

struct CsrCommittedSessionSnapshot
{
  CsrReferenceInput input;
  CsrCaptureMetadata metadata;
  std::unique_ptr<CsrSnapshotObservation> active_observation;
  std::unique_ptr<CsrSnapshotObservation> released_observation;
};

using CsrSessionSnapshotBuildResult = std::variant<std::unique_ptr<CsrCommittedSessionSnapshot>, CsrCaptureCode>;

[[nodiscard]] CsrSessionSnapshotBuildResult buildCsrSessionSnapshot(const Database& database, const StaAnalysisContext& context, std::uint64_t session_token);

[[nodiscard]] CsrComputationResult computeCsrSessionReference(CsrCommittedSessionSnapshot& snapshot);

[[nodiscard]] CsrTimingResultScatterAttempt scatterCsrSessionTimingResult(Database& database, CsrCommittedSessionSnapshot& snapshot,
                                                                          const CsrTimingResult& result, const StaAnalysisContext& current_context,
                                                                          std::uint64_t active_session_token);

void releaseCsrSessionSnapshot(CsrCommittedSessionSnapshot& snapshot) noexcept;

}  // namespace ista::csr
