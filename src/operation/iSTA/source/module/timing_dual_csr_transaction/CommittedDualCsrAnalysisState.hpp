// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <type_traits>
#include <utility>
#include <vector>

#include "DualCsrArcGroupIndex.hpp"
#include "DualCsrDescriptors.hpp"
#include "DualCsrTimingResultValidator.hpp"
#include "LevelizationExecutionTrace.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_dual_csr {

struct CommittedDualCsrAnalysisState
{
  DualCsrState graph;
  DualCsrTimingResult timing;
  timing_edit::TimingEditResolverState resolver;
  DualCsrArcGroupIndex arc_group_index;
  std::optional<timing_edit::CommittedEditIdentity> identity;
  timing_edit::TransactionGeneration generation;
  std::vector<DualCsrBatchDescriptor> descriptor_trace;
  std::vector<timing_edit::EditBatch> semantic_trace;
  csr::CsrTraversalTelemetry traversal_telemetry;
  std::vector<LevelizationExecutionTrace> execution_trace{};
  std::shared_ptr<const timing_edit::BaselineSemanticIdentity> baseline{};

  void swap(CommittedDualCsrAnalysisState& other) noexcept;

  friend bool operator==(const CommittedDualCsrAnalysisState&,
                         const CommittedDualCsrAnalysisState&);
};

struct DualCsrPendingBatch
{
  timing_edit::EditBatch semantic;
  LevelizationExecutionTrace execution;
  std::optional<DualCsrBatchDescriptor> descriptor;
};

[[nodiscard]] DualCsrViolations validatePendingDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& owner, const DualCsrTimingResult& timing,
    const timing_edit::TimingEditResolverState& resolver, const std::optional<timing_edit::CommittedEditIdentity>& identity,
    const DualCsrPendingBatch& batch, timing_edit::AppendOnlyHighWater before_high_water);

void swap(CommittedDualCsrAnalysisState& lhs,
          CommittedDualCsrAnalysisState& rhs) noexcept;

[[nodiscard]] DualCsrViolations validateCommittedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& state);

static_assert(std::is_nothrow_move_constructible_v<timing_edit::EditBatch>);
static_assert(std::is_nothrow_move_constructible_v<LevelizationExecutionTrace>);
static_assert(std::is_nothrow_move_constructible_v<DualCsrBatchDescriptor>);
static_assert(std::is_nothrow_swappable_v<DualCsrState>);
static_assert(std::is_nothrow_swappable_v<DualCsrTimingResult>);
static_assert(
    std::is_nothrow_swappable_v<timing_edit::TimingEditResolverState>);
static_assert(std::is_nothrow_swappable_v<DualCsrArcGroupIndex>);
static_assert(
    std::is_nothrow_swappable_v<timing_edit::CommittedEditIdentity>);
static_assert(std::is_nothrow_swappable_v<
              std::vector<DualCsrBatchDescriptor>>);
static_assert(std::is_nothrow_swappable_v<
              std::vector<timing_edit::EditBatch>>);
static_assert(std::is_nothrow_swappable_v<
              std::vector<LevelizationExecutionTrace>>);
static_assert(noexcept(std::declval<CommittedDualCsrAnalysisState&>().swap(
    std::declval<CommittedDualCsrAnalysisState&>())));

}  // namespace ista::timing_dual_csr
