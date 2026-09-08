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

// Own the graph and index needed to build the detached candidate descriptor.
// Resolver and identity copies serve only the diagnostic exact-before proof.
class ExpectedBeforeBinding final
{
 public:
  [[nodiscard]] const DualCsrState& graph() const noexcept
  {
    return _graph;
  }
  [[nodiscard]] const timing_edit::TimingEditResolverState& resolver() const { return _resolver.value(); }
  [[nodiscard]] const timing_edit::CommittedEditIdentity& identity() const { return _identity.value(); }
  [[nodiscard]] const DualCsrArcGroupIndex& arcGroupIndex()
      const noexcept
  {
    return _arc_group_index;
  }
  friend bool operator==(const ExpectedBeforeBinding&,
                         const ExpectedBeforeBinding&) = default;

 private:
  ExpectedBeforeBinding(DualCsrState graph, const timing_edit::TimingEditResolverState& resolver, const timing_edit::CommittedEditIdentity& identity,
                        DualCsrArcGroupIndex arc_group_index)
      : _graph(std::move(graph)), _arc_group_index(std::move(arc_group_index))
  {
    if constexpr (incregpusta_debug_checks_enabled) {
      _resolver.emplace(resolver);
      _identity.emplace(identity);
    }
  }

  DualCsrState _graph;
  std::optional<timing_edit::TimingEditResolverState> _resolver;
  std::optional<timing_edit::CommittedEditIdentity> _identity;
  DualCsrArcGroupIndex _arc_group_index;

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
