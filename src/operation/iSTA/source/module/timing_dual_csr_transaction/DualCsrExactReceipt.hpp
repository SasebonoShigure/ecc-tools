// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "AffectedCapturePlan.hpp"
#include "CommittedDualCsrAnalysisState.hpp"
#include "DualCsrFunctionalProjection.hpp"

namespace ista::timing_dual_csr {

inline constexpr auto kDualCsrExactReceiptSchema
    = "ista-dual-csr-exact-receipt/1.4.0";

enum class DualCsrReceiptPhase : std::uint8_t
{
  kPreCandidatePrepare,
  kCandidateApplyExecute,
  kPostCandidateCapture,
  kDualCsrUpdate,
  kFullLevelization,
  kMixedLevelization,
  kFullPropagation,
  kIntrinsicVerify,
  kPublish,
  kNativeFinalRollback
};

enum class DualCsrPropagationScope : std::uint8_t
{
  kFullGraph
};

struct InactiveArcStructuralObservation
{
  csr::ArcId id;
  csr::ArcSemanticPayload historical_payload;

  friend bool operator==(
      const InactiveArcStructuralObservation&,
      const InactiveArcStructuralObservation&) = default;
};

struct DualCsrStructuralReceipt
{
  timing_edit::TransactionGeneration generation;
  timing_edit::AppendOnlyHighWater high_water;
  DualCsrArcGroupIndex arc_group_index;
  std::vector<InactiveArcStructuralObservation> inactive_arcs;
  BaseCsrState base_out;
  BaseCsrState base_in;
  AuxOutCsrState aux_out;
  DualCsrTelemetry update_telemetry;

  friend bool operator==(const DualCsrStructuralReceipt&,
                         const DualCsrStructuralReceipt&) = default;
};

struct DualCsrPublishedObservation
{
  FunctionalSemanticReceipt functional;
  DualCsrStructuralReceipt structural;

  friend bool operator==(const DualCsrPublishedObservation&,
                         const DualCsrPublishedObservation&) = default;
};

struct DualCsrOperationCounters
{
  std::uint64_t native_full_graph_exports{0};
  std::uint64_t native_full_reference_exports{0};
  std::uint64_t host_snapshot_rebuilds{0};
  std::uint64_t baseline_factory_calls{0};
  std::uint64_t fallback_calls{0};
  std::uint64_t candidate_creates{0};
  std::uint64_t candidate_clones{0};
  std::uint64_t candidate_applies{0};
  std::uint64_t candidate_executes{0};
  std::uint64_t candidate_installs{0};
  std::uint64_t candidate_rollbacks{0};

  friend bool operator==(const DualCsrOperationCounters&,
                         const DualCsrOperationCounters&) = default;
};

// This evidence is filled by the owner of the isolated native carrier. It is
// deliberately separate from publish authorization and can only finalize a
// successful receipt after the run-final rollback has completed.
struct DualCsrTransactionEvidence
{
  std::vector<DualCsrReceiptPhase> phase_trace;
  std::vector<NarrowCaptureAudit> capture_audits;
  DualCsrOperationCounters operation_counters;
  bool incremental_batch_executed{false};
  DualCsrPropagationScope propagation_scope{
      DualCsrPropagationScope::kFullGraph};
  bool native_final_rollback_completed{false};

  friend bool operator==(const DualCsrTransactionEvidence&,
                         const DualCsrTransactionEvidence&) = default;
};

struct DualCsrTransactionReceipt
{
  timing_edit::TimingEditSessionToken session_token;
  timing_edit::TransactionGeneration generation;
  csr::CsrReferenceInput baseline_reference_input;
  timing_edit::TimingEditResolverState resolver;
  DualCsrArcGroupIndex arc_group_index;
  timing_edit::CommittedEditIdentity identity;
  std::vector<DualCsrBatchDescriptor> descriptor_trace;
  std::vector<timing_edit::DesignEditPlan> semantic_trace;
  csr::CsrTraversalTelemetry traversal_telemetry;
  std::vector<LevelizationExecutionTrace> execution_trace;
  DualCsrTelemetry update_telemetry;
  DualCsrTransactionEvidence evidence;

  friend bool operator==(const DualCsrTransactionReceipt&,
                         const DualCsrTransactionReceipt&) = default;
};

struct DualCsrExactReceipt
{
  std::string schema{kDualCsrExactReceiptSchema};
  DualCsrPublishedObservation published;
  DualCsrTransactionReceipt transaction;

  friend bool operator==(const DualCsrExactReceipt&,
                         const DualCsrExactReceipt&) = default;
};

struct DualCsrPublishedObservationAttempt
{
  std::optional<DualCsrPublishedObservation> observation;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return observation.has_value() && violations.empty();
  }
};

struct DualCsrExactReceiptAttempt
{
  std::optional<DualCsrExactReceipt> receipt;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return receipt.has_value() && violations.empty();
  }
};

[[nodiscard]] DualCsrPublishedObservationAttempt
makeDualCsrPublishedObservation(
    const CommittedDualCsrAnalysisState& committed);

[[nodiscard]] DualCsrViolations validateDualCsrStructuralReceipt(
    const CommittedDualCsrAnalysisState& committed,
    const DualCsrStructuralReceipt& receipt);

[[nodiscard]] DualCsrExactReceiptAttempt finalizeDualCsrExactReceipt(
    const CommittedDualCsrAnalysisState& committed,
    DualCsrPublishedObservation published,
    csr::CsrReferenceInput baseline_reference_input,
    DualCsrTransactionEvidence evidence);

}  // namespace ista::timing_dual_csr
