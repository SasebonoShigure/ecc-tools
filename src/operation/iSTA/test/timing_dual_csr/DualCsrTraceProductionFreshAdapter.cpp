// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTraceProductionFreshAdapter.hpp"

#include <stdexcept>
#include <utility>

#include "BufferTraceReplayer.hpp"
#include "DualCsrBaselineFactory.hpp"

namespace ista::timing_dual_csr::test {
namespace {

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error("Stage 5 fresh semantic projection: "
                           + std::move(message));
}

FunctionalSemanticReceipt normalizeFresh(
    csr::CsrReferenceInput input, csr::CsrTimingResult timing,
    timing_edit::AppendOnlyHighWater high_water)
{
  if (input.source_identity.session_token == 0
      || input.source_identity != timing.source_identity) {
    fail("fresh input and timing do not share one nonzero source identity");
  }

  // graph_generation is deliberately excluded from FunctionalSemanticReceipt.
  // The Stage 2 full exporter currently reports generation zero even after a
  // cumulative Stage 4 replay. Normalize both copies together so the baseline
  // factory remains a strict shape/payload validator if that diagnostic field
  // later becomes generation-aware.
  input.source_identity.graph_generation = 0;
  timing.source_identity.graph_generation = 0;
  high_water.node_slots = input.nodes.size();
  high_water.arc_slots = input.arcs.size();

  const timing_edit::TimingEditSessionToken token{
      input.source_identity.session_token};
  DualCsrBaselineAttempt baseline
      = DualCsrBaselineFactory::create(input, token, high_water);
  if (!baseline.success()) {
    std::string message = "flat fresh input cannot form a validated all-active state";
    for (const auto& error : baseline.violations)
      message += "\n" + error.field + ": " + error.message;
    fail(std::move(message));
  }
  DualCsrTimingResult projected_timing{
      .values = std::move(timing),
      .active_arc_mask = std::vector<std::uint8_t>(input.arcs.size(), 1U)};
  FunctionalSemanticReceiptAttempt projected
      = projectDualCsrFunctionalSemanticReceipt(
          *baseline.state, projected_timing);
  if (!projected.success()) {
    std::string message = "fresh CPU/full result cannot form a semantic receipt";
    for (const auto& error : projected.violations)
      message += "\n" + error.field + ": " + error.message;
    fail(std::move(message));
  }
  return std::move(*projected.receipt);
}

}  // namespace

ProductionFreshSemanticPrefix runProductionFreshSemanticPrefix(
    const buffer_trace::ReplayProgram& program, std::size_t prefix_batches)
{
  buffer_trace::BufferTraceProductionFreshAdapter adapter;
  buffer_trace::ReplayReceipt replay = adapter.withFreshBaseline(
      program, [&](buffer_trace::BufferReplayContext& context) {
        return buffer_trace::replayTracePrefix(context, program,
                                               prefix_batches);
      });
  if (replay.prefix_batches != prefix_batches
      || replay.batches.size() != prefix_batches
      || replay.cumulative_trace.batches.size() != prefix_batches
      || replay.committed_identity.generation.value() != prefix_batches) {
    fail("Stage 4 replay returned the wrong cumulative prefix");
  }

  ProductionFreshSemanticPrefix result{
      .functional = normalizeFresh(
          std::move(replay.final_csr_input),
          std::move(replay.final_csr_timing),
          replay.resolver_state.highWater()),
      .source_isolation = adapter.sourceIsolationReceipt(),
      .loader_invocation_count = adapter.loaderInvocationCount(),
      .guarded_work_invocation_count = adapter.guardedWorkInvocationCount()};
  if (result.loader_invocation_count != 1
      || result.guarded_work_invocation_count != 1
      || adapter.oneShotState()
             != buffer_trace::OneShotProcessState::kFinished) {
    fail("fresh semantic process did not complete exactly one guarded load");
  }
  return result;
}

}  // namespace ista::timing_dual_csr::test
