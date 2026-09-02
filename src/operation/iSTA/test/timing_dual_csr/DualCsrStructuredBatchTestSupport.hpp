// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <optional>

#include "DualCsrStructuredBatchPreparer.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista {

class Database;

namespace timing_edit::test {
class GuardedTestSession;
}

}  // namespace ista

namespace ista::timing_dual_csr::test::structured_support {

struct StructuredFixture
{
  timing_edit::TimingEditSessionToken token;
  timing_edit::TimingEditResolverState resolver;
  timing_edit::CommittedEditIdentity identity;
  timing_edit::DesignEditPlan plan;
  csr::CsrReferenceInput baseline_reference_input;
  DualCsrState graph;
  DualCsrArcGroupIndex arc_group_index;
  csr::ArcId base_arc;
  csr::ArcId aux_arc;
  std::optional<csr::ArcId> unmoved_fanout_arc;
};

[[nodiscard]] StructuredFixture makeStructuredFixture(
    Database& database, timing_edit::TimingEditSessionToken token);

[[nodiscard]] StructuredFixture makeStructuredFanoutFixture(
    Database& database, timing_edit::TimingEditSessionToken token);

[[nodiscard]] PendingStructuredBatch prepare(
    const StructuredFixture& fixture);

// Contract tests may inspect only the move-only reader-minted capability.
[[nodiscard]] CandidateBoundCapture captureCandidateForContractTest(
    timing_edit::test::GuardedTestSession& session,
    const StructuredFixture& fixture,
    PendingStructuredBatch& pending);

// Replays pure prepare, executes and rolls back a real native candidate,
// validates narrow capture, and lets the private builder mint the capability.
[[nodiscard]] VerifiedStructuredBatch captureAndBuildVerifiedBatch(
    timing_edit::test::GuardedTestSession& session,
    const StructuredFixture& fixture);

}  // namespace ista::timing_dual_csr::test::structured_support
