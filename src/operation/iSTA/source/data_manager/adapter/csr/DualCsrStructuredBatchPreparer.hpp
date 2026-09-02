// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <utility>

#include "AffectedCapturePlan.hpp"
#include "DualCsrArcGroupIndex.hpp"
#include "DualSemanticReadView.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_dual_csr {

class TimingEditDualCsrDescriptorBuilder;

// A DB-free, field-for-field binding of the committed values used while a
// native candidate is detached. It deliberately contains neither a nonce nor
// a digest: a later transaction can compare the graph, resolver, and semantic
// identity directly without introducing a dependency on the transaction
// aggregate into the native adapter.
class ExpectedBeforeBinding final
{
 public:
  [[nodiscard]] const DualCsrState& graph() const noexcept
  {
    return _graph;
  }
  [[nodiscard]] const timing_edit::TimingEditResolverState& resolver()
      const noexcept
  {
    return _resolver;
  }
  [[nodiscard]] const timing_edit::CommittedEditIdentity& identity()
      const noexcept
  {
    return _identity;
  }
  [[nodiscard]] const DualCsrArcGroupIndex& arcGroupIndex()
      const noexcept
  {
    return _arc_group_index;
  }
  [[nodiscard]] std::uint64_t graphValueCopyCount() const noexcept
  {
    return 1;
  }
  [[nodiscard]] std::uint64_t graphValueCopyMinimumBytes() const noexcept
  {
    return _graph_value_copy_minimum_bytes;
  }

  friend bool operator==(const ExpectedBeforeBinding&,
                         const ExpectedBeforeBinding&) = default;

 private:
  ExpectedBeforeBinding(
      DualCsrState graph,
      timing_edit::TimingEditResolverState resolver,
      timing_edit::CommittedEditIdentity identity,
      DualCsrArcGroupIndex arc_group_index,
      std::uint64_t graph_value_copy_minimum_bytes)
      : _graph(std::move(graph)),
        _resolver(std::move(resolver)),
        _identity(std::move(identity)),
        _arc_group_index(std::move(arc_group_index)),
        _graph_value_copy_minimum_bytes(graph_value_copy_minimum_bytes)
  {
  }

  DualCsrState _graph;
  timing_edit::TimingEditResolverState _resolver;
  timing_edit::CommittedEditIdentity _identity;
  DualCsrArcGroupIndex _arc_group_index;
  std::uint64_t _graph_value_copy_minimum_bytes{0};

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
  [[nodiscard]] const timing_edit::TimingEditResolverState& stagedResolver()
      const noexcept
  {
    return _staged_resolver;
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
      timing_edit::TimingEditResolverState staged_resolver,
      AffectedCapturePlan capture_plan,
      ExpectedBeforeBinding expected_before)
      : _prepared_batch(std::move(prepared_batch)),
        _staged_resolver(std::move(staged_resolver)),
        _capture_plan(std::move(capture_plan)),
        _expected_before(std::move(expected_before))
  {
  }

  timing_edit::PreparedBatch _prepared_batch;
  timing_edit::TimingEditResolverState _staged_resolver;
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
      timing_edit::DesignEditPlan plan,
      const timing_edit::TimingEditResolverState& committed_resolver,
      const DualSemanticReadView& committed_view,
      const DualCsrState& exact_committed_graph,
      const timing_edit::CommittedEditIdentity& exact_committed_identity,
      const DualCsrArcGroupIndex& committed_arc_group_index);
};

}  // namespace ista::timing_dual_csr
