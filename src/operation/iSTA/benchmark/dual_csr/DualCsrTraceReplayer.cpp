// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTraceReplayer.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "BufferTraceTypes.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "CpuIncrementalLevelizationRunner.hpp"
#include "DualCsrBaselineFactoryInstrumentationInternal.hpp"
#include "DualCsrNativeCandidateCarrierInternal.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "DualCsrTraceBaselineInternal.hpp"
#include "DualCsrTransactionCoordinator.hpp"
#include "DualSemanticReadView.hpp"
#include "NarrowTimingRecordReader.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista::timing_dual_csr {
namespace {

using detail::CandidateLifecycleSnapshot;
using detail::DualCsrNativeCandidateCarrier;

using SteadyClock = std::chrono::steady_clock;

double elapsedMilliseconds(SteadyClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(
             SteadyClock::now() - start)
      .count();
}

const char* narrowCaptureErrorToken(NarrowCaptureError error) noexcept
{
  switch (error) {
    case NarrowCaptureError::kBatchBindingMismatch:
      return "batch_binding_mismatch";
    case NarrowCaptureError::kInvalidGeneration:
      return "invalid_generation";
    case NarrowCaptureError::kInvalidHighWater:
      return "invalid_high_water";
    case NarrowCaptureError::kUnsupportedPolicy:
      return "unsupported_policy";
    case NarrowCaptureError::kParasiticModeMismatch:
      return "parasitic_mode_mismatch";
    case NarrowCaptureError::kDuplicateRequest:
      return "duplicate_request";
    case NarrowCaptureError::kUnplannedDependency:
      return "unplanned_dependency";
    case NarrowCaptureError::kNativeNodeNotFound:
      return "native_node_not_found";
    case NarrowCaptureError::kNativeInstanceNotFound:
      return "native_instance_not_found";
    case NarrowCaptureError::kNativeNetNotFound:
      return "native_net_not_found";
    case NarrowCaptureError::kArcMultiplicityMismatch:
      return "arc_multiplicity_mismatch";
    case NarrowCaptureError::kArcOrdinalMismatch:
      return "arc_ordinal_mismatch";
    case NarrowCaptureError::kArcKeyMismatch:
      return "arc_key_mismatch";
    case NarrowCaptureError::kMalformedNativeRecord:
      return "malformed_native_record";
    case NarrowCaptureError::kFullExportObserved:
      return "full_export_observed";
  }
  return "unknown";
}

void appendCaptureCounts(std::string& message, std::string_view label,
                         std::uint64_t requested, std::uint64_t reads,
                         std::uint64_t accepted)
{
  message += label;
  message += '=';
  message += std::to_string(requested);
  message += '/';
  message += std::to_string(reads);
  message += '/';
  message += std::to_string(accepted);
}

std::string narrowCaptureFailureMessage(const CandidateBoundCapture& capture)
{
  std::string message{"candidate-bound narrow capture failed: error="};
  const std::optional<NarrowCaptureError> error = capture.error();
  if (error.has_value()) {
    message += narrowCaptureErrorToken(*error);
    message += '(';
    message += std::to_string(static_cast<unsigned>(*error));
    message += ')';
  } else {
    message += "missing";
  }

  const NarrowCaptureAudit* audit = capture.audit();
  if (audit == nullptr)
    return message + " audit=missing";

  message += " audit{requested/reads/accepted ";
  appendCaptureCounts(message, "nodes", audit->requested_nodes,
                      audit->node_reads, audit->accepted_nodes);
  message += " instances=";
  appendCaptureCounts(message, "", audit->requested_instances,
                      audit->instance_reads, audit->accepted_instances);
  message += " arcs=";
  appendCaptureCounts(message, "", audit->requested_arcs,
                      audit->native_arc_candidate_reads,
                      audit->accepted_arcs);
  message += " deactivations=";
  appendCaptureCounts(message, "", audit->requested_deactivations,
                      audit->deactivated_arc_before_reads,
                      audit->accepted_deactivations);
  message += " load_nets=";
  appendCaptureCounts(message, "", audit->requested_load_nets,
                      audit->net_reads, audit->accepted_load_nets);
  message += " library_cells=";
  appendCaptureCounts(message, "", audit->requested_library_cells,
                      audit->library_cell_reads,
                      audit->accepted_library_cells);
  message += " cell_pin_capacitances=";
  appendCaptureCounts(message, "", audit->requested_cell_pin_capacitances,
                      audit->cell_pin_capacitance_reads,
                      audit->accepted_cell_pin_capacitances);
  message += " node_roles=";
  appendCaptureCounts(message, "", audit->requested_node_roles,
                      audit->node_role_reads, audit->accepted_node_roles);
  message += " captured_arc_records="
             + std::to_string(audit->captured_arc_records);
  message += " rejected_reads=" + std::to_string(audit->rejected_reads);
  message += " native_full_graph_exports="
             + std::to_string(audit->native_full_graph_exports);
  message += " native_full_reference_exports="
             + std::to_string(audit->native_full_reference_exports) + '}';
  return message;
}

[[noreturn]] void fail(DualCsrTraceFailureCode code,
                       std::string message)
{
  throw DualCsrTraceRuntimeError(
      code, "Dual-CSR targeted trace: " + std::move(message));
}

const std::vector<timing_edit::DesignEditPlan>& batches(
    const buffer_trace::ReplayProgram& program)
{
  return std::visit(
      [](const auto& workload)
          -> const std::vector<timing_edit::DesignEditPlan>& {
        return workload.batches;
      },
      program.workload);
}

std::uint64_t counterDelta(std::uint64_t before,
                           std::uint64_t after,
                           std::string_view label)
{
  if (after < before)
    fail(DualCsrTraceFailureCode::kForbiddenOperationObserved,
         std::string(label) + " counter moved backwards");
  return after - before;
}

void checkedAdd(std::uint64_t& value, std::uint64_t increment,
                std::string_view label)
{
  if (increment > std::numeric_limits<std::uint64_t>::max() - value)
    fail(DualCsrTraceFailureCode::kForbiddenOperationObserved,
         std::string(label) + " counter overflowed");
  value += increment;
}

void requireRollback(timing_edit::InstalledNativeDatabase& installed)
{
  if (!installed.active()
      || !DualCsrNativeCandidateCarrier::rollback(installed))
    fail(DualCsrTraceFailureCode::kRollbackRejected,
         "native candidate rollback did not restore the baseline");
}

timing_edit::NativeDatabaseCandidate nextCandidate(
    Database& baseline,
    std::optional<timing_edit::InstalledNativeDatabase>& installed)
{
  if (!installed)
    return DualCsrNativeCandidateCarrier::create(baseline);
  if (!installed->active())
    fail(DualCsrTraceFailureCode::kCandidateRejected,
         "cumulative installed candidate is not active");
  timing_edit::NativeDatabaseCandidate candidate
      = DualCsrNativeCandidateCarrier::clone(*installed);
  requireRollback(*installed);
  installed.reset();
  return candidate;
}

DualCsrOperationCounters measuredCounters(
    const csr::detail::NativeFullExportCounters& full_before,
    const csr::detail::NativeFullExportCounters& full_after,
    std::uint64_t baseline_before,
    std::uint64_t baseline_after,
    CandidateLifecycleSnapshot lifecycle_before,
    CandidateLifecycleSnapshot lifecycle_after,
    std::uint64_t fallback_calls)
{
  const auto lifecycle = detail::candidateLifecycleDelta(
      lifecycle_before, lifecycle_after);
  const std::uint64_t graph_exports = counterDelta(
      full_before.graph_exports, full_after.graph_exports,
      "native full graph export");
  const std::uint64_t reference_exports = counterDelta(
      full_before.reference_exports, full_after.reference_exports,
      "native full reference export");
  return {
      .native_full_graph_exports = graph_exports,
      .native_full_reference_exports = reference_exports,
      .host_snapshot_rebuilds = reference_exports,
      .baseline_factory_calls = counterDelta(
          baseline_before, baseline_after, "baseline factory"),
      .fallback_calls = fallback_calls,
      .candidate_creates = lifecycle.creates,
      .candidate_clones = lifecycle.clones,
      .candidate_applies = lifecycle.applies,
      .candidate_executes = lifecycle.executes,
      .candidate_installs = lifecycle.installs,
      .candidate_rollbacks = lifecycle.rollbacks};
}

void requireTargetedCounters(const DualCsrOperationCounters& counters,
                             std::uint64_t generation)
{
  if (counters.native_full_graph_exports != 0
      || counters.native_full_reference_exports != 0
      || counters.host_snapshot_rebuilds != 0
      || counters.baseline_factory_calls != 0
      || counters.fallback_calls != 0)
    fail(DualCsrTraceFailureCode::kForbiddenOperationObserved,
         "targeted interval observed a full, baseline, or fallback path");
  if (generation == 0
      || counters.candidate_creates != 1
      || counters.candidate_clones != generation - 1
      || counters.candidate_applies != generation
      || counters.candidate_executes != generation
      || counters.candidate_installs != generation
      || counters.candidate_rollbacks != generation)
    fail(DualCsrTraceFailureCode::kCandidateRejected,
         "measured cumulative candidate lifecycle differs");
}

DualCsrPublishedObservation observe(
    const CommittedDualCsrAnalysisState& committed)
{
  auto attempt = makeDualCsrPublishedObservation(committed);
  if (!attempt.success())
    fail(DualCsrTraceFailureCode::kReceiptRejected,
         "published aggregate cannot form an exact observation");
  return std::move(*attempt.observation);
}

DualCsrResourceObservation observeResources(
    const CommittedDualCsrAnalysisState& committed)
{
  auto attempt = observeDualCsrResources(committed.graph);
  if (!attempt.success())
    fail(DualCsrTraceFailureCode::kReceiptRejected,
         "committed aggregate cannot form an exact resource observation");
  return std::move(*attempt.observation);
}

bool matchesExtent(const DualCsrVectorMemoryObservation& observed,
                   DualCsrBufferKind expected_buffer,
                   const csr::CsrBufferExtent& expected_extent) noexcept
{
  return observed.buffer == expected_buffer
         && observed.logical_elements == expected_extent.logical_size
         && observed.capacity_elements == expected_extent.capacity;
}

void requireResourceDeclarations(
    const std::vector<DualCsrResourceObservation>& resources,
    const std::vector<DualCsrPublishedObservation>& observations,
    const DualCsrTransactionEvidence& evidence)
{
  if (resources.size() != observations.size()
      || resources.empty()
      || !evidence.incremental_batch_executed
      || evidence.propagation_scope
             != DualCsrPropagationScope::kFullGraph
      || evidence.operation_counters.native_full_graph_exports != 0
      || evidence.operation_counters.native_full_reference_exports != 0
      || evidence.operation_counters.host_snapshot_rebuilds != 0
      || evidence.operation_counters.fallback_calls != 0)
    fail(DualCsrTraceFailureCode::kReceiptRejected,
         "resource strategy declaration lacks targeted execution evidence");

  for (std::size_t index = 0; index < resources.size(); ++index) {
    const auto& resource = resources[index];
    const auto& structural = observations[index].structural;
    const auto& telemetry = structural.update_telemetry;
    const auto& buffers = resource.vector_memory.buffers;
    const std::array<csr::CsrBufferExtent, 8> extents{
        structural.base_out.offsets_extent,
        structural.base_out.edge_extent,
        structural.base_in.offsets_extent,
        structural.base_in.edge_extent,
        structural.aux_out.offsets_extent,
        structural.aux_out.source_extent,
        structural.aux_out.edge_extent,
        structural.aux_out.mapping_extent};
    bool memory_matches
        = resource.vector_memory.estimate_kind
              == DualCsrMemoryEstimateKind::kShallowVectorStorage
          && buffers[0].logical_elements
                 == structural.high_water.node_slots
          && buffers[1].logical_elements
                 == structural.high_water.arc_slots;
    for (std::size_t buffer = 0; buffer < buffers.size(); ++buffer) {
      const auto checked = detail::checkedShallowVectorStorageBytes(
          buffers[buffer].capacity_elements,
          buffers[buffer].element_size_bytes);
      memory_matches
          = memory_matches
            && buffers[buffer].buffer
                   == kDualCsrObservedBufferOrder[buffer]
            && checked.has_value()
            && *checked == buffers[buffer].estimated_reserved_bytes;
      if (buffer >= 2)
        memory_matches = memory_matches
                         && matchesExtent(buffers[buffer],
                                          kDualCsrObservedBufferOrder[
                                              buffer],
                                          extents[buffer - 2]);
    }
    if (resource.generation != structural.generation
        || resource.requested_strategy != kDualCsrRequestedStrategy
        || resource.executed_strategy != kDualCsrExecutedStrategy
        || resource.tombstone_ratio
               != dualCsrTombstoneRatio(
                   telemetry.base_physical_edges,
                   telemetry.base_tombstones)
        || telemetry.full_rebuild_count != 0
        || telemetry.fallback_count != 0 || !memory_matches)
      fail(DualCsrTraceFailureCode::kReceiptRejected,
           "resource observation differs from its published batch");
  }
}

}  // namespace

DualCsrTraceRunResult replayPrefixWithMode(
    const DualCsrTraceContext& context,
    const buffer_trace::ReplayProgram& program,
    std::size_t prefix_batches,
    LevelizationExecutionMode mode)
{
  const auto& trace_batches = batches(program);
  if (prefix_batches == 0 || prefix_batches > trace_batches.size())
    fail(DualCsrTraceFailureCode::kInvalidProgram,
         "prefix must select at least one complete frozen batch");
  const timing_edit::TimingEditSessionToken session_token{
      context.session_token};
  if (context.session_token == 0 || !context.authority.active()
      || context.authority.sessionToken() != session_token)
    fail(DualCsrTraceFailureCode::kInvalidBaseline,
         "operation authority and context session differ");

  const std::uint64_t baseline_factory_before
      = detail::currentBaselineFactoryCallCount();
  CommittedDualCsrAnalysisState baseline = detail::makeTraceBaseline(
      context.baseline_database, context.baseline_reference_input,
      session_token);
  const std::uint64_t targeted_baseline_before
      = detail::currentBaselineFactoryCallCount();
  const std::uint64_t baseline_outside = counterDelta(
      baseline_factory_before, targeted_baseline_before,
      "outside-targeted baseline factory");
  if (baseline_outside != 1)
    fail(DualCsrTraceFailureCode::kInvalidBaseline,
         "generation-zero baseline did not call its factory exactly once");

  const auto full_before
      = csr::detail::currentNativeFullExportCounters();
  const CandidateLifecycleSnapshot lifecycle_before
      = detail::currentCandidateLifecycleSnapshot();
  DualCsrTransactionCoordinator coordinator(std::move(baseline));
  std::optional<timing_edit::InstalledNativeDatabase> installed;
  std::vector<DualCsrPublishedObservation> observations;
  observations.reserve(prefix_batches);
  std::vector<DualCsrBatchPhaseTimings> phase_timings;
  phase_timings.reserve(prefix_batches);
  std::vector<DualCsrResourceObservation> resource_observations;
  resource_observations.reserve(prefix_batches);
  DualCsrTransactionEvidence evidence;
  std::uint64_t fallback_calls = 0;

  try {
    for (std::size_t index = 0; index < prefix_batches; ++index) {
      const auto& committed = coordinator.committed();
      DualCsrBatchPhaseTimings batch_timing{
          .generation_before = committed.generation.value()};
      if (trace_batches[index].expected_generation
              != committed.generation)
        fail(DualCsrTraceFailureCode::kInvalidProgram,
             "semantic batch generation is not contiguous");
      const auto prepare_started = SteadyClock::now();
      DualSemanticReadView committed_view(committed.graph);
      PendingStructuredBatch pending
          = DualCsrStructuredBatchPreparer::prepare(
              trace_batches[index], committed.resolver, committed_view,
              committed.graph, committed.identity,
              committed.arc_group_index);
      batch_timing.pre_candidate_prepare_ms
          = elapsedMilliseconds(prepare_started);
      evidence.phase_trace.push_back(
          DualCsrReceiptPhase::kPreCandidatePrepare);

      const auto candidate_started = SteadyClock::now();
      timing_edit::NativeDatabaseCandidate candidate
          = nextCandidate(context.baseline_database, installed);
      const auto translated = DualCsrNativeCandidateCarrier::apply(
          candidate, pending.preparedBatch(), pending.stagedResolver());
      if (!translated.applied())
        fail(DualCsrTraceFailureCode::kCandidateRejected,
             "native translation rejected a prepared frozen batch");
      installed.emplace(DualCsrNativeCandidateCarrier::installAndExecute(
          context.authority, std::move(candidate)));
      batch_timing.candidate_apply_execute_ms
          = elapsedMilliseconds(candidate_started);
      evidence.phase_trace.push_back(
          DualCsrReceiptPhase::kCandidateApplyExecute);

      const auto capture_started = SteadyClock::now();
      CandidateBoundCapture capture = NarrowTimingRecordReader{}.read(
          installed->database(), committed_view,
          committed.arc_group_index, pending.stagedResolver(),
          pending.preparedBatch(), pending.capturePlan());
      if (!capture.success())
        fail(DualCsrTraceFailureCode::kCaptureRejected,
             narrowCaptureFailureMessage(capture));
      VerifiedStructuredBatch verified
          = TimingEditDualCsrDescriptorBuilder::build(
              std::move(pending), std::move(capture));
      batch_timing.post_candidate_capture_ms
          = elapsedMilliseconds(capture_started);
      evidence.capture_audits.push_back(verified.captureAudit());
      evidence.phase_trace.push_back(
          DualCsrReceiptPhase::kPostCandidateCapture);

      std::optional<DualCsrVerifiedStage> verified_stage;
      DualCsrStageTimings stage_timings;
      if (mode == LevelizationExecutionMode::kMixedStage6) {
        auto staged = CpuIncrementalLevelizationRunner::run(
            committed, std::move(verified));
        if (!staged.success() || !staged.phase_timings)
          fail(DualCsrTraceFailureCode::kStageRejected,
               "Dual-CSR update or mixed levelization failed");
        stage_timings = *staged.phase_timings;
        verified_stage.emplace(std::move(*staged.staged()));
      } else {
        DualCsrStageAttempt staged = makeStagedDualCsrAnalysisState(
            committed, std::move(verified));
        if (!staged.success())
          fail(DualCsrTraceFailureCode::kStageRejected,
               "Dual-CSR update or direct-view CPU/full timing failed");
        stage_timings = staged.timings;
        verified_stage.emplace(std::move(*staged.staged));
      }
      batch_timing.dual_csr_update_ms = stage_timings.dual_csr_update_ms;
      batch_timing.full_levelization_ms = stage_timings.full_levelization_ms;
      batch_timing.mixed_build_ms = stage_timings.mixed_build_ms;
      batch_timing.mixed_kahn_ms = stage_timings.mixed_kahn_ms;
      batch_timing.reconstruct_ms = stage_timings.reconstruct_ms;
      batch_timing.full_propagation_ms = stage_timings.full_propagation_ms;
      batch_timing.invariant_ms = stage_timings.invariant_ms;
      batch_timing.verify_ms = stage_timings.verify_ms;
      evidence.phase_trace.push_back(DualCsrReceiptPhase::kDualCsrUpdate);
      evidence.phase_trace.push_back(
          mode == LevelizationExecutionMode::kMixedStage6
              ? DualCsrReceiptPhase::kMixedLevelization
              : DualCsrReceiptPhase::kFullLevelization);
      evidence.phase_trace.push_back(
          DualCsrReceiptPhase::kFullPropagation);
      evidence.phase_trace.push_back(
          DualCsrReceiptPhase::kIntrinsicVerify);

      const auto publish_started = SteadyClock::now();
      const auto status = coordinator.publish(std::move(*verified_stage));
      batch_timing.publish_ms
          = elapsedMilliseconds(publish_started);
      if (!status.ok() || !status.published || status.poisoned)
        fail(DualCsrTraceFailureCode::kPublishRejected,
             "atomic aggregate publish rejected the verified stage");
      evidence.phase_trace.push_back(DualCsrReceiptPhase::kPublish);
      observations.push_back(observe(coordinator.committed()));
      resource_observations.push_back(
          observeResources(coordinator.committed()));
      batch_timing.generation_after
          = coordinator.committed().generation.value();
      checkedAdd(fallback_calls,
                 coordinator.committed().graph.telemetry.fallback_count,
                 "fallback");
      phase_timings.push_back(batch_timing);
    }

    if (!installed)
      fail(DualCsrTraceFailureCode::kRollbackRejected,
           "successful prefix has no cumulative installed candidate");
    requireRollback(*installed);
    installed.reset();
    evidence.phase_trace.push_back(
        DualCsrReceiptPhase::kNativeFinalRollback);
    evidence.native_final_rollback_completed = true;
  } catch (...) {
    if (installed && installed->active()
        && !DualCsrNativeCandidateCarrier::rollback(*installed))
      fail(DualCsrTraceFailureCode::kRollbackRejected,
           "failure cleanup could not restore the native baseline");
    throw;
  }

  const auto full_after
      = csr::detail::currentNativeFullExportCounters();
  const std::uint64_t targeted_baseline_after
      = detail::currentBaselineFactoryCallCount();
  const CandidateLifecycleSnapshot lifecycle_after
      = detail::currentCandidateLifecycleSnapshot();
  evidence.operation_counters = measuredCounters(
      full_before, full_after, targeted_baseline_before,
      targeted_baseline_after, lifecycle_before, lifecycle_after,
      fallback_calls);
  evidence.incremental_batch_executed = true;
  evidence.propagation_scope = DualCsrPropagationScope::kFullGraph;
  requireResourceDeclarations(resource_observations, observations,
                              evidence);
  requireTargetedCounters(evidence.operation_counters,
                          prefix_batches);

  auto exact = finalizeDualCsrExactReceipt(
      coordinator.committed(), observations.back(),
      context.baseline_reference_input, std::move(evidence));
  if (!exact.success())
    fail(DualCsrTraceFailureCode::kReceiptRejected,
         "final rollback evidence could not seal the exact receipt");
  return {
      .exact_receipt = std::move(*exact.receipt),
      .published_observations = std::move(observations),
      .envelope = {
          .baseline_factory_calls_outside_targeted = baseline_outside,
          .baseline_factory_calls_inside_targeted = counterDelta(
              targeted_baseline_before, targeted_baseline_after,
              "inside-targeted baseline factory"),
          .batch_phase_timings = std::move(phase_timings),
          .batch_resource_observations
              = std::move(resource_observations)}};
}

DualCsrTraceRunResult DualCsrTraceReplayer::replayPrefix(
    const DualCsrTraceContext& context,
    const buffer_trace::ReplayProgram& program,
    std::size_t prefix_batches)
{
  return replayPrefixWithMode(context, program, prefix_batches,
                              LevelizationExecutionMode::kFullStage5);
}

DualCsrTraceRunResult DualCsrTraceReplayer::replayMixedPrefix(
    const DualCsrTraceContext& context,
    const buffer_trace::ReplayProgram& program,
    std::size_t prefix_batches)
{
  return replayPrefixWithMode(context, program, prefix_batches,
                              LevelizationExecutionMode::kMixedStage6);
}

DualCsrTraceRunResult replayDualCsrTracePrefix(
    const DualCsrTraceContext& context,
    const buffer_trace::ReplayProgram& program,
    std::size_t prefix_batches)
{
  return DualCsrTraceReplayer::replayPrefix(
      context, program, prefix_batches);
}

DualCsrTraceRunResult replayMixedDualCsrTracePrefix(
    const DualCsrTraceContext& context,
    const buffer_trace::ReplayProgram& program,
    std::size_t prefix_batches)
{
  return DualCsrTraceReplayer::replayMixedPrefix(
      context, program, prefix_batches);
}

}  // namespace ista::timing_dual_csr
