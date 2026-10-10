// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <span>
#include <utility>

#include "DualCsrDescriptorValidation.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "NarrowTimingRecordReader.hpp"

namespace ista::csr::detail { class NativeLibraryModels; }
namespace ista::timing_edit { class NativeExecutionAuthority; }

namespace ista::timing_dual_csr {
namespace detail { struct NarrowTimingRecords; }

struct CommittedDualCsrAnalysisState;
struct DualCsrStageAttempt;
class DualCsrStageFactoryAccess;
class VerifiedStructuredBatch;
namespace test {
class DualCsrStageFactoryFaultAdapter;
}

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
  [[nodiscard]] const timing_edit::TimingEditResolverDelta& resolverDelta() const noexcept
  {
    return _resolver_delta;
  }
  [[nodiscard]] const DualCsrBatchDescriptor& descriptor() const noexcept
  {
    return _descriptor;
  }
  [[nodiscard]] const std::optional<NarrowCaptureAudit>& captureAudit() const noexcept
  {
    return _capture_audit;
  }

 private:
  VerifiedStructuredBatch(
      const DualCsrState* source_graph,
      timing_edit::PreparedBatch prepared_batch,
      timing_edit::TimingEditResolverDelta resolver_delta,
      DualCsrBatchDescriptor descriptor,
      std::optional<NarrowCaptureAudit> capture_audit)
      : _source_graph(source_graph), _prepared_batch(std::move(prepared_batch)),
        _resolver_delta(std::move(resolver_delta)),
        _descriptor(std::move(descriptor)),
        _capture_audit(std::move(capture_audit))
  {
  }

  const DualCsrState* _source_graph;
  timing_edit::PreparedBatch _prepared_batch;
  timing_edit::TimingEditResolverDelta _resolver_delta;
  DualCsrBatchDescriptor _descriptor;
  std::optional<NarrowCaptureAudit> _capture_audit;

  friend class TimingEditDualCsrDescriptorBuilder;
  friend class DualCsrStageFactoryAccess;
  friend class ista::CudaCsrEditState;
  friend class test::DualCsrStageFactoryFaultAdapter;
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

  // Generates the local edit from committed CSR and immutable Liberty only.
  // RC records are prepared physical inputs, never native timing answers.
  [[nodiscard]] static VerifiedStructuredBatch buildDirect(
      PendingStructuredBatch pending, const timing_edit::NativeExecutionAuthority& authority,
      csr::detail::NativeLibraryModels& models,
      std::span<const csr::ParasiticNetModel> affected_rc = {});

 private:
  [[nodiscard]] static VerifiedStructuredBatch finish(
      PendingStructuredBatch pending, detail::NarrowTimingRecords records,
      std::optional<NarrowCaptureAudit> capture_audit);
};

}  // namespace ista::timing_dual_csr
