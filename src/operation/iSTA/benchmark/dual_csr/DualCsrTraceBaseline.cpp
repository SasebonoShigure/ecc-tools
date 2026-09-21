// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "Database.hpp"
#include "DualCsrArcGroupIndex.hpp"
#include "DualCsrArcGroupResolverValidation.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrReplayTypes.hpp"
#include "DualCsrSemanticIdentity.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrTraceBaselineInternal.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

[[noreturn]] void baselineFail(std::string message)
{
  throw DualCsrTraceRuntimeError(
      DualCsrTraceFailureCode::kInvalidBaseline,
      "Dual-CSR trace baseline: " + std::move(message));
}

}  // namespace

CommittedDualCsrAnalysisState makeTraceBaseline(
    const Database& database,
    const csr::CsrReferenceInput& reference_input,
    timing_edit::TimingEditSessionToken session_token)
{
  if (session_token.value() == 0
      || reference_input.source_identity.session_token
             != session_token.value()
      || reference_input.source_identity.graph_generation != 0)
    baselineFail("reference input is not a generation-zero session value");

  timing_edit::TimingEditResolverState resolver
      = timing_edit::TimingEditResolverState::bindBaseline(database);
  DualCsrBaselineAttempt baseline = DualCsrBaselineFactory::create(
      reference_input, session_token, resolver.highWater());
  if (!baseline.success())
    baselineFail("baseline factory rejected the full reference input");
  DualCsrState graph = std::move(*baseline.state);

  DualCsrArcGroupBindingAttempt bindings
      = captureFullDualCsrArcGroupBindings(resolver);
  if (!bindings.success())
    baselineFail("resolver cannot form complete generation-zero bindings");
  DualCsrArcGroupIndexAttempt index_attempt
      = buildDualCsrArcGroupIndex(graph, bindings.bindings);
  if (!index_attempt.success())
    baselineFail("resolver bindings cannot form the baseline arc index");

  csr::CsrCpuReferenceBackend backend;
  DualCsrEdgeViewImpl edges(graph);
  DualCsrTimingPayloadViewImpl payloads(graph);
  auto computation = backend.computeWithTelemetry(edges, payloads);
  auto* values = std::get_if<csr::CsrTimingResult>(
      &computation.computation.outcome);
  if (values == nullptr)
    baselineFail("direct-view CPU/full timing failed");
  DualCsrTimingResult timing{
      .values = std::move(*values)};
  graph.telemetry.traversal = computation.telemetry;

  timing_edit::CommittedEditIdentity identity
      = deriveDualCsrCommittedIdentity(graph, resolver);
  auto semantic_baseline = std::make_shared<const timing_edit::BaselineSemanticIdentity>(
      timing_edit::BaselineSemanticIdentity{database.get_design_name(), identity.current_fingerprints, identity.canonical_pre_state});
  CommittedDualCsrAnalysisState committed{
      .graph = std::move(graph),
      .timing = std::move(timing),
      .resolver = std::move(resolver),
      .arc_group_index = std::move(*index_attempt.index),
      .identity = std::move(identity),
      .generation = timing_edit::TransactionGeneration{0},
      .descriptor_trace = {},
      .semantic_trace = {},
      .traversal_telemetry = computation.telemetry,
      .baseline = std::move(semantic_baseline)};
  if (incregpusta_debug_checks_enabled && !validateCommittedDualCsrAnalysisState(committed).empty())
    baselineFail("generation-zero aggregate failed intrinsic validation");
  return committed;
}

}  // namespace ista::timing_dual_csr::detail
