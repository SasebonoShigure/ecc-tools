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
#include "CsrReferenceValidator.hpp"

#include <algorithm>

#include "CsrFifoLevelization.hpp"
#include "CsrReferenceValidatorDetail.hpp"

namespace ista::csr {

using namespace detail;

std::optional<CsrInvalidationReason> firstCsrInvalidationReason(const CsrSnapshotIdentity& committed, const CsrCurrentFingerprints& current) noexcept
{
  if (!current.topology_fingerprint || *current.topology_fingerprint != committed.topology_fingerprint)
    return CsrInvalidationReason::kTopologyChanged;
  if (!current.clock_constraint_fingerprint || *current.clock_constraint_fingerprint != committed.clock_constraint_fingerprint)
    return CsrInvalidationReason::kClockConstraintChanged;
  if (!current.library_fingerprint || *current.library_fingerprint != committed.library_fingerprint)
    return CsrInvalidationReason::kLibraryChanged;
  if (!current.corner_fingerprint || *current.corner_fingerprint != committed.corner_fingerprint)
    return CsrInvalidationReason::kCornerChanged;
  if (!current.parasitic_fingerprint || *current.parasitic_fingerprint != committed.parasitic_fingerprint)
    return CsrInvalidationReason::kParasiticChanged;
  if (!current.timing_input_fingerprint || *current.timing_input_fingerprint != committed.timing_input_fingerprint)
    return CsrInvalidationReason::kTimingInputChanged;
  return std::nullopt;
}

std::vector<CsrContractViolation> validateCaptureResult(const CsrCaptureResult& result)
{
  Violations errors;
  if (result.error && !knownEnum(*result.error))
    reject(errors, "capture.error", "capture code enum is unknown");
  return errors;
}

std::vector<CsrContractViolation> validateReferenceSupportResult(const CsrReferenceSupportResult& result)
{
  Violations errors;
  bool known = true;
  for (auto reason : result.unsupported_reasons)
    if (!knownEnum(reason)) {
      reject(errors, "support.unsupported_reasons", "unsupported reason enum is unknown");
      known = false;
    }
  if (known && (!std::ranges::is_sorted(result.unsupported_reasons, {}, [](auto value) {
        return static_cast<std::uint8_t>(value);
      }) || std::ranges::adjacent_find(result.unsupported_reasons) != result.unsupported_reasons.end()))
    reject(errors, "support.unsupported_reasons", "reasons must follow stable unique enum order");
  return errors;
}

std::vector<CsrContractViolation> validateComputationResult(const CsrReferenceInput& input, const CsrComputationResult& result)
{
  Violations errors;
  if (!validateReferenceInput(input).empty()) {
    const auto* code = std::get_if<CsrComputationCode>(&result.outcome);
    if (code == nullptr || *code != CsrComputationCode::kInvariantViolation)
      reject(errors, "computation.outcome", "malformed reference input requires invariant_violation");
    return errors;
  }

  if (result.outcome.valueless_by_exception()) {
    reject(errors, "computation.outcome", "variant is valueless");
    return errors;
  }

  const auto* timing = std::get_if<CsrTimingResult>(&result.outcome);
  const auto* support = std::get_if<CsrReferenceSupportResult>(&result.outcome);
  const auto* code = std::get_if<CsrComputationCode>(&result.outcome);

  if (!input.unsupported_reasons.empty()) {
    if (support == nullptr) {
      reject(errors, "computation.outcome", "unsupported reference input requires an exact support result");
    } else {
      appendViolations(errors, validateReferenceSupportResult(*support));
      if (support->unsupported_reasons != input.unsupported_reasons)
        reject(errors, "computation.outcome", "support reasons must exactly equal the reference input reasons");
    }
    return errors;
  }

  const bool fifo_complete = replayFifoLevelization(input).complete;
  if (!fifo_complete) {
    if (code == nullptr || *code != CsrComputationCode::kCycle)
      reject(errors, "computation.outcome", "cycle code must exactly match FIFO levelization completeness");
    return errors;
  }

  if (timing != nullptr) {
    appendViolations(errors, validateTimingResult(input, *timing));
  } else if (support != nullptr) {
    reject(errors, "computation.outcome", "supported reference input must not return a support result");
  } else if (!knownEnum(*code)) {
    reject(errors, "computation.outcome", "computation code enum is unknown");
  } else if (*code == CsrComputationCode::kCycle) {
    reject(errors, "computation.outcome", "cycle code must exactly match FIFO levelization completeness");
  }
  return errors;
}

std::vector<CsrContractViolation> validateScatterResult(const CsrScatterResult& result)
{
  Violations errors;
  const bool code_known = knownEnum(result.code);
  if (!code_known)
    reject(errors, "scatter.code", "scatter code enum is unknown");
  if ((!code_known || result.code != CsrScatterCode::kSuccess) && result.writes_applied != 0)
    reject(errors, "scatter.writes_applied", "failed scatter must apply zero writes");
  const bool requires_reason = code_known && result.code == CsrScatterCode::kSnapshotInvalidated;
  if (result.invalidation_reason.has_value() != requires_reason)
    reject(errors, "scatter.invalidation_reason", "reason presence must exactly match snapshot_invalidated");
  if (result.invalidation_reason && !knownEnum(*result.invalidation_reason))
    reject(errors, "scatter.invalidation_reason", "invalidation reason enum is unknown");
  return errors;
}

}  // namespace ista::csr
