// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "AffectedCapturePlan.hpp"
#include "DualCsrArcGroupIndex.hpp"
#include "DualSemanticReadView.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_dual_csr {

class TimingEditDualCsrDescriptorBuilder;
class DualCsrTransactionCoordinator;

// Borrow only during exclusive descriptor preparation. The builder consumes
// this binding before graph/index mutation or capacity growth.
class ExpectedBeforeBinding final
{
 public:
  [[nodiscard]] const DualCsrState& graph() const noexcept { return *_graph; }
  [[nodiscard]] const timing_edit::TimingEditResolverState& resolver() const noexcept { return *_resolver; }
  [[nodiscard]] const DualCsrArcGroupIndex& arcGroupIndex() const noexcept { return *_arc_group_index; }

 private:
  ExpectedBeforeBinding(const DualCsrState& graph, const timing_edit::TimingEditResolverState& resolver,
                        const DualCsrArcGroupIndex& arc_group_index) noexcept
      : _graph(&graph), _resolver(&resolver), _arc_group_index(&arc_group_index)
  {
  }

  const DualCsrState* _graph;
  const timing_edit::TimingEditResolverState* _resolver;
  const DualCsrArcGroupIndex* _arc_group_index;

  friend class DualCsrStructuredBatchPreparer;
};

class PendingStructuredBatch final
{
 public:
  PendingStructuredBatch(const PendingStructuredBatch&) = delete;
  PendingStructuredBatch& operator=(const PendingStructuredBatch&) = delete;
  PendingStructuredBatch(PendingStructuredBatch&&) noexcept = default;
  PendingStructuredBatch& operator=(PendingStructuredBatch&&) noexcept
      = default;

  [[nodiscard]] const timing_edit::PreparedBatch& preparedBatch()
      const noexcept
  {
    return _prepared_batch;
  }
  [[nodiscard]] timing_edit::TimingEditResolverView stagedResolver()
      const noexcept
  {
    return {_expected_before.resolver(), _resolver_delta};
  }
  [[nodiscard]] const AffectedCapturePlan& capturePlan() const noexcept
  {
    return _capture_plan;
  }
  [[nodiscard]] const ExpectedBeforeBinding& expectedBefore()
      const noexcept
  {
    return _expected_before;
  }

 private:
  PendingStructuredBatch(
      timing_edit::PreparedBatch prepared_batch,
      timing_edit::TimingEditResolverDelta resolver_delta,
      AffectedCapturePlan capture_plan,
      ExpectedBeforeBinding expected_before)
      : _prepared_batch(std::move(prepared_batch)),
        _resolver_delta(std::move(resolver_delta)),
        _capture_plan(std::move(capture_plan)),
        _expected_before(std::move(expected_before))
  {
  }

  timing_edit::PreparedBatch _prepared_batch;
  timing_edit::TimingEditResolverDelta _resolver_delta;
  AffectedCapturePlan _capture_plan;
  ExpectedBeforeBinding _expected_before;

  friend class DualCsrStructuredBatchPreparer;
  friend class TimingEditDualCsrDescriptorBuilder;
};

class DualCsrStructuredBatchPreparer final
{
 public:
  // Pure pre-candidate expansion. Native membership, collision, and Liberty
  // checks remain authoritative in the isolated candidate apply path.
  [[nodiscard]] static PendingStructuredBatch prepare(
      const timing_edit::DesignEditPlan& plan,
      const timing_edit::TimingEditResolverState& committed_resolver,
      const DualSemanticReadView& committed_view,
      const DualCsrState& exact_committed_graph,
      const std::optional<timing_edit::CommittedEditIdentity>& exact_committed_identity,
      const DualCsrArcGroupIndex& committed_arc_group_index);

 private:
  [[nodiscard]] static PendingStructuredBatch prepareBound(
      timing_edit::EditBatch edits,
      std::shared_ptr<const timing_edit::BaselineSemanticIdentity> baseline,
      const timing_edit::TimingEditResolverState& committed_resolver,
      const DualSemanticReadView& committed_view,
      const DualCsrState& exact_committed_graph,
      const std::optional<timing_edit::CommittedEditIdentity>& exact_committed_identity,
      const DualCsrArcGroupIndex& committed_arc_group_index);

  friend class DualCsrTransactionCoordinator;
};

}  // namespace ista::timing_dual_csr
