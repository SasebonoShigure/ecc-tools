// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include "DualCsrModeRecoveryReplay.hpp"
#include "DualCsrNativeCandidateCarrierInternal.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "DualCsrStructuredBatchTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "NarrowTimingRecordReader.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using structured_support::StructuredFixture;

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

class CumulativeBatchSource final
    : public DualCsrModeRecoveryBatchSource
{
 public:
  CumulativeBatchSource(
      Database& baseline,
      const timing_edit::NativeExecutionAuthority& authority)
      : _baseline(baseline), _authority(authority)
  {
  }

  [[nodiscard]] VerifiedStructuredBatch mint(
      const CommittedDualCsrAnalysisState& committed,
      timing_edit::DesignEditPlan plan)
  {
    DualSemanticReadView committed_view(committed.graph);
    PendingStructuredBatch pending
        = DualCsrStructuredBatchPreparer::prepare(
            std::move(plan), committed.resolver, committed_view,
            committed.graph, committed.identity,
            committed.arc_group_index);

    timing_edit::NativeDatabaseCandidate candidate
        = _installed
              ? detail::DualCsrNativeCandidateCarrier::clone(
                    *_installed)
              : detail::DualCsrNativeCandidateCarrier::create(
                    _baseline);
    if (_installed) {
      require(detail::DualCsrNativeCandidateCarrier::rollback(
                  *_installed),
              "cumulative source could not roll back prior candidate");
      _installed.reset();
    }
    const auto translated
        = detail::DualCsrNativeCandidateCarrier::apply(
            candidate, pending.preparedBatch(),
            pending.stagedResolver());
    require(translated.applied(),
            "cumulative source native apply was rejected");
    _installed.emplace(
        detail::DualCsrNativeCandidateCarrier::installAndExecute(
            _authority, std::move(candidate)));
    CandidateBoundCapture capture = NarrowTimingRecordReader{}.read(
        _installed->database(), committed_view,
        committed.arc_group_index, pending.stagedResolver(),
        pending.preparedBatch(), pending.capturePlan());
    if (!capture.success()) {
      const auto error = capture.error();
      throw std::runtime_error(
          "cumulative source narrow capture was rejected at mint "
          + std::to_string(_mint_count) + " with error "
          + (error ? std::to_string(static_cast<unsigned>(*error))
                   : std::string{"missing"}));
    }
    ++_mint_count;
    return TimingEditDualCsrDescriptorBuilder::build(
        std::move(pending), std::move(capture));
  }

  [[nodiscard]] DualCsrModeRecoveryBatchAttempt rebuild(
      const DualCsrModeRecoveryBatchRequest& request) override
  {
    DualCsrModeRecoveryBatchAttempt attempt;
    try {
      if (request.persisted_execution.generation().value()
              != request.batch_index + 1
          || request.persisted_semantic_batch.expected_generation.value()
                 != request.batch_index) {
        attempt.detail
            = "persisted recovery request is not generation-contiguous";
        return attempt;
      }
      VerifiedStructuredBatch verified = mint(
          request.exact_recovered_before,
          request.persisted_semantic_batch);
      if (verified.descriptor() != request.persisted_descriptor) {
        attempt.detail
            = "rebuilt descriptor differs from typed-rebound persistence";
        return attempt;
      }
      attempt.verified_batch.emplace(std::move(verified));
      return attempt;
    } catch (const std::exception& error) {
      attempt.detail = error.what();
      return attempt;
    }
  }

  [[nodiscard]] bool finish() noexcept
  {
    if (!_installed)
      return true;
    const bool rolled_back
        = detail::DualCsrNativeCandidateCarrier::rollback(*_installed);
    _installed.reset();
    return rolled_back;
  }

 private:
  Database& _baseline;
  const timing_edit::NativeExecutionAuthority& _authority;
  std::optional<timing_edit::InstalledNativeDatabase> _installed;
  std::size_t _mint_count{0};
};

DualCsrVerifiedStage stage(
    const CommittedDualCsrAnalysisState& committed,
    VerifiedStructuredBatch verified, LevelizationExecutionMode mode)
{
  if (mode == LevelizationExecutionMode::kFullStage5) {
    auto attempt = makeStagedDualCsrAnalysisState(
        committed, std::move(verified));
    require(attempt.success(), "focused full stage was rejected");
    return std::move(*attempt.staged);
  }
  auto attempt = makeMixedLevelizedDualCsrAnalysisState(
      committed, std::move(verified));
  auto* staged = std::get_if<DualCsrVerifiedStage>(&attempt);
  require(staged != nullptr, "focused mixed stage was rejected");
  return std::move(*staged);
}

struct PoisonedScenario
{
  std::unique_ptr<DualCsrTransactionCoordinator> coordinator;
  CommittedDualCsrAnalysisState poisoned_snapshot;
  std::vector<timing_edit::DesignEditPlan> plans;
};

PoisonedScenario producePoisoned(
    const std::vector<LevelizationExecutionMode>& modes,
    bool noncompact_generation_zero)
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
            for (std::size_t index = 0; index < modes.size(); ++index) {
              auto verified = source.mint(
                  coordinator->committed(), plans[index]);
              const auto status = coordinator->publish(
                  stage(coordinator->committed(), std::move(verified),
                        modes[index]),
                  index + 1 == modes.size()
                      ? DualCsrPublishFailpoint::kAfterSwap
                      : DualCsrPublishFailpoint::kNone);
              if (index + 1 == modes.size())
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

DualCsrModeRecoveryReplayEvidence recoverAndContinueMixed(
    PoisonedScenario& scenario, bool noncompact_generation_zero,
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

  DualCsrModeRecoveryReplayEvidence evidence;
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t, StaCsrSessionContext& context,
              const timing_edit::NativeExecutionAuthority& authority) {
            timing_edit::test::requireReleasedSnapshotForNativeMutation(
                context, authority.sessionToken());
            CumulativeBatchSource source(STADM.getDatabase(), authority);
            auto attempt = replayDualCsrModesForRecovery(
                scenario.poisoned_snapshot, std::move(baseline), source);
            require(attempt.success(),
                    "real mode-dispatched recovery replay failed"
                        + (attempt.failure
                               ? ": " + attempt.failure->detail
                               : std::string{}));
            evidence = attempt.evidence;
            const auto recovered = scenario.coordinator->recover(
                std::move(*attempt.recovery));
            require(recovered.ok() && recovered.published
                        && !recovered.poisoned,
                    "verified mode recovery did not clear poison");

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
  auto scenario = producePoisoned(
      {LevelizationExecutionMode::kMixedStage6}, true);
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

  auto legacy_clone = rebindRecoveredAggregateSession(
      poisoned, poisoned.graph.session_token,
      timing_edit::TimingEditSessionToken{
          poisoned.graph.session_token.value() + 100});
  require(legacy_clone.success(),
          "legacy bypass rejection fixture could not be rebound");
  ++legacy_clone.rebound->graph.telemetry.full_rebuild_count;
  const auto legacy_bypass = makeDualCsrVerifiedRecovery(
      poisoned, std::move(*legacy_clone.rebound));
  require(!legacy_bypass.success(),
          "legacy aggregate clone/rebind bypassed persisted mode replay");

  const auto evidence
      = recoverAndContinueMixed(scenario, true, true);
  require(evidence.descriptors_replayed == 1
              && evidence.full_records_replayed == 0
              && evidence.mixed_records_replayed == 1
              && evidence.recovery_replay_count == 1,
          "noncompact recovery envelope counters are wrong");
  require(scenario.coordinator->committed().generation.value() == 2
              && scenario.coordinator->committed().execution_trace.back()
                     .mode() == LevelizationExecutionMode::kMixedStage6,
          "noncompact recovery did not continue with the next mixed batch");
}

void fullThenMixedModeDispatchContract()
{
  auto scenario = producePoisoned(
      {LevelizationExecutionMode::kFullStage5,
       LevelizationExecutionMode::kMixedStage6},
      false);
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
  require(evidence.descriptors_replayed == 2
              && evidence.full_records_replayed == 1
              && evidence.mixed_records_replayed == 1
              && evidence.recovery_replay_count == 1,
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

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_edit::test::NativeTransactionRuntime runtime;
    ista::timing_dual_csr::test::mixedNoncompactRecoveryContract();
    ista::timing_dual_csr::test::fullThenMixedModeDispatchContract();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
