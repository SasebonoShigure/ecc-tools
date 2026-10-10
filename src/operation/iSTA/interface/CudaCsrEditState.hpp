// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "DualCsrStructuredBatchPreparer.hpp"
#include "DualCsrUpdaterInternal.hpp"

namespace ista {
namespace timing_dual_csr { class VerifiedStructuredBatch; }

// Functional host topology for CUDA edits; timing results belong to the device.
class CudaCsrEditState final
{
 public:
  CudaCsrEditState(const CudaCsrEditState&) = delete;
  CudaCsrEditState& operator=(const CudaCsrEditState&) = delete;
  [[nodiscard]] timing_dual_csr::PendingStructuredBatch prepareBatch(timing_edit::EditBatch edits);

  class Patch final
  {
   public:
    Patch(CudaCsrEditState& owner, timing_dual_csr::VerifiedStructuredBatch batch);
    Patch(const Patch&) = delete;
    Patch& operator=(const Patch&) = delete;
    void apply();
    void commit() noexcept;

   private:
    CudaCsrEditState& _owner;
    timing_edit::PreparedTimingEditResolverPatch _resolver;
    timing_dual_csr::detail::PreparedDualCsrGraphPatch _graph;
    timing_dual_csr::detail::PreparedDualCsrIndexPatch _index;
    std::optional<timing_edit::CommittedEditIdentity> _identity;
  };

  timing_dual_csr::DualCsrState graph;
  timing_edit::TimingEditResolverState resolver;
  timing_dual_csr::DualCsrArcGroupIndex index;
  std::shared_ptr<const timing_edit::BaselineSemanticIdentity> baseline;
  std::optional<timing_edit::CommittedEditIdentity> identity;

 private:
  friend class StaCudaSessionState;
  friend class PhysicalStaSessionState;
  CudaCsrEditState(Database& database, const csr::CsrReferenceInput& input,
                   timing_edit::TimingEditSessionToken session);
};
}  // namespace ista
