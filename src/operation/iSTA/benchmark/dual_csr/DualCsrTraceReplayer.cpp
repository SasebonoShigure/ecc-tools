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
#include "CpuIncrementalLevelizationRunner.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "DualCsrBaselineFactoryInstrumentationInternal.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "CpuDualCsrBaseline.hpp"
#include "DualCsrTransactionCoordinator.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"
#include "StaProductionLifecycle.hpp"
#include "SemanticCsrNativeView.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DelayCalculator.hpp"
#include "TimingAnalyzer.hpp"

namespace ista::timing_dual_csr {
namespace {

using SteadyClock = std::chrono::steady_clock;

double elapsedMilliseconds(SteadyClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(
             SteadyClock::now() - start)
      .count();
}

[[noreturn]] void fail(DualCsrTraceFailureCode code,
                       std::string message)
{
  throw DualCsrTraceRuntimeError(
      code, "Dual-CSR targeted trace: " + std::move(message));
}

const std::vector<timing_edit::EditBatch>& batches(
    const buffer_trace::ReplayProgram& program)
{
  return std::visit(
      [](const auto& workload)
          -> const std::vector<timing_edit::EditBatch>& {
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

DualCsrOperationCounters measuredCounters(
    const csr::detail::NativeFullExportCounters& full_before,
    const csr::detail::NativeFullExportCounters& full_after,
    std::uint64_t baseline_before,
    std::uint64_t baseline_after,
    std::uint64_t fallback_calls)
{
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
.fallback_calls = fallback_calls};
}

void requireTargetedCounters(const DualCsrOperationCounters& counters)
{
  if (counters.native_full_graph_exports != 0
      || counters.native_full_reference_exports != 0
      || counters.host_snapshot_rebuilds != 0
      || counters.baseline_factory_calls != 0
      || counters.fallback_calls != 0)
    fail(DualCsrTraceFailureCode::kForbiddenOperationObserved,
         "targeted interval observed a full, baseline, or fallback path");

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
        structural.aux_out.rows_extent,
        structural.aux_out.source_extent,
        structural.aux_edge_extent,
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
  const auto setup_started = SteadyClock::now();
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

  const std::uint64_t baseline_factory_before = incregpusta_debug_checks_enabled ? detail::currentBaselineFactoryCallCount() : 0;
  CommittedDualCsrAnalysisState baseline = [&] {
    try {
      return makeCpuDualCsrBaseline(context.baseline_database, context.baseline_reference_input, session_token);
    } catch (const std::invalid_argument& error) {
      fail(DualCsrTraceFailureCode::kInvalidBaseline, error.what());
    }
  }();
  const std::uint64_t targeted_baseline_before = incregpusta_debug_checks_enabled ? detail::currentBaselineFactoryCallCount() : 0;
  const std::uint64_t baseline_outside = counterDelta(
      baseline_factory_before, targeted_baseline_before,
      "outside-targeted baseline factory");
  if (incregpusta_debug_checks_enabled && baseline_outside != 1)
    fail(DualCsrTraceFailureCode::kInvalidBaseline,
         "generation-zero baseline did not call its factory exactly once");

  const auto full_before = incregpusta_debug_checks_enabled ? csr::detail::currentNativeFullExportCounters() : csr::detail::NativeFullExportCounters{};
  DualCsrTransactionCoordinator coordinator(std::move(baseline));
  SemanticCsrNativeView native_view(session_token);
  std::vector<DualCsrPublishedObservation> observations;
  if constexpr (incregpusta_debug_checks_enabled)
    observations.reserve(prefix_batches);
  std::vector<DualCsrBatchPhaseTimings> phase_timings;
  phase_timings.reserve(prefix_batches);
  std::vector<DualCsrResourceObservation> resource_observations;
  if constexpr (incregpusta_debug_checks_enabled)
    resource_observations.reserve(prefix_batches);
  DualCsrTransactionEvidence evidence;
  std::uint64_t fallback_calls = 0;

  const double backend_setup_ms = elapsedMilliseconds(setup_started);
  for (std::size_t index = 0; index < prefix_batches; ++index) {
      const auto& committed = coordinator.committed();
      DualCsrBatchPhaseTimings batch_timing{
          .generation_before = committed.generation.value()};
      if (trace_batches[index].expected_generation
              != committed.generation)
        fail(DualCsrTraceFailureCode::kInvalidProgram,
             "semantic batch generation is not contiguous");
      const auto prepare_started = SteadyClock::now();
      PendingStructuredBatch pending
          = coordinator.prepareBatch(trace_batches[index]);
      batch_timing.prepare_batch_ms
          = elapsedMilliseconds(prepare_started);
      if constexpr (incregpusta_debug_checks_enabled)
        evidence.phase_trace.push_back(DualCsrReceiptPhase::kPrepareBatch);

      const auto direct_started = SteadyClock::now();
      VerifiedStructuredBatch verified = TimingEditDualCsrDescriptorBuilder::buildDirect(
          std::move(pending), context.authority, context.models);
      batch_timing.direct_prepare_ms = elapsedMilliseconds(direct_started);
      if constexpr (incregpusta_debug_checks_enabled)
        evidence.phase_trace.push_back(DualCsrReceiptPhase::kDirectPrepare);

      std::optional<DualCsrVerifiedStage> verified_stage;
      DualCsrStageTimings stage_timings;
      if (mode == LevelizationExecutionMode::kMixedStage6) {
        auto staged = CpuIncrementalLevelizationRunner::run(
            coordinator, std::move(verified));
        if (!staged.success() || !staged.phase_timings)
          fail(DualCsrTraceFailureCode::kStageRejected,
               "Dual-CSR update or mixed levelization failed");
        stage_timings = *staged.phase_timings;
        verified_stage.emplace(std::move(*staged.staged()));
      } else {
        DualCsrStageAttempt staged = makeStagedDualCsrAnalysisState(
            coordinator, std::move(verified));
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
      if constexpr (incregpusta_debug_checks_enabled)
        evidence.phase_trace.push_back(DualCsrReceiptPhase::kDualCsrUpdate);
      if constexpr (incregpusta_debug_checks_enabled)
        evidence.phase_trace.push_back(mode == LevelizationExecutionMode::kMixedStage6 ? DualCsrReceiptPhase::kMixedLevelization
                                                                                       : DualCsrReceiptPhase::kFullLevelization);
      if constexpr (incregpusta_debug_checks_enabled)
        evidence.phase_trace.push_back(DualCsrReceiptPhase::kFullPropagation);
      if constexpr (incregpusta_debug_checks_enabled)
        evidence.phase_trace.push_back(DualCsrReceiptPhase::kIntrinsicVerify);

      const auto publish_started = SteadyClock::now();
      const auto status = coordinator.publish(std::move(*verified_stage));
      batch_timing.publish_ms
          = elapsedMilliseconds(publish_started);
      if (!status.ok() || !status.published || status.poisoned)
        fail(DualCsrTraceFailureCode::kPublishRejected,
             "atomic aggregate publish rejected the verified stage");
      batch_timing.session_step_ms = elapsedMilliseconds(prepare_started);
      // Explicit consumers retain per-batch results; ordinary replay materializes only its final prefix.
      if (context.consume_results || index + 1 == prefix_batches) {
        const auto& current = coordinator.committed();
        const auto materialize_started = SteadyClock::now();
        native_view.materialize(context.baseline_database, context.authority, current.graph, current.resolver,
                                std::span(trace_batches).first(index + 1), current.timing.values);
        batch_timing.query_materialization_ms = elapsedMilliseconds(materialize_started);
        const auto report_started = SteadyClock::now();
        const auto report = executeStaLifecyclePipeline(
            {{"TimingAnalyzer", [] { TimingAnalyzer::initInst(); },
              [] { STATA.analyzePropagatedTiming(); }, [] { TimingAnalyzer::destroyInst(); }}},
            StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
        if (report.cleanup_failed)
          context.authority.poison();
        if (!report.succeeded)
          fail(DualCsrTraceFailureCode::kPublishRejected, "CPU propagated timing report failed: " + report.message);
        if (context.consume_results)
          context.consume_results();
        batch_timing.report_ms = elapsedMilliseconds(report_started);
        batch_timing.results_consumed = true;
      }
      batch_timing.consumer_step_ms = elapsedMilliseconds(prepare_started);
      if constexpr (incregpusta_debug_checks_enabled)
        evidence.phase_trace.push_back(DualCsrReceiptPhase::kPublish);
      if constexpr (incregpusta_debug_checks_enabled)
        observations.push_back(observe(coordinator.committed()));
      if constexpr (incregpusta_debug_checks_enabled)
        resource_observations.push_back(observeResources(coordinator.committed()));
      batch_timing.generation_after
          = coordinator.committed().generation.value();
      checkedAdd(fallback_calls,
                 coordinator.committed().graph.telemetry.fallback_count,
                 "fallback");
      phase_timings.push_back(batch_timing);
    }

  const auto& committed_result = coordinator.committed();
  const DualCsrWorkloadResult actual{.input_node_slots = context.baseline_reference_input.nodes.size(),
                                     .input_arc_slots = context.baseline_reference_input.arcs.size(),
                                     .requested_mode = mode,
                                     .generation = committed_result.generation.value(),
                                     .node_slots = committed_result.graph.nodes.size(),
                                     .arc_slots = committed_result.graph.arc_slots.size(),
                                     .fallback_count = fallback_calls,
                                     .executed_mode = committed_result.execution_trace.back().mode()};
  if constexpr (!incregpusta_debug_checks_enabled) {
    return {.transaction_receipt = std::nullopt,
            .result = actual,
            .published_observations = {},
            .envelope = {.backend_setup_ms = backend_setup_ms, .batch_phase_timings = std::move(phase_timings), .batch_resource_observations = {}}};
  }

  const auto full_after = incregpusta_debug_checks_enabled ? csr::detail::currentNativeFullExportCounters() : csr::detail::NativeFullExportCounters{};
  const std::uint64_t targeted_baseline_after = incregpusta_debug_checks_enabled ? detail::currentBaselineFactoryCallCount() : 0;
  evidence.operation_counters = measuredCounters(
      full_before, full_after, targeted_baseline_before,
      targeted_baseline_after, fallback_calls);
  evidence.incremental_batch_executed = true;
  evidence.propagation_scope = DualCsrPropagationScope::kFullGraph;
  requireResourceDeclarations(resource_observations, observations,
                              evidence);
  requireTargetedCounters(evidence.operation_counters);

  auto exact = finalizeDualCsrExactReceipt(
      coordinator.committed(), std::move(observations.back()),
      context.baseline_reference_input, std::move(evidence));
  if (!exact.success())
    fail(DualCsrTraceFailureCode::kReceiptRejected,
         "committed execution evidence could not seal the exact receipt");
  observations.back() = std::move(exact.receipt->published);
  return {.transaction_receipt = std::move(exact.receipt->transaction),
          .result = actual,
          .published_observations = std::move(observations),
          .envelope
          = {.backend_setup_ms = backend_setup_ms,
             .baseline_factory_calls_outside_targeted = baseline_outside,
             .baseline_factory_calls_inside_targeted = counterDelta(targeted_baseline_before, targeted_baseline_after, "inside-targeted baseline factory"),
             .batch_phase_timings = std::move(phase_timings),
             .batch_resource_observations = std::move(resource_observations)}};
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
