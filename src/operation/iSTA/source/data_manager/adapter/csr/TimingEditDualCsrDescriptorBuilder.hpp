// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <utility>

#include "DualCsrDescriptorValidation.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "NarrowTimingRecordReader.hpp"

namespace ista::timing_dual_csr {

struct CommittedDualCsrAnalysisState;
struct DualCsrStageAttempt;
class VerifiedStructuredBatch;
[[nodiscard]] DualCsrStageAttempt makeStagedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState&, VerifiedStructuredBatch&&);

class VerifiedStructuredBatch final
{
 public:
  VerifiedStructuredBatch(const VerifiedStructuredBatch&) = delete;
  VerifiedStructuredBatch& operator=(const VerifiedStructuredBatch&) = delete;
  VerifiedStructuredBatch(VerifiedStructuredBatch&&) noexcept = default;
  VerifiedStructuredBatch& operator=(VerifiedStructuredBatch&&) noexcept
      = default;

  [[nodiscard]] const timing_edit::PreparedBatch& preparedBatch()
      const noexcept
  {
    return _prepared_batch;
  }
  [[nodiscard]] const timing_edit::TimingEditResolverState& stagedResolver()
      const noexcept
  {
    return _staged_resolver;
  }
  [[nodiscard]] const DualCsrBatchDescriptor& descriptor() const noexcept
  {
    return _descriptor;
  }
  [[nodiscard]] const ExpectedBeforeBinding& expectedBefore()
      const noexcept
  {
    return _expected_before;
  }
  [[nodiscard]] const NarrowCaptureAudit& captureAudit() const noexcept
  {
    return _capture_audit;
  }

 private:
  VerifiedStructuredBatch(
      timing_edit::PreparedBatch prepared_batch,
      timing_edit::TimingEditResolverState staged_resolver,
      DualCsrBatchDescriptor descriptor,
      ExpectedBeforeBinding expected_before,
      NarrowCaptureAudit capture_audit)
      : _prepared_batch(std::move(prepared_batch)),
        _staged_resolver(std::move(staged_resolver)),
        _descriptor(std::move(descriptor)),
        _expected_before(std::move(expected_before)),
        _capture_audit(std::move(capture_audit))
  {
  }

  timing_edit::PreparedBatch _prepared_batch;
  timing_edit::TimingEditResolverState _staged_resolver;
  DualCsrBatchDescriptor _descriptor;
  ExpectedBeforeBinding _expected_before;
  NarrowCaptureAudit _capture_audit;

  friend class TimingEditDualCsrDescriptorBuilder;
  friend DualCsrStageAttempt makeStagedDualCsrAnalysisState(
      const CommittedDualCsrAnalysisState&,
      VerifiedStructuredBatch&&);
};

class TimingEditDualCsrDescriptorBuilder final
{
 public:
  // CandidateBoundCapture is reader-minted and move-only. The builder still
  // rechecks its exact plan, resolver IDs, typed records, field policy, audit,
  // and descriptor validators before privately minting the next capability.
  [[nodiscard]] static VerifiedStructuredBatch build(
      PendingStructuredBatch pending,
      CandidateBoundCapture&& capture);
};

}  // namespace ista::timing_dual_csr
