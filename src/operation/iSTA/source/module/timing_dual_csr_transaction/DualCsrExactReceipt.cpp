// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrExactReceipt.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "CsrReferenceValidator.hpp"
namespace ista::timing_dual_csr {
namespace {

void reject(DualCsrViolations& errors, DualCsrViolationCode code,
            std::string field, std::string message)
{
  errors.push_back({code, std::move(field), std::move(message)});
}

DualCsrStructuralReceipt structuralReceipt(
    const CommittedDualCsrAnalysisState& committed)
{
  const DualCsrState& graph = committed.graph;
  DualCsrStructuralReceipt receipt{
      .generation = graph.generation,
      .high_water = graph.high_water,
      .arc_group_index = committed.arc_group_index,
      .inactive_arcs = {},
      .base_out = graph.base_out,
      .base_in = graph.base_in,
      .aux_out = graph.aux_out,
      .aux_edge_extent = {},
      .update_telemetry = graph.telemetry};
  for (const auto& row : graph.aux_out.row_arcs) {
    receipt.aux_edge_extent.logical_size += row.size();
    receipt.aux_edge_extent.capacity += row.capacity();
  }
  receipt.inactive_arcs.reserve(graph.arc_slots.size());
  for (std::size_t index = 0; index < graph.arc_slots.size(); ++index)
    if (!graph.arc_slots[index].active)
      receipt.inactive_arcs.push_back(
          {csr::ArcId{static_cast<std::uint64_t>(index)},
           graph.arc_slots[index].payload});
  return receipt;
}

constexpr std::array<DualCsrReceiptPhase, 7> kBatchPhases{
    DualCsrReceiptPhase::kPrepareBatch,
    DualCsrReceiptPhase::kDirectPrepare,
    DualCsrReceiptPhase::kDualCsrUpdate,
    DualCsrReceiptPhase::kFullLevelization,
    DualCsrReceiptPhase::kFullPropagation,
    DualCsrReceiptPhase::kIntrinsicVerify,
    DualCsrReceiptPhase::kPublish};

DualCsrReceiptPhase expectedBatchPhase(
    const CommittedDualCsrAnalysisState& committed,
    std::size_t batch, std::size_t phase)
{
  if (phase == 3 && committed.execution_trace.at(batch).mode()
                        == LevelizationExecutionMode::kMixedStage6)
    return DualCsrReceiptPhase::kMixedLevelization;
  return kBatchPhases.at(phase);
}

DualCsrViolations validateEvidence(
    const CommittedDualCsrAnalysisState& committed,
    const DualCsrTransactionEvidence& evidence)
{
  DualCsrViolations errors;
  const std::uint64_t generation = committed.generation.value();
  constexpr std::uint64_t kBatchPhaseCount = kBatchPhases.size();
  const bool phase_count_valid
      = generation
        <= std::numeric_limits<std::uint64_t>::max()
               / kBatchPhaseCount;
  const std::uint64_t expected_phases
      = phase_count_valid ? generation * kBatchPhaseCount : 0;
  if (generation == 0 || !phase_count_valid
      || evidence.phase_trace.size() != expected_phases) {
    reject(errors, DualCsrViolationCode::kGeneration,
           "transaction.phase_trace",
           "successful run must contain the execution phases of each committed batch");
  } else if (committed.execution_trace.size() != generation) {
    reject(errors, DualCsrViolationCode::kGeneration,
           "transaction.execution_trace",
           "phase validation requires one execution record per batch");
  } else {
    for (std::uint64_t batch = 0; batch < generation; ++batch)
      for (std::size_t phase = 0; phase < kBatchPhases.size(); ++phase)
        if (evidence.phase_trace[
                batch * kBatchPhases.size() + phase]
            != expectedBatchPhase(committed, batch, phase))
          reject(errors, DualCsrViolationCode::kConflict,
                 "transaction.phase_trace",
                 "batch phase order or levelization mode differs from the committed execution trace");
  }
  const auto& counters = evidence.operation_counters;
  if (counters.native_full_graph_exports != 0
      || counters.native_full_reference_exports != 0
      || counters.host_snapshot_rebuilds != 0
      || counters.baseline_factory_calls != 0
      || counters.fallback_calls != 0
      || committed.graph.telemetry.fallback_count != 0)
    reject(errors, DualCsrViolationCode::kConflict,
           "transaction.operation_counters",
           "targeted batch interval observed a forbidden full/fallback operation");
  if (!evidence.incremental_batch_executed
      || evidence.propagation_scope
             != DualCsrPropagationScope::kFullGraph)
    reject(errors, DualCsrViolationCode::kConflict,
           "transaction.run_completion",
           "receipt requires successful committed full propagation");
  return errors;
}

DualCsrViolations validateBaselineReferenceInput(
    const CommittedDualCsrAnalysisState& committed,
    const csr::CsrReferenceInput& baseline)
{
  DualCsrViolations errors;
  if (!csr::validateReferenceInputForSession(
           baseline, committed.graph.session_token.value())
           .empty())
    reject(errors, DualCsrViolationCode::kExpectedValue,
           "transaction.baseline_reference_input",
           "generation-zero reference input is not strict-valid for the committed session");
  if (committed.semantic_trace.empty() || !committed.baseline) {
    reject(errors, DualCsrViolationCode::kGeneration,
           "transaction.semantic_trace",
           "successful receipt requires a semantic batch trace");
    return errors;
  }
  const auto& identity = baseline.source_identity;
  const auto& expected
      = committed.baseline->fingerprints;
  if (identity.fingerprint_schema_version != expected.schema_version
      || identity.topology_fingerprint != expected.topology
      || identity.clock_constraint_fingerprint
             != expected.clock_constraint
      || identity.library_fingerprint != expected.library
      || identity.corner_fingerprint != expected.corner
      || identity.parasitic_fingerprint != expected.parasitic
      || identity.timing_input_fingerprint != expected.timing_input)
    reject(errors, DualCsrViolationCode::kExpectedValue,
           "transaction.baseline_reference_input.source_identity",
           "generation-zero typed anchor differs from the first semantic baseline identity");
  return errors;
}

}  // namespace

DualCsrViolations validateDualCsrStructuralReceipt(
    const CommittedDualCsrAnalysisState& committed,
    const DualCsrStructuralReceipt& receipt)
{
  DualCsrViolations errors
      = validateCommittedDualCsrAnalysisState(committed);
  if (receipt != structuralReceipt(committed))
    reject(errors, DualCsrViolationCode::kExpectedValue,
           "structural_receipt",
           "structural receipt differs from the validated committed state");
  return errors;
}

DualCsrPublishedObservationAttempt makeDualCsrPublishedObservation(
    const CommittedDualCsrAnalysisState& committed)
{
  DualCsrPublishedObservationAttempt attempt;
  attempt.violations = validateCommittedDualCsrAnalysisState(committed);
  if (!attempt.violations.empty())
    return attempt;
  auto projected = projectDualCsrFunctionalSemanticReceipt(
      committed.graph, committed.timing);
  if (!projected.success()) {
    attempt.violations.insert(
        attempt.violations.end(),
        std::make_move_iterator(projected.violations.begin()),
        std::make_move_iterator(projected.violations.end()));
    return attempt;
  }
  attempt.observation = DualCsrPublishedObservation{
      .functional = std::move(*projected.receipt),
      .structural = structuralReceipt(committed)};
  return attempt;
}

DualCsrExactReceiptAttempt finalizeDualCsrExactReceipt(
    const CommittedDualCsrAnalysisState& committed,
    DualCsrPublishedObservation published,
    csr::CsrReferenceInput baseline_reference_input,
    DualCsrTransactionEvidence evidence)
{
  DualCsrExactReceiptAttempt attempt;
  if (!incregpusta_debug_checks_enabled || !committed.identity
      || committed.descriptor_trace.size() != committed.generation.value()) {
    reject(attempt.violations, DualCsrViolationCode::kConflict, "receipt.diagnostics",
           "full receipt requires debug checks and complete identity/descriptor evidence");
    return attempt;
  }
  auto expected = makeDualCsrPublishedObservation(committed);
  attempt.violations = std::move(expected.violations);
  if (!expected.success() || published != *expected.observation)
    reject(attempt.violations, DualCsrViolationCode::kExpectedValue,
           "published_observation",
           "post-publish functional/structural observation is not exact");
  DualCsrViolations evidence_errors
      = validateEvidence(committed, evidence);
  attempt.violations.insert(
      attempt.violations.end(),
      std::make_move_iterator(evidence_errors.begin()),
      std::make_move_iterator(evidence_errors.end()));
  DualCsrViolations baseline_errors
      = validateBaselineReferenceInput(committed,
                                       baseline_reference_input);
  attempt.violations.insert(
      attempt.violations.end(),
      std::make_move_iterator(baseline_errors.begin()),
      std::make_move_iterator(baseline_errors.end()));
  if (!attempt.violations.empty())
    return attempt;

  std::vector<timing_edit::DesignEditPlan> semantic_trace;
  semantic_trace.reserve(committed.semantic_trace.size());
  for (const auto& record : committed.semantic_trace)
    semantic_trace.push_back({.baseline = *committed.baseline, .expected_generation = record.expected_generation, .edits = record.edits});
  DualCsrTransactionReceipt transaction{
      .session_token = committed.graph.session_token,
      .generation = committed.generation,
      .baseline_reference_input = std::move(baseline_reference_input),
      .resolver = committed.resolver,
      .arc_group_index = committed.arc_group_index,
      .identity = committed.identity.value(),
      .descriptor_trace = committed.descriptor_trace,
      .semantic_trace = std::move(semantic_trace),
      .traversal_telemetry = committed.traversal_telemetry,
      .execution_trace = committed.execution_trace,
      .update_telemetry = committed.graph.telemetry,
      .evidence = std::move(evidence)};
  attempt.receipt = DualCsrExactReceipt{
      .published = std::move(published),
      .transaction = std::move(transaction)};
  return attempt;
}

}  // namespace ista::timing_dual_csr
