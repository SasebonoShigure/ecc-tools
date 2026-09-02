// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTransactionCoordinator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "../../data_manager/adapter/csr/DualCsrSemanticIdentity.hpp"
#include "../../data_manager/adapter/csr/TimingEditDualCsrDescriptorBuilder.hpp"
#include "NarrowCaptureAuditValidation.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "DualCsrArcGroupResolverValidation.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrUpdater.hpp"

namespace ista::timing_dual_csr {
namespace {

using SteadyClock = std::chrono::steady_clock;

double elapsedMilliseconds(SteadyClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(
             SteadyClock::now() - start)
      .count();
}

void rejectViolation(DualCsrViolations& errors, DualCsrViolationCode code,
                     std::string field, std::string message)
{
  errors.push_back({code, std::move(field), std::move(message)});
}

bool prefixMatches(const CommittedDualCsrAnalysisState& committed,
                   const CommittedDualCsrAnalysisState& staged)
{
  if (staged.descriptor_trace.size() != committed.descriptor_trace.size() + 1
      || staged.semantic_trace.size() != committed.semantic_trace.size() + 1)
    return false;
  return std::equal(committed.descriptor_trace.begin(),
                    committed.descriptor_trace.end(),
                    staged.descriptor_trace.begin())
         && std::equal(committed.semantic_trace.begin(),
                       committed.semantic_trace.end(),
                       staged.semantic_trace.begin());
}

bool baselineMatches(const CommittedDualCsrAnalysisState& committed,
                     const timing_edit::PreparedBatch& batch)
{
  if (!committed.semantic_trace.empty())
    return batch.baseline() == committed.semantic_trace.front().baseline;
  return committed.generation.value() == 0
         && committed.identity.generation
                == timing_edit::TransactionGeneration{0}
         && committed.identity.current_fingerprints
                == batch.baseline().fingerprints
         && committed.identity.canonical_pre_state
                == batch.baseline().canonical_pre_state;
}

timing_edit::DesignEditPlan semanticPlan(
    const timing_edit::PreparedBatch& batch)
{
  return {.schema_version = timing_edit::kDesignEditPlanSchema,
          .baseline = batch.baseline(),
          .expected_generation = batch.expectedGeneration(),
          .edits = batch.normalizedEdits()};
}

std::vector<std::uint8_t> activeArcMask(const DualCsrState& graph)
{
  std::vector<std::uint8_t> mask;
  mask.reserve(graph.arc_slots.size());
  for (const auto& slot : graph.arc_slots)
    mask.push_back(slot.active ? 1U : 0U);
  return mask;
}

void clearSession(csr::CsrSnapshotIdentity& identity) noexcept
{
  identity.session_token = 0;
}

std::vector<DualCsrBatchDescriptor> sessionNeutralDescriptors(
    std::vector<DualCsrBatchDescriptor> descriptors)
{
  for (auto& descriptor : descriptors) {
    descriptor.session_token = timing_edit::TimingEditSessionToken{};
    if (descriptor.reference_payload.source_identity) {
      clearSession(
          descriptor.reference_payload.source_identity->expected);
      clearSession(
          descriptor.reference_payload.source_identity->replacement);
    }
  }
  return descriptors;
}

bool recoveryTelemetryMatches(const DualCsrTelemetry& current,
                              const DualCsrTelemetry& rebuilt)
{
  if (current.full_rebuild_count
      == std::numeric_limits<std::uint64_t>::max())
    return false;
  DualCsrTelemetry expected = current;
  ++expected.full_rebuild_count;
  return expected == rebuilt;
}

bool recoverySemanticsMatch(
    const CommittedDualCsrAnalysisState& current,
    const CommittedDualCsrAnalysisState& rebuilt)
{
  auto current_reference = current.graph.reference_payload;
  auto rebuilt_reference = rebuilt.graph.reference_payload;
  clearSession(current_reference.source_identity);
  clearSession(rebuilt_reference.source_identity);

  auto current_timing = current.timing;
  auto rebuilt_timing = rebuilt.timing;
  clearSession(current_timing.values.source_identity);
  clearSession(rebuilt_timing.values.source_identity);

  auto current_index = current.arc_group_index;
  auto rebuilt_index = rebuilt.arc_group_index;
  current_index.session_token = timing_edit::TimingEditSessionToken{};
  rebuilt_index.session_token = timing_edit::TimingEditSessionToken{};

  return current.generation == rebuilt.generation
         && current.graph.high_water == rebuilt.graph.high_water
         && current.graph.nodes == rebuilt.graph.nodes
         && current.graph.arc_slots == rebuilt.graph.arc_slots
         && current.graph.base_out.row_offsets
                == rebuilt.graph.base_out.row_offsets
         && current.graph.base_out.physical_slots
                == rebuilt.graph.base_out.physical_slots
         && current.graph.base_in.row_offsets
                == rebuilt.graph.base_in.row_offsets
         && current.graph.base_in.physical_slots
                == rebuilt.graph.base_in.physical_slots
         && current.graph.aux_out.row_offsets
                == rebuilt.graph.aux_out.row_offsets
         && current.graph.aux_out.row_sources
                == rebuilt.graph.aux_out.row_sources
         && current.graph.aux_out.arc_ids == rebuilt.graph.aux_out.arc_ids
         && current.graph.aux_out.source_to_aux_row
                == rebuilt.graph.aux_out.source_to_aux_row
         && current_reference == rebuilt_reference
         && current_timing == rebuilt_timing
         && current.resolver == rebuilt.resolver
         && current_index == rebuilt_index
         && current.identity == rebuilt.identity
         && recoveryTelemetryMatches(current.graph.telemetry,
                                     rebuilt.graph.telemetry)
         && current.semantic_trace == rebuilt.semantic_trace
         && sessionNeutralDescriptors(current.descriptor_trace)
                == sessionNeutralDescriptors(rebuilt.descriptor_trace)
         && current.traversal_telemetry == rebuilt.traversal_telemetry;
}

}  // namespace

DualCsrStageAttempt makeStagedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& committed,
    VerifiedStructuredBatch&& verified_batch)
{
  DualCsrStageAttempt attempt;
  timing_edit::PreparedBatch prepared_batch
      = std::move(verified_batch._prepared_batch);
  timing_edit::TimingEditResolverState staged_resolver
      = std::move(verified_batch._staged_resolver);
  DualCsrBatchDescriptor descriptor
      = std::move(verified_batch._descriptor);
  ExpectedBeforeBinding expected_before
      = std::move(verified_batch._expected_before);
  NarrowCaptureAudit capture_audit
      = std::move(verified_batch._capture_audit);
  const std::uint64_t expected = committed.generation.value();
  attempt.violations = validateCommittedDualCsrAnalysisState(committed);
  if (!attempt.violations.empty())
    return attempt;
  if (expected == std::numeric_limits<std::uint64_t>::max()
      || prepared_batch.sealed()
      || prepared_batch.targetPostCommittedIdentity()
      || expected_before.graph() != committed.graph
      || expected_before.resolver() != committed.resolver
      || expected_before.arcGroupIndex() != committed.arc_group_index
      || expected_before.identity() != committed.identity
      || prepared_batch.sessionToken() != committed.graph.session_token
      || prepared_batch.expectedGeneration() != committed.generation
      || prepared_batch.targetGeneration()
             != timing_edit::TransactionGeneration{expected + 1}
      || prepared_batch.expectedPreCommittedIdentity() != committed.identity
      || prepared_batch.policyReceipt().disposition
             != timing_edit::EditDisposition::kSupported
      || !baselineMatches(committed, prepared_batch)
      || descriptor.session_token != prepared_batch.sessionToken()
      || descriptor.expected_generation
             != prepared_batch.expectedGeneration()
      || descriptor.target_generation
             != prepared_batch.targetGeneration()
      || descriptor.before != prepared_batch.slotPlan().before
      || descriptor.after != prepared_batch.slotPlan().after
      || staged_resolver.highWater() != descriptor.after
      || !validateNarrowCaptureAudit(
          capture_audit,
          NarrowCaptureAuditExpectation{
              descriptor.after, descriptor.logical_deactivations.size()})) {
    rejectViolation(attempt.violations, DualCsrViolationCode::kGeneration,
                    "stage",
                    "verified batch and exact committed state do not form the next generation");
    return attempt;
  }
  try {
    if (committed.resolver.stage(prepared_batch) != staged_resolver) {
      rejectViolation(attempt.violations,
                      DualCsrViolationCode::kConflict,
                      "stage.resolver",
                      "verified staged resolver is not the exact batch transition");
      return attempt;
    }
  } catch (const std::invalid_argument& error) {
    rejectViolation(attempt.violations, DualCsrViolationCode::kConflict,
                    "stage.resolver", error.what());
    return attempt;
  }

  const auto update_started = SteadyClock::now();
  DualCsrIndexedUpdateAttempt graph_attempt = DualCsrUpdater::stage(
      committed.graph, committed.arc_group_index, descriptor);
  if (!graph_attempt.success()) {
    attempt.violations = std::move(graph_attempt.violations);
    return attempt;
  }
  attempt.timings.dual_csr_update_ms
      = elapsedMilliseconds(update_started);
  DualCsrArcGroupIndex arc_group_index
      = std::move(*graph_attempt.arc_group_index);
  const auto invariant_started = SteadyClock::now();
  attempt.violations = validateDualCsrArcGroupResolverTransition(
      committed.arc_group_index, arc_group_index, descriptor,
      staged_resolver);
  attempt.timings.invariant_ms
      = elapsedMilliseconds(invariant_started);
  if (!attempt.violations.empty())
    return attempt;
  DualCsrState graph = std::move(*graph_attempt.state);
  csr::CsrCpuReferenceBackend backend;
  DualCsrEdgeViewImpl edges(graph);
  DualCsrTimingPayloadViewImpl payloads(graph);
  auto computation = backend.computeWithTelemetry(edges, payloads);
  attempt.timings.full_levelization_ms
      = computation.phase_timings.levelization_ms;
  attempt.timings.full_propagation_ms
      = computation.phase_timings.propagation_ms;
  auto* values = std::get_if<csr::CsrTimingResult>(
      &computation.computation.outcome);
  if (values == nullptr) {
    rejectViolation(attempt.violations,
                    DualCsrViolationCode::kTimingResult,
                    "stage.full_cpu",
                    "direct-view full CPU computation did not produce timing");
    return attempt;
  }
  DualCsrTimingResult timing{std::move(*values), activeArcMask(graph)};
  graph.telemetry.traversal = computation.telemetry;

  timing_edit::CommittedEditIdentity identity
      = deriveDualCsrCommittedIdentity(graph, staged_resolver);
  timing_edit::DesignEditPlan semantic_batch = semanticPlan(prepared_batch);

  CommittedDualCsrAnalysisState staged{
      .graph = std::move(graph),
      .timing = std::move(timing),
      .resolver = std::move(staged_resolver),
      .arc_group_index = std::move(arc_group_index),
      .identity = std::move(identity),
      .generation = descriptor.target_generation,
      .descriptor_trace = committed.descriptor_trace,
      .semantic_trace = committed.semantic_trace,
      .traversal_telemetry = computation.telemetry};
  staged.descriptor_trace.push_back(std::move(descriptor));
  staged.semantic_trace.push_back(std::move(semantic_batch));
  const auto verify_started = SteadyClock::now();
  attempt.violations = validateCommittedDualCsrAnalysisState(staged);
  attempt.timings.verify_ms
      = elapsedMilliseconds(verify_started);
  if (attempt.violations.empty())
    attempt.staged.emplace(
        DualCsrVerifiedStage(committed, std::move(staged)));
  return attempt;
}

DualCsrRecoveryAttempt makeDualCsrVerifiedRecovery(
    const CommittedDualCsrAnalysisState& poisoned_committed,
    CommittedDualCsrAnalysisState rebuilt)
{
  DualCsrRecoveryAttempt attempt;
  attempt.violations
      = validateCommittedDualCsrAnalysisState(poisoned_committed);
  DualCsrViolations rebuilt_errors
      = validateCommittedDualCsrAnalysisState(rebuilt);
  attempt.violations.insert(attempt.violations.end(),
                            std::make_move_iterator(rebuilt_errors.begin()),
                            std::make_move_iterator(rebuilt_errors.end()));
  auto poisoned_binding_errors = validateDualCsrArcGroupResolverFull(
      poisoned_committed.arc_group_index, poisoned_committed.resolver);
  auto rebuilt_binding_errors = validateDualCsrArcGroupResolverFull(
      rebuilt.arc_group_index, rebuilt.resolver);
  attempt.violations.insert(attempt.violations.end(),
      std::make_move_iterator(poisoned_binding_errors.begin()),
      std::make_move_iterator(poisoned_binding_errors.end()));
  attempt.violations.insert(attempt.violations.end(),
      std::make_move_iterator(rebuilt_binding_errors.begin()),
      std::make_move_iterator(rebuilt_binding_errors.end()));
  if (poisoned_committed.graph.session_token
          == rebuilt.graph.session_token
      || !recoverySemanticsMatch(poisoned_committed, rebuilt))
    rejectViolation(attempt.violations, DualCsrViolationCode::kConflict,
                    "recovery",
                    "fresh recovery must rebind only session identity, increment full-rebuild count exactly once, and preserve every other committed semantic and telemetry field");
  if (attempt.violations.empty())
    attempt.recovery.emplace(
        DualCsrVerifiedRecovery(poisoned_committed,
                                std::move(rebuilt)));
  return attempt;
}

DualCsrTransactionCoordinator::DualCsrTransactionCoordinator(
    CommittedDualCsrAnalysisState initial)
    : _committed(std::move(initial))
{
  const auto errors = validateCommittedDualCsrAnalysisState(_committed);
  const auto binding_errors = validateDualCsrArcGroupResolverFull(
      _committed.arc_group_index, _committed.resolver);
  if (!errors.empty() || !binding_errors.empty())
    throw std::invalid_argument("invalid initial Dual-CSR aggregate");
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::reject(
    DualCsrTransactionCode code) const noexcept
{
  return {code, false, _poisoned};
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::publish(
    DualCsrVerifiedStage verified,
    DualCsrPublishFailpoint failpoint)
{
  if (_poisoned)
    return reject(DualCsrTransactionCode::kPoisoned);
  if (failpoint != DualCsrPublishFailpoint::kNone
      && failpoint != DualCsrPublishFailpoint::kBeforeSwap
      && failpoint != DualCsrPublishFailpoint::kAfterSwap)
    return reject(DualCsrTransactionCode::kUnknownFailpoint);
  if (!validateCommittedDualCsrAnalysisState(_committed).empty()) {
    _poisoned = true;
    return reject(DualCsrTransactionCode::kInvalidCommittedState);
  }
  if (!(verified._expected_committed == _committed))
    return reject(DualCsrTransactionCode::kTraceMismatch);

  CommittedDualCsrAnalysisState staged = std::move(verified._state);
  if (!validateCommittedDualCsrAnalysisState(staged).empty())
    return reject(DualCsrTransactionCode::kInvalidStagedState);
  if (staged.graph.session_token != _committed.graph.session_token
      || staged.generation.value() != _committed.generation.value() + 1)
    return reject(DualCsrTransactionCode::kStaleGeneration);
  if (!prefixMatches(_committed, staged))
    return reject(DualCsrTransactionCode::kTraceMismatch);
  if (failpoint == DualCsrPublishFailpoint::kBeforeSwap)
    return reject(DualCsrTransactionCode::kPreSwapFault);

  _committed.swap(staged);
  if (failpoint == DualCsrPublishFailpoint::kAfterSwap) {
    _poisoned = true;
    return {DualCsrTransactionCode::kPostSwapPoisoned, true, true};
  }
  return {DualCsrTransactionCode::kOk, true, false};
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::recover(
    DualCsrVerifiedRecovery verified)
{
  if (!_poisoned)
    return reject(DualCsrTransactionCode::kRecoveryNotPoisoned);
  if (!(verified._expected_poisoned == _committed))
    return reject(DualCsrTransactionCode::kTraceMismatch);

  CommittedDualCsrAnalysisState rebuilt = std::move(verified._state);
  if (!validateCommittedDualCsrAnalysisState(rebuilt).empty())
    return reject(DualCsrTransactionCode::kInvalidStagedState);
  if (rebuilt.graph.session_token == _committed.graph.session_token)
    return reject(DualCsrTransactionCode::kTraceMismatch);
  _committed.swap(rebuilt);
  _poisoned = false;
  return {DualCsrTransactionCode::kOk, true, false};
}

}  // namespace ista::timing_dual_csr
