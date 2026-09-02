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
#include "TimingEditResolver.hpp"

namespace ista::timing_dual_csr {

struct CommittedDualCsrAnalysisState
{
  DualCsrState graph;
  DualCsrTimingResult timing;
  timing_edit::TimingEditResolverState resolver;
  DualCsrArcGroupIndex arc_group_index;
  timing_edit::CommittedEditIdentity identity;
  timing_edit::TransactionGeneration generation;
  std::vector<DualCsrBatchDescriptor> descriptor_trace;
  std::vector<timing_edit::DesignEditPlan> semantic_trace;
  csr::CsrTraversalTelemetry traversal_telemetry;

  void swap(CommittedDualCsrAnalysisState& other) noexcept;

  friend bool operator==(const CommittedDualCsrAnalysisState&,
                         const CommittedDualCsrAnalysisState&) = default;
};

void swap(CommittedDualCsrAnalysisState& lhs,
          CommittedDualCsrAnalysisState& rhs) noexcept;

[[nodiscard]] DualCsrViolations validateCommittedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& state);

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
              std::vector<timing_edit::DesignEditPlan>>);
static_assert(noexcept(std::declval<CommittedDualCsrAnalysisState&>().swap(
    std::declval<CommittedDualCsrAnalysisState&>())));

}  // namespace ista::timing_dual_csr
