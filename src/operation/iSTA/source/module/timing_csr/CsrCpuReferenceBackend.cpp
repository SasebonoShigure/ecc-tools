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
#include "CsrCpuReferenceBackend.hpp"

#include <cassert>
#include <utility>

#include "CsrCpuReferenceBackendInternal.hpp"
#include "CsrReferenceValidator.hpp"

namespace ista::csr {
namespace {

[[nodiscard]] CsrComputationResult stageFailure(CsrCpuReferenceStageStatus status)
{
  assert(status != CsrCpuReferenceStageStatus::kSuccess);
  if (status == CsrCpuReferenceStageStatus::kNumericFailure)
    return CsrComputationResult{CsrComputationCode::kNumericFailure};
  return CsrComputationResult{CsrComputationCode::kInvariantViolation};
}

}  // namespace

CsrComputationResult computeCsrCpuReferenceWithReplay(const CsrReferenceInput& input, CsrFifoReplayFunction replay)
{
  assert(replay != nullptr);

  if (!validateReferenceInput(input).empty())
    return CsrComputationResult{CsrComputationCode::kInvariantViolation};

  if (!input.unsupported_reasons.empty())
    return CsrComputationResult{CsrReferenceSupportResult{input.unsupported_reasons}};

  CsrFifoLevelization fifo = replay(input);
  if (!fifo.complete)
    return CsrComputationResult{CsrComputationCode::kCycle};

  CsrCpuReferenceWorkState work;
  work.result.schema_version = input.schema_version;
  work.result.source_identity = input.source_identity;

  CsrCpuReferenceStageStatus status = initializeCsrCpuReferenceResult(input, fifo, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return stageFailure(status);

  status = initializeCsrCpuReferenceArcs(input, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return stageFailure(status);

  status = propagateCsrCpuReferenceForward(input, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return stageFailure(status);

  status = propagateCsrCpuReferenceRequired(input, work);
  if (status != CsrCpuReferenceStageStatus::kSuccess)
    return stageFailure(status);

  return CsrComputationResult{std::move(work.result)};
}

CsrComputationResult CsrCpuReferenceBackend::compute(const CsrReferenceInput& input) const
{
  return computeCsrCpuReferenceWithReplay(input, &replayFifoLevelization);
}

}  // namespace ista::csr
