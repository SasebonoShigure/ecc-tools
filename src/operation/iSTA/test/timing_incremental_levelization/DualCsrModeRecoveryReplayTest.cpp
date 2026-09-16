// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "DataManager.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrFunctionalReceiptJson.hpp"
#include "DualCsrModeRecoveryReplay.hpp"
#include "DualCsrModeRecoveryTestSupport.hpp"
#include "DualCsrNativeCandidateCarrierInternal.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "DualCsrStructuredBatchTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrUpdater.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NarrowTimingRecordReader.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using ::ista::timing_dual_csr::detail::DualCsrNativeCandidateCarrier;
using structured_support::StructuredFixture;
using recovery_support::CumulativeBatchSource;
using recovery_support::stage;

template <typename T>
concept HasWallClockMember = requires(T value) {
  value.wall_clock_ms;
};

static_assert(!HasWallClockMember<DualCsrModeRecoveryReplayEvidence>);

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<std::uint8_t> activeMask(const DualCsrState& graph)
{
  std::vector<std::uint8_t> result;
  result.reserve(graph.arc_slots.size());
  for (const auto& slot : graph.arc_slots)
    result.push_back(slot.active ? 1U : 0U);
  return result;
}

DualCsrTimingResult timingFor(const DualCsrState& graph)
{
  DualCsrEdgeViewImpl edges(graph);
  DualCsrTimingPayloadViewImpl payloads(graph);
  csr::CsrCpuReferenceBackend backend;
  auto output = backend.computeWithTelemetry(edges, payloads);
  auto* values = std::get_if<csr::CsrTimingResult>(
      &output.computation.outcome);
  require(values != nullptr, "focused baseline timing failed");
  DualCsrTimingResult timing{std::move(*values), activeMask(graph)};
  require(validateDualCsrTimingResult(graph, timing).empty(),
          "focused baseline timing is invalid");
  return timing;
}

CommittedDualCsrAnalysisState initialAggregate(
    const StructuredFixture& fixture)
{
  CommittedDualCsrAnalysisState state{
      .graph = fixture.graph,
      .timing = timingFor(fixture.graph),
      .resolver = fixture.resolver,
      .arc_group_index = fixture.arc_group_index,
      .identity = fixture.identity,
      .generation = timing_edit::TransactionGeneration{0},
      .descriptor_trace = {},
      .semantic_trace = {},
      .traversal_telemetry = fixture.graph.telemetry.traversal,
      .execution_trace = {}};
  require(validateCommittedDualCsrAnalysisState(state).empty(),
          "focused generation-zero aggregate is invalid");
  return state;
}

void makeLegallyNoncompact(CommittedDualCsrAnalysisState& state)
{
  auto& values = state.timing.values;
  bool split = false;
  for (std::size_t level = 0;
       level + 1 < values.level_offsets.size(); ++level) {
    const std::uint64_t begin = values.level_offsets[level];
    const std::uint64_t end = values.level_offsets[level + 1];
    if (end - begin <= 1)
      continue;
    values.level_offsets.insert(
        values.level_offsets.begin()
            + static_cast<std::ptrdiff_t>(level + 1),
        begin + 1);
    split = true;
    break;
  }
  require(split, "fixture has no level that can be legally split");
  for (std::size_t level = 0;
       level + 1 < values.level_offsets.size(); ++level)
    for (std::uint64_t position = values.level_offsets[level];
         position < values.level_offsets[level + 1]; ++position)
      values.node_to_level.at(values.level_nodes.at(position).value())
          = level + 1;
  require(validateCommittedDualCsrAnalysisState(state).empty(),
          "split noncompact levelization is not legal");
}

std::size_t fullLevelCount(const DualCsrState& graph)
{
  DualCsrEdgeViewImpl edges(graph);
  DualCsrTimingPayloadViewImpl payloads(graph);
  csr::CsrCpuReferenceBackend backend;
  auto output = backend.computeWithTelemetry(edges, payloads);
  const auto* values = std::get_if<csr::CsrTimingResult>(
      &output.computation.outcome);
  require(values != nullptr && !values->level_offsets.empty(),
          "full compact oracle failed");
  return values->level_offsets.size() - 1;
}

std::vector<timing_edit::DesignEditPlan> chainedPlans(
    const StructuredFixture& fixture, std::size_t count)
{
  require(count > 0, "focused replay needs at least one plan");
  require(count <= 3, "focused replay fixture supports three plans");
  require(fixture.unmoved_fanout_arc.has_value(),
          "focused replay fixture needs an untouched fanout sink");
  std::vector<timing_edit::DesignEditPlan> plans;
  plans.reserve(count);
  plans.push_back(fixture.plan);
  const timing_edit::StableNodeRef second_sink
      = plans.front().edits.front().moved_sinks.back();
  const timing_edit::StableNodeRef untouched_sink
      = timing_edit::ExistingNodeRef{
          fixture.graph.arc_slots
              .at(fixture.unmoved_fanout_arc->value())
              .payload.key.base.sink};
  timing_edit::CreationToken prior
      = plans.front().edits.front().creation_token;
  for (std::size_t index = 1; index < count; ++index) {
    auto plan = fixture.plan;
    plan.expected_generation
        = timing_edit::TransactionGeneration{index};
    auto& edit = plan.edits.front();
    if (index == 2) {
      edit.original_net
          = fixture.plan.edits.front().original_net;
      edit.moved_sinks = {untouched_sink};
    } else {
      edit.original_net = timing_edit::CreatedNetRef{
          prior, timing_edit::CreatedNetRole::kOutputNet};
      edit.moved_sinks = {second_sink};
    }
    edit.creation_token = timing_edit::test::editTestCreationToken(
        index + 1, 0);
    prior = edit.creation_token;
    plans.push_back(std::move(plan));
  }
  return plans;
}

struct PoisonedScenario
{
  std::unique_ptr<DualCsrTransactionCoordinator> coordinator;
  CommittedDualCsrAnalysisState poisoned_snapshot;
  std::vector<timing_edit::DesignEditPlan> plans;
};

PoisonedScenario produceCommittedScenario(const std::vector<LevelizationExecutionMode>& modes, bool noncompact_generation_zero, bool poison_after_commit = true)
{
  timing_edit::test::installNativeTransactionBaseline();
  timing_edit::test::GuardedTestSession session;
  StructuredFixture fixture
      = structured_support::makeStructuredFanoutFixture(
          STADM.getDatabase(), session.token());
  CommittedDualCsrAnalysisState baseline = initialAggregate(fixture);
  if (noncompact_generation_zero)
    makeLegallyNoncompact(baseline);
  auto plans = chainedPlans(fixture, modes.size() + 1);
  auto coordinator
      = std::make_unique<DualCsrTransactionCoordinator>(baseline);

  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t, StaCsrSessionContext& context,
              const timing_edit::NativeExecutionAuthority& authority) {
            timing_edit::test::requireReleasedSnapshotForNativeMutation(
                context, authority.sessionToken());
            CumulativeBatchSource source(STADM.getDatabase(), authority);
            auto wrong_generation = plans.front();
            wrong_generation.expected_generation = timing_edit::TransactionGeneration{1};
            const auto* before_nodes = coordinator->committed().graph.nodes.data();
            bool rejected = false;
            try {
              static_cast<void>(source.mint(coordinator->committed(), std::move(wrong_generation)));
            } catch (const std::invalid_argument&) {
              rejected = true;
            }
            require(rejected && coordinator->committed().generation.value() == 0 && coordinator->committed().graph.nodes.data() == before_nodes,
                    "structured prepare accepted a foreign generation or changed committed state");
            for (std::size_t index = 0; index < modes.size(); ++index) {
              auto verified = source.mint(
                  coordinator->committed(), plans[index]);
              const auto status = coordinator->publish(
                  stage(coordinator->committed(), std::move(verified), modes[index]),
                  poison_after_commit && index + 1 == modes.size() ? DualCsrPublishFailpoint::kAfterSwap : DualCsrPublishFailpoint::kNone);
              if (poison_after_commit && index + 1 == modes.size())
                require(status.code
                            == DualCsrTransactionCode::kPostSwapPoisoned
                            && status.published && status.poisoned,
                        "focused final publish did not poison after swap");
              else
                require(status.ok() && status.published
                            && !status.poisoned,
                        "focused prefix publish failed");
            }
            require(source.finish(),
                    "focused original candidate rollback failed");
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok(), "focused original guarded replay failed: "
                            + guarded.message);
  require(session.closeAndRelease().ok(),
          "focused original session close failed");
  return {std::move(coordinator), {}, std::move(plans)};
}

void finalizePoisonedSnapshot(PoisonedScenario& scenario)
{
  scenario.poisoned_snapshot = scenario.coordinator->committed();
}

class RejectingRecoveryBatchSource final : public DualCsrModeRecoveryBatchSource
{
 public:
  DualCsrModeRecoveryBatchAttempt rebuild(const DualCsrModeRecoveryBatchRequest&) override
  {
    return {.verified_batch = std::nullopt, .violations = {}, .detail = "recovery source rejected the batch"};
  }
};

std::optional<DualCsrModeRecoveryReplayEvidence> recoverAndContinueMixed(PoisonedScenario& scenario, bool noncompact_generation_zero,
                                                                         bool continue_with_next_mixed)
{
  timing_edit::test::installNativeTransactionBaseline();
  timing_edit::test::GuardedTestSession session;
  StructuredFixture fixture
      = structured_support::makeStructuredFanoutFixture(
          STADM.getDatabase(), session.token());
  CommittedDualCsrAnalysisState baseline = initialAggregate(fixture);
  auto continuation_plans = chainedPlans(
      fixture, scenario.plans.size());
  if (noncompact_generation_zero)
    makeLegallyNoncompact(baseline);
  require(baseline.graph.session_token
              != scenario.poisoned_snapshot.graph.session_token,
          "fresh recovery session token did not change");

  std::optional<DualCsrModeRecoveryReplayEvidence> evidence;
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t, StaCsrSessionContext& context,
              const timing_edit::NativeExecutionAuthority& authority) {
            timing_edit::test::requireReleasedSnapshotForNativeMutation(
                context, authority.sessionToken());
            CumulativeBatchSource source(STADM.getDatabase(), authority);
            RejectingRecoveryBatchSource rejecting_source;
            const auto* original_nodes = scenario.coordinator->committed().graph.nodes.data();
            const auto original_generation = scenario.coordinator->committed().generation;
            const bool originally_poisoned = scenario.coordinator->poisoned();
            const auto rejected = replayDualCsrModesForRecovery(scenario.poisoned_snapshot, baseline, rejecting_source);
            require(
                !rejected.success() && !rejected.recovery && rejected.failure && rejected.failure->code == DualCsrModeRecoveryReplayCode::kBatchSourceRejected,
                "rejected recovery source minted a commit capability");
            require(rejected.evidence.has_value() == incregpusta_debug_checks_enabled, "failed recovery audit presence does not match build mode");
            require(scenario.coordinator->committed().graph.nodes.data() == original_nodes
                        && scenario.coordinator->committed().generation == original_generation && scenario.coordinator->poisoned() == originally_poisoned,
                    "failed recovery changed the committed state");
            auto attempt = replayDualCsrModesForRecovery(
                scenario.poisoned_snapshot, std::move(baseline), source);
            require(attempt.success(),
                    "real mode-dispatched recovery replay failed"
                        + (attempt.failure
                               ? ": " + attempt.failure->detail
                               : std::string{}));
            evidence = attempt.evidence;
            // Successful publication must not depend on retaining diagnostic output.
            attempt.evidence.reset();
            require(evidence.has_value() == incregpusta_debug_checks_enabled, "recovery audit presence does not match build mode");
            if (scenario.coordinator->poisoned()) {
              const auto* recovered_nodes = attempt.recovery->state().graph.nodes.data();
              const auto recovered = scenario.coordinator->recover(std::move(*attempt.recovery));
              require(recovered.ok() && recovered.published && !recovered.poisoned, "verified mode recovery did not clear poison");
              require(scenario.coordinator->committed().graph.nodes.data() == recovered_nodes, "recovery publication copied the recovered graph");
            } else {
              // Restore persisted work after closing its session; no fault injection.
              scenario.coordinator = std::make_unique<DualCsrTransactionCoordinator>(attempt.recovery->state());
            }

            if (continue_with_next_mixed) {
            const std::size_t next
                = static_cast<std::size_t>(
                    scenario.coordinator->committed().generation.value());
            require(next < continuation_plans.size(),
                    "focused continuation plan is missing");
            auto verified = source.mint(
                scenario.coordinator->committed(),
                continuation_plans[next]);
            const auto continued = scenario.coordinator->publish(
                stage(scenario.coordinator->committed(),
                      std::move(verified),
                      LevelizationExecutionMode::kMixedStage6));
            require(continued.ok() && continued.published
                        && !continued.poisoned,
                    "mixed continuation after recovery failed");
            }
            require(source.finish(),
                    "focused recovery candidate rollback failed");
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok(), "focused recovery guarded replay failed: "
                            + guarded.message);
  require(session.closeAndRelease().ok(),
          "focused recovery session close failed");
  return evidence;
}

void mixedNoncompactRecoveryContract()
{
  auto scenario = produceCommittedScenario({LevelizationExecutionMode::kMixedStage6}, true);
  finalizePoisonedSnapshot(scenario);
  const auto& poisoned = scenario.poisoned_snapshot;
  require(poisoned.execution_trace.size() == 1
              && poisoned.execution_trace.front().mode()
                     == LevelizationExecutionMode::kMixedStage6
              && poisoned.execution_trace.front()
                     .fullLevelizerEntryDelta() == 0,
          "noncompact mixed publish lacks its real mode record");
  const std::size_t mixed_levels
      = poisoned.execution_trace.front().levels().level_offsets.size() - 1;
  require(mixed_levels > fullLevelCount(poisoned.graph),
          "focused mixed publish did not preserve legal noncompact levels");

  const auto wrong_old = rebindRecoveredAggregateSession(
      poisoned,
      timing_edit::TimingEditSessionToken{
          poisoned.graph.session_token.value() + 1},
      timing_edit::TimingEditSessionToken{
          poisoned.graph.session_token.value() + 2});
  require(!wrong_old.success(),
          "typed rebind accepted a partially matching old binding");

  const auto evidence
      = recoverAndContinueMixed(scenario, true, true);
  require(evidence && evidence->descriptors_replayed == 1 && evidence->full_records_replayed == 0 && evidence->mixed_records_replayed == 1
              && evidence->recovery_replay_count == 1,
          "noncompact recovery envelope counters are wrong");
  require(scenario.coordinator->committed().generation.value() == 2
              && scenario.coordinator->committed().execution_trace.back()
                     .mode() == LevelizationExecutionMode::kMixedStage6,
          "noncompact recovery did not continue with the next mixed batch");
}

void fullThenMixedModeDispatchContract()
{
  auto scenario = produceCommittedScenario({LevelizationExecutionMode::kFullStage5, LevelizationExecutionMode::kMixedStage6}, false);
  finalizePoisonedSnapshot(scenario);
  const auto& trace = scenario.poisoned_snapshot.execution_trace;
  require(trace.size() == 2
              && scenario.poisoned_snapshot.descriptor_trace.size() == 2
              && trace[0].mode()
                     == LevelizationExecutionMode::kFullStage5
              && trace[0].fullLevelizerEntryDelta() == 1
              && trace[1].mode()
                     == LevelizationExecutionMode::kMixedStage6
              && trace[1].fullLevelizerEntryDelta() == 0,
          "g1 full -> g2 mixed persistence is not one real record per descriptor");

  const auto evidence
      = recoverAndContinueMixed(scenario, false, false);
  require(evidence && evidence->descriptors_replayed == 2 && evidence->full_records_replayed == 1 && evidence->mixed_records_replayed == 1
              && evidence->recovery_replay_count == 1,
          "mode-dispatched recovery did not replay full then mixed exactly");
  const auto& recovered = scenario.coordinator->committed();
  require(recovered.generation.value() == 2
              && recovered.execution_trace.size() == 2
              && recovered.descriptor_trace.size() == 2
              && recovered.execution_trace.back().mode()
                     == LevelizationExecutionMode::kMixedStage6
              && recovered.execution_trace.back()
                     .fullLevelizerEntryDelta() == 0,
          "g1 full -> g2 mixed recovery did not preserve exact mode dispatch");
}

void runtimeRecoveryContract(const char* output)
{
  auto scenario = produceCommittedScenario({LevelizationExecutionMode::kFullStage5, LevelizationExecutionMode::kMixedStage6}, false, false);
  finalizePoisonedSnapshot(scenario);
  const auto& initial = scenario.poisoned_snapshot;
  require(initial.generation.value() == 2, "ordinary batches did not commit");
  require(initial.execution_trace.back().levels().level_nodes.empty() == !incregpusta_debug_checks_enabled,
          "execution diagnostic payload does not match build mode");
  require((initial.traversal_telemetry == csr::CsrTraversalTelemetry{}) == !incregpusta_debug_checks_enabled,
          "storage visit collection does not match build mode");
  const auto evidence = recoverAndContinueMixed(scenario, false, false);
  require(evidence.has_value() == incregpusta_debug_checks_enabled, "recovery audit presence does not match build mode");
  const auto& recovered_trace = scenario.coordinator->committed().execution_trace;
  require(recovered_trace.size() == 2 && recovered_trace[0].mode() == LevelizationExecutionMode::kFullStage5
              && recovered_trace[1].mode() == LevelizationExecutionMode::kMixedStage6,
          "ordinary recovery did not execute the persisted modes");
  const auto stale = DualCsrUpdater::stage(initial.graph, initial.descriptor_trace.front());
  require(!stale.success(), "default path accepted a stale descriptor");
  if constexpr (!incregpusta_debug_checks_enabled) {
    bool rejected = false;
    try {
      static_cast<void>(DualCsrUpdater::stage(initial.graph, initial.descriptor_trace.front(), {DualCsrUpdateFailpoint::kAfterDetachedClone}));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "default path silently accepted fault injection");
  }
  // The frozen fixture supports one recovery followed by its second batch.
  auto continuation = produceCommittedScenario({LevelizationExecutionMode::kMixedStage6}, false, false);
  finalizePoisonedSnapshot(continuation);
  recoverAndContinueMixed(continuation, false, true);
  require(continuation.coordinator->committed().generation.value() == 2, "ordinary recovery did not continue to the next batch");
  if (output != nullptr) {
    // Explicit development acceptance export; never called by performance probes.
    auto before = projectDualCsrFunctionalSemanticReceipt(initial.graph, initial.timing);
    const auto& current = continuation.coordinator->committed();
    const auto& recovered = scenario.coordinator->committed();
    auto replayed = projectDualCsrFunctionalSemanticReceipt(recovered.graph, recovered.timing);
    auto after = projectDualCsrFunctionalSemanticReceipt(current.graph, current.timing);
    require(before.success() && replayed.success() && after.success(), "development projection failed");
    nlohmann::json result{{"before_recovery", functionalSemanticReceiptToJson(*before.receipt)},
                          {"after_continuation", functionalSemanticReceiptToJson(*after.receipt)},
                          {"after_two_batch_replay", functionalSemanticReceiptToJson(*replayed.receipt)},
                          {"generation", current.generation.value()}};
    std::ofstream file(output);
    file << result.dump(2);
    require(static_cast<bool>(file), "could not write development comparison output");
  }
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main(int argc, char* argv[])
{
  try {
    if (argc == 5 && std::string(argv[1]) == "--replay" && std::string(argv[3]) == "--runtime-output") {
      ista::timing_dual_csr::test::realReplayRecovery(argv[2], argv[4]);
      return 0;
    }
    ista::timing_edit::test::NativeTransactionRuntime runtime;
    if (argc == 3 && std::string(argv[1]) == "--runtime-output") {
      ista::timing_dual_csr::test::runtimeRecoveryContract(argv[2]);
    } else if (argc != 1) {
      throw std::invalid_argument("usage: recovery_test [--runtime-output FILE]");
    } else if constexpr (ista::incregpusta_debug_checks_enabled) {
      ista::timing_dual_csr::test::mixedNoncompactRecoveryContract();
      ista::timing_dual_csr::test::fullThenMixedModeDispatchContract();
    } else {
      ista::timing_dual_csr::test::runtimeRecoveryContract(nullptr);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
