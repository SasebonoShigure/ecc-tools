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
#include <cstdint>

#include "CsrValidatorTestFixture.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

void testCaptureAndSupport()
{
  requireValid(validateCaptureResult(CsrCaptureResult{}), "capture success");
  requireValid(validateCaptureResult(CsrCaptureResult{CsrCaptureCode::kMalformedTopology}), "typed capture failure");
  requireInvalid(validateCaptureResult(CsrCaptureResult{static_cast<CsrCaptureCode>(0xff)}), "unknown capture code");

  requireValid(validateReferenceSupportResult(CsrReferenceSupportResult{}), "supported reference");
  CsrReferenceSupportResult supported{{CsrReferenceUnsupportedReason::kParasiticNet, CsrReferenceUnsupportedReason::kSequentialInstance}};
  requireValid(validateReferenceSupportResult(supported), "ordered unsupported reasons");
  supported.unsupported_reasons = {CsrReferenceUnsupportedReason::kSequentialInstance, CsrReferenceUnsupportedReason::kParasiticNet};
  requireInvalid(validateReferenceSupportResult(supported), "unordered unsupported reasons");
  supported.unsupported_reasons = {CsrReferenceUnsupportedReason::kParasiticNet, CsrReferenceUnsupportedReason::kParasiticNet};
  requireInvalid(validateReferenceSupportResult(supported), "duplicate unsupported reasons");
  supported.unsupported_reasons = {static_cast<CsrReferenceUnsupportedReason>(0xff)};
  requireInvalid(validateReferenceSupportResult(supported), "unknown unsupported reason");
}

void testComputationOutcome()
{
  const auto accept = [](const CsrReferenceInput& input, const auto& outcome, const std::string& name) {
    CsrComputationResult result;
    result.outcome = outcome;
    requireValid(validateComputationResult(input, result), name);
  };
  const auto reject = [](const CsrReferenceInput& input, const auto& outcome, const std::string& name) {
    CsrComputationResult result;
    result.outcome = outcome;
    requireInvalid(validateComputationResult(input, result), name);
  };

  const CsrReferenceInput complete = referenceInput();
  accept(complete, timingResult(complete), "complete timing result");
  accept(complete, CsrComputationCode::kInvariantViolation, "complete invariant failure");
  accept(complete, CsrComputationCode::kNumericFailure, "complete numeric failure");
  reject(complete, CsrComputationCode::kCycle, "complete cycle code");
  reject(complete, CsrReferenceSupportResult{}, "complete support result");
  reject(complete, static_cast<CsrComputationCode>(0xff), "complete unknown code");

  const CsrReferenceInput cycle = cycleInput();
  requireValid(validateReferenceInput(cycle), "active-cycle reference input");
  accept(cycle, CsrComputationCode::kCycle, "active cycle code");
  reject(cycle, CsrComputationCode::kInvariantViolation, "cycle invariant code");
  reject(cycle, CsrComputationCode::kNumericFailure, "cycle numeric code");
  reject(cycle, CsrTimingResult{}, "cycle timing result");
  reject(cycle, CsrReferenceSupportResult{}, "cycle support result");
  reject(cycle, static_cast<CsrComputationCode>(0xff), "cycle unknown code");

  const CsrReferenceInput unsupported = clockInput();
  requireValid(validateReferenceInput(unsupported), "unsupported reference input");
  accept(unsupported, CsrReferenceSupportResult{unsupported.unsupported_reasons}, "exact support result");
  reject(unsupported, CsrReferenceSupportResult{}, "empty support result");
  reject(unsupported, CsrReferenceSupportResult{{CsrReferenceUnsupportedReason::kParasiticNet}}, "wrong support result");
  reject(unsupported, CsrTimingResult{}, "unsupported timing result");
  reject(unsupported, CsrComputationCode::kCycle, "unsupported cycle code");
  reject(unsupported, CsrComputationCode::kInvariantViolation, "unsupported invariant code");
  reject(unsupported, CsrComputationCode::kNumericFailure, "unsupported numeric code");
  reject(unsupported, static_cast<CsrComputationCode>(0xff), "unsupported unknown code");

  CsrReferenceInput malformed = cycle;
  malformed.schema_version = "malformed";
  malformed.unsupported_reasons = {CsrReferenceUnsupportedReason::kParasiticNet};
  requireInvalid(validateReferenceInput(malformed), "malformed unsupported cycle input");
  accept(malformed, CsrComputationCode::kInvariantViolation, "malformed invariant code");
  reject(malformed, CsrComputationCode::kCycle, "malformed cycle code");
  reject(malformed, CsrComputationCode::kNumericFailure, "malformed numeric code");
  reject(malformed, CsrTimingResult{}, "malformed timing result");
  reject(malformed, CsrReferenceSupportResult{malformed.unsupported_reasons}, "malformed support result");
  reject(malformed, static_cast<CsrComputationCode>(0xff), "malformed unknown code");
}

void testScatterOutcome()
{
  requireValid(validateScatterResult(CsrScatterResult{CsrScatterCode::kSuccess, 7, std::nullopt}), "scatter success writes");
  requireValid(validateScatterResult(CsrScatterResult{CsrScatterCode::kInvalidState, 0, std::nullopt}), "scatter typed failure");
  requireValid(validateScatterResult(CsrScatterResult{CsrScatterCode::kSnapshotInvalidated, 0, CsrInvalidationReason::kTopologyChanged}),
               "scatter invalidation");
  requireInvalid(validateScatterResult(CsrScatterResult{CsrScatterCode::kInvalidState, 1, std::nullopt}), "failed scatter writes");
  requireInvalid(validateScatterResult(CsrScatterResult{CsrScatterCode::kSnapshotInvalidated, 0, std::nullopt}), "missing invalidation reason");
  requireInvalid(validateScatterResult(CsrScatterResult{CsrScatterCode::kSuccess, 0, CsrInvalidationReason::kLibraryChanged}), "spurious invalidation reason");
  requireInvalid(validateScatterResult(CsrScatterResult{static_cast<CsrScatterCode>(0xff), 0, std::nullopt}), "unknown scatter code");
  requireInvalid(validateScatterResult(CsrScatterResult{CsrScatterCode::kSnapshotInvalidated, 0, static_cast<CsrInvalidationReason>(0xff)}),
                 "unknown invalidation reason");
}

}  // namespace

int main()
{
  testCaptureAndSupport();
  testComputationOutcome();
  testScatterOutcome();
  return 0;
}
