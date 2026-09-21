#include "CanonicalTimingRecordProjectorInternal.hpp"
// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "DataManager.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DualCsrSemanticIdentity.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "DualCsrStructuredBatchTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrTransactionCoordinator.hpp"
#include "DualSemanticReadView.hpp"
#include "Instance.hpp"
#include "NarrowTimingRecordReader.hpp"
#include "NativeDatabaseValidation.hpp"
#include "Net.hpp"
#include "Pin.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "Stage6FunctionalReceipt.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingPoint.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using structured_support::StructuredFixture;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void addParallelCellCaptureGroup(Database& database)
{
  constexpr std::string_view kInstance{"capture_peer"};
  constexpr std::string_view kSource{"capture_peer:A"};
  constexpr std::string_view kSink{"capture_peer:Y"};
  constexpr std::string_view kCell{"CAPTURE_PEER_X1"};

  TimingCell cell = database.get_timing_library()
                        .get_cell_map().at("BUF_X1");
  cell.set_cell_name(std::string{kCell});
  cell.get_cell_arc_list().push_back(
      cell.get_cell_arc_list().front());
  require(database.get_timing_library().get_cell_map()
              .emplace(std::string{kCell}, std::move(cell)).second,
          "parallel capture cell collided with the fixture library");

  Instance instance;
  instance.set_instance_id(0);
  instance.set_instance_name(std::string{kInstance});
  instance.set_cell_name(std::string{kCell});
  instance.set_pin_name_list(
      {std::string{kSource}, std::string{kSink}});
  instance.set_output_pin_name(std::string{kSink});
  require(database.get_instance_map()
              .emplace(std::string{kInstance}, std::move(instance))
              .second,
          "parallel capture instance collided with the fixture");

  Pin source;
  source.set_pin_name("A");
  source.set_full_name(std::string{kSource});
  source.set_instance_name(std::string{kInstance});
  source.set_net_name("capture_input");
  source.set_direction(PinDirection::kInput);
  source.set_is_port(false);
  Pin sink;
  sink.set_pin_name("Y");
  sink.set_full_name(std::string{kSink});
  sink.set_instance_name(std::string{kInstance});
  sink.set_net_name("n0");
  sink.set_direction(PinDirection::kOutput);
  sink.set_is_port(false);
  require(database.get_pin_map()
              .emplace(std::string{kSource}, std::move(source)).second
              && database.get_pin_map()
                     .emplace(std::string{kSink}, std::move(sink)).second,
          "parallel capture pins collided with the fixture");

  database.get_timing_point_map().emplace(
      std::string{kSource}, TimingPoint{});
  database.get_timing_point_map().emplace(
      std::string{kSink}, TimingPoint{});
  database.get_timing_point_map().at(std::string{kSource}).set_level(2);
  database.get_timing_point_map().at(std::string{kSink}).set_level(3);
  database.get_timing_point_map().at("in").set_level(1);
  database.get_timing_point_map().at("out").set_level(4);
  database.get_timing_order_list()
      = {"in", std::string{kSource}, std::string{kSink}, "out"};

  database.get_pin_map().at("in").set_net_name("capture_input");
  auto& output_net = database.get_net_map().at("n0");
  output_net.set_pin_name_list({std::string{kSink}, "out"});
  output_net.set_driver_pin(std::string{kSink});
  output_net.set_driver_pin_list({std::string{kSink}});
  output_net.set_load_pin_list({"out"});
  Net input_net;
  input_net.set_net_name("capture_input");
  input_net.set_pin_name_list({"in", std::string{kSource}});
  input_net.set_driver_pin("in");
  input_net.set_driver_pin_list({"in"});
  input_net.set_load_pin_list({std::string{kSource}});
  require(database.get_net_map()
              .emplace("capture_input", std::move(input_net)).second,
          "parallel capture input net collided with the fixture");

  Arc& original = database.get_arc_list().front();
  original.set_arc_name("n0:capture_peer:Y->out");
  original.set_source_pin(std::string{kSink});
  original.set_owner_name("n0");
  database.get_outgoing_arc_list_map().at("in").clear();
  database.get_outgoing_arc_list_map()[std::string{kSink}]
      .push_back(0);
  const auto append_arc = [&](Arc arc) {
    database.get_arc_list().push_back(std::move(arc));
    const std::size_t index = database.get_arc_list().size() - 1;
    const auto& appended = database.get_arc_list().back();
    database.get_outgoing_arc_list_map()[appended.get_source_pin()]
        .push_back(index);
    database.get_incoming_arc_list_map()[appended.get_sink_pin()]
        .push_back(index);
  };
  Arc net_arc;
  net_arc.set_arc_name("capture_input:in->capture_peer:A");
  net_arc.set_source_pin("in");
  net_arc.set_sink_pin(std::string{kSource});
  net_arc.set_owner_name("capture_input");
  net_arc.set_type(ArcType::kNet);
  append_arc(std::move(net_arc));
  auto& cell_arcs = database.get_timing_library()
                        .get_cell_map().at(std::string{kCell})
                        .get_cell_arc_list();
  for (std::size_t ordinal = 0; ordinal < cell_arcs.size(); ++ordinal) {
    Arc arc;
    arc.set_arc_name("capture_peer:A->Y#" +
                     std::to_string(ordinal));
    arc.set_source_pin(std::string{kSource});
    arc.set_sink_pin(std::string{kSink});
    arc.set_owner_name(std::string{kInstance});
    arc.set_type(ArcType::kCell);
    arc.set_library_source_port(cell_arcs[ordinal].get_source_port());
    arc.set_library_sink_port(cell_arcs[ordinal].get_sink_port());
    arc.set_timing_cell_arc(&cell_arcs[ordinal]);
    append_arc(std::move(arc));
  }
  const auto failure
      = timing_edit::validateNativeDatabase(database);
  require(!failure,
          failure ? "parallel cell capture fixture is invalid: "
                        + failure->object + ": " + failure->detail
                  : std::string{});
}

DualCsrTimingResult timingFor(const DualCsrState& graph)
{
  DualCsrEdgeViewImpl edges(graph);
  DualCsrTimingPayloadViewImpl payloads(graph);
  csr::CsrCpuReferenceBackend backend;
  auto output = backend.computeWithTelemetry(edges, payloads);
  auto* values = std::get_if<csr::CsrTimingResult>(
      &output.computation.outcome);
  require(values != nullptr,
          "active-payload microcase CPU timing failed");
  DualCsrTimingResult timing{std::move(*values)};
  require(validateDualCsrTimingResult(graph, timing).empty(),
          "active-payload microcase CPU timing is invalid");
  return timing;
}

FunctionalSemanticReceipt receiptFor(
    const DualCsrState& graph, const DualCsrTimingResult& timing)
{
  auto attempt = projectDualCsrFunctionalSemanticReceipt(graph, timing);
  require(attempt.success(),
          "active-payload functional receipt projection failed");
  return std::move(*attempt.receipt);
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
      .execution_trace = {},
      .baseline = std::make_shared<const timing_edit::BaselineSemanticIdentity>(fixture.plan.baseline)};
  require(validateCommittedDualCsrAnalysisState(state).empty(),
          "active-payload generation-zero aggregate is invalid");
  return state;
}

std::vector<timing_edit::EditBatch> twoPlans(
    const StructuredFixture& fixture)
{
  require(fixture.unmoved_fanout_arc.has_value(),
          "active-payload fixture needs an untouched fanout peer");
  std::vector<timing_edit::EditBatch> plans{{fixture.plan.expected_generation, fixture.plan.edits}};
  timing_edit::EditBatch continuation{fixture.plan.expected_generation, fixture.plan.edits};
  continuation.expected_generation
      = timing_edit::TransactionGeneration{1};
  auto& edit = continuation.edits.front();
  edit.original_net = timing_edit::CreatedNetRef{
      fixture.plan.edits.front().creation_token,
      timing_edit::CreatedNetRole::kOutputNet};
  edit.moved_sinks = {
      fixture.plan.edits.front().moved_sinks.back()};
  edit.creation_token
      = timing_edit::test::editTestCreationToken(2, 0);
  plans.push_back(std::move(continuation));
  return plans;
}

class CumulativeBatchMinter final
{
 public:
  CumulativeBatchMinter(
      const timing_edit::NativeExecutionAuthority& authority)
      : _authority(authority)
  {
  }

  [[nodiscard]] VerifiedStructuredBatch mint(
      DualCsrTransactionCoordinator& coordinator,
      timing_edit::EditBatch plan,
      bool exercise_omitted_peer = false)
  {
    const auto& committed = coordinator.committed();
    commitPending(committed.generation);
    DualSemanticReadView committed_view(committed.graph);
    PendingStructuredBatch pending
        = coordinator.prepareBatch(std::move(plan));

    _installed.emplace(timing_edit::NativeEditTransaction::prepareFull(
        _authority, pending.preparedBatch(), pending.stagedResolver()));
    _pending_generation = pending.preparedBatch().targetGeneration();
    timing_edit::NativeCandidateExecutor::executeFull(*_installed);

    if (exercise_omitted_peer) {
      auto omitted = pending.capturePlan();
      const auto leader = std::ranges::find_if(
          omitted.arcs, [](const AffectedArcCapture& request) {
            return request.expected_post_multiplicity > 1;
          });
      require(leader != omitted.arcs.end(),
              "continuation plan has no active same-base peer group");
      const auto peer = std::ranges::find_if(
          omitted.arcs, [&](const AffectedArcCapture& request) {
            return request.ref != leader->ref
                   && request.expected_post_multiplicity
                          == leader->expected_post_multiplicity
                   && request.expected_post_base
                          == leader->expected_post_base;
          });
      require(peer != omitted.arcs.end(),
              "continuation plan cannot omit a same-base peer");
      omitted.arcs.erase(peer);
      const auto committed_before = committed;
      csr::detail::NativeLibraryModels models(_installed->database());
      const CandidateBoundCapture rejected
          = NarrowTimingRecordReader{models}.read(
              _installed->database(), committed_view,
              committed.arc_group_index, pending.stagedResolver(),
              pending.preparedBatch(), omitted);
      require(rejected.error()
                  == NarrowCaptureError::kArcMultiplicityMismatch
                  && committed == committed_before,
              "omitted same-base peer was not rejected atomically at the typed multiplicity boundary");
    }
    csr::detail::NativeLibraryModels models(_installed->database());
    CandidateBoundCapture capture = NarrowTimingRecordReader{models}.read(
        _installed->database(), committed_view,
        committed.arc_group_index, pending.stagedResolver(),
        pending.preparedBatch(), pending.capturePlan());
    require(capture.success(),
            "active-payload narrow capture was rejected");
    return TimingEditDualCsrDescriptorBuilder::build(
        std::move(pending), std::move(capture));
  }

  // Recovery's coordinator is local to replay; retain only the pending
  // generation, then confirm publication through the next call or its result.
  void commitPending(timing_edit::TransactionGeneration generation)
  {
    if (!_installed)
      return;
    require(_installed->active() && generation == _pending_generation,
            "native batch has no matching committed CSR generation");
    _installed->commit();
    _installed.reset();
  }

  [[nodiscard]] bool rollback() noexcept
  {
    if (!_installed)
      return true;
    const bool restored = _installed->rollback();
    _installed.reset();
    return restored;
  }

 private:
  const timing_edit::NativeExecutionAuthority& _authority;
  std::optional<timing_edit::NativeEditTransaction> _installed;
  timing_edit::TransactionGeneration _pending_generation;
};

struct DeletedPeerCanary
{
  csr::ArcId deleted;
  csr::ArcId active_peer;
  csr::ArcKey active_key;
  csr::ArcKey inactive_key;
};

DeletedPeerCanary installDeletedPeerCanary(
    CommittedDualCsrAnalysisState& state, csr::ArcId active_peer,
    std::uint64_t inactive_ordinal)
{
  const auto deleted = std::ranges::find_if(
      state.graph.arc_slots,
      [](const DualCsrArcSlot& slot) { return !slot.active; });
  require(deleted != state.graph.arc_slots.end(),
          "generation one contains no deleted ArcId");
  const std::size_t deleted_index = static_cast<std::size_t>(
      std::distance(state.graph.arc_slots.begin(), deleted));
  const csr::ArcId deleted_id{
      static_cast<std::uint64_t>(deleted_index)};
  auto& peer = state.graph.arc_slots.at(
      static_cast<std::size_t>(active_peer.value()));
  require(peer.active,
          "untouched same-base canary peer is not active");
  require(peer.payload.key.parallel_ordinal == 0,
          "untouched active peer is not canonical ordinal zero");

  const auto identity_before = deriveDualCsrCommittedIdentity(state.graph, state.resolver);
  deleted->payload = peer.payload;
  deleted->payload.key.parallel_ordinal = inactive_ordinal;
  if (inactive_ordinal == 0)
    require(deleted->payload.key == peer.payload.key,
            "ordinal-zero inactive peer is not an exact ArcKey collision");
  require(deleted->payload.key.base == peer.payload.key.base,
          "deleted and active canary arcs do not share one base");

  const auto* index_slot = state.arc_group_index.slot(deleted_id);
  require(index_slot != nullptr
              && !index_slot->current_active_key.has_value(),
          "deleted ArcId retained an active index capability");
  const bool resolver_tombstone = std::ranges::any_of(
      state.resolver.arcBindings(), [&](const auto& item) {
        return item.second.slot.value() == deleted_id.value()
               && item.second.tombstone;
      });
  require(resolver_tombstone,
          "deleted ArcId retained an active resolver capability");

  const DualSemanticReadView view(state.graph);
  const auto inactive = view.arcById(deleted_id);
  const auto keyed = view.arcByKey(peer.payload.key);
  require(!inactive.hasValue()
              && inactive.error()
                     == DualSemanticReadError::kInactiveArc
              && keyed.hasValue()
              && &keyed.value() == &peer.payload,
          "semantic view did not isolate the deleted same-base peer");
  require(deriveDualCsrCommittedIdentity(
              state.graph, state.resolver)
              == identity_before,
          "inactive historical key changed committed semantics");
  require(validateCommittedDualCsrAnalysisState(state).empty(),
          "same-base inactive canary made the aggregate invalid");
  return {deleted_id, active_peer, peer.payload.key,
          deleted->payload.key};
}

struct ScenarioResult
{
  Stage6FunctionalReceipt functional;
  Stage6LevelizationReceipt levelization;
  std::optional<MixedLevelizationDeterministicTrace> mixed;
};

enum class FinalFactoryMode : std::uint8_t
{
  kMixed,
  kFull
};

ScenarioResult runScenario(std::uint64_t inactive_ordinal,
                           FinalFactoryMode final_mode)
{
  timing_edit::test::installNativeTransactionBaseline();
  timing_edit::test::GuardedTestSession session;
  addParallelCellCaptureGroup(STADM.getDatabase());
  StructuredFixture fixture
      = structured_support::makeStructuredFanoutFixture(
          STADM.getDatabase(), session.token());
  const auto plans = twoPlans(fixture);
  const auto initial = initialAggregate(fixture);
  std::optional<ScenarioResult> result;

  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t, StaCsrSessionContext& context,
              const timing_edit::NativeExecutionAuthority& authority) {
            timing_edit::test::requireReleasedSnapshotForNativeMutation(
                context, authority.sessionToken());
            CumulativeBatchMinter source(authority);

            DualCsrTransactionCoordinator first_commit(initial);
            auto first = source.mint(first_commit, plans.front(), true);
            auto full = makeStagedDualCsrAnalysisState(
                first_commit, std::move(first));
            require(full.success(),
                    "generation-one public full factory was rejected");
            require(first_commit.publish(std::move(*full.staged)).ok(), "generation-one publication failed");
            CommittedDualCsrAnalysisState generation_one = first_commit.committed();
            const DeletedPeerCanary canary
                = installDeletedPeerCanary(
                    generation_one, *fixture.unmoved_fanout_arc,
                    inactive_ordinal);

            DualCsrTransactionCoordinator final_commit(std::move(generation_one));
            auto second = source.mint(
                final_commit, plans.back());
            std::optional<DualCsrVerifiedStage> final_stage;
            if (final_mode == FinalFactoryMode::kMixed) {
              auto mixed_attempt
                  = makeMixedLevelizedDualCsrAnalysisState(
                      final_commit, std::move(second));
              auto* mixed_stage
                  = std::get_if<DualCsrVerifiedStage>(&mixed_attempt);
              require(mixed_stage != nullptr,
                      "generation-two public mixed factory was rejected");
              final_stage.emplace(std::move(*mixed_stage));
            } else {
              auto full_attempt = makeStagedDualCsrAnalysisState(
                  final_commit, std::move(second));
              require(full_attempt.success(),
                      "independent generation-two public full factory was rejected");
              final_stage.emplace(std::move(*full_attempt.staged));
            }
            const auto& execution = final_stage->batch().execution;
            const bool correct_mode
                = final_mode == FinalFactoryMode::kMixed
                      ? execution.mode()
                                == LevelizationExecutionMode::kMixedStage6
                            && execution.fullLevelizerEntryDelta() == 0
                            && execution.suppliedLevelsValidated()
                            && execution.mixed().has_value()
                      : execution.mode()
                                == LevelizationExecutionMode::kFullStage5
                            && execution.fullLevelizerEntryDelta() == 1
                            && !execution.suppliedLevelsValidated()
                            && !execution.mixed().has_value();
            require(correct_mode,
                    "active-payload stage lost its selected public factory boundary");
            require(final_commit.publish(std::move(*final_stage)).ok(), "generation-two publication failed");
            const auto& staged = final_commit.committed();
            const auto& deleted = staged.graph.arc_slots.at(
                static_cast<std::size_t>(canary.deleted.value()));
            const auto& peer = staged.graph.arc_slots.at(
                static_cast<std::size_t>(canary.active_peer.value()));
            require(!deleted.active && peer.active
                        && deleted.payload.key == canary.inactive_key
                        && peer.payload.key == canary.active_key
                        && deleted.payload.key.base
                               == peer.payload.key.base,
                    "mixed update changed the deleted/active peer canary");

            const auto incremental_receipt
                = receiptFor(staged.graph, staged.timing);
            const auto fresh_receipt
                = receiptFor(staged.graph, timingFor(staged.graph));
            const auto incremental = separateStage6FunctionalReceipt(
                incremental_receipt, staged.generation.value());
            const auto fresh = separateStage6FunctionalReceipt(
                fresh_receipt, staged.generation.value());
            require(incremental.functional == fresh.functional,
                    "mixed functional receipt differs from fresh full rebuild");
            require(validateStage6LevelizationReceipt(
                        incremental.functional.graph,
                        incremental.levelization,
                        staged.generation.value())
                        .empty()
                        && validateStage6LevelizationReceipt(
                               fresh.functional.graph,
                               fresh.levelization,
                               staged.generation.value())
                               .empty(),
                    "incremental or fresh levelization receipt is invalid");
            const auto visible_peer_count = std::ranges::count_if(
                incremental.functional.graph.active_arcs,
                [&](const DualCsrProjectedArc& arc) {
                  return arc.key == canary.active_key;
                });
            require(visible_peer_count == 1,
                    "functional receipt exposed the deleted same-key peer");

            result = ScenarioResult{
                incremental.functional,
                incremental.levelization,
                staged.execution_trace.back().mixed()};
            source.commitPending(final_commit.committed().generation);
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok(),
          "active-payload guarded scenario failed: " + guarded.message);
  require(session.closeAndRelease().ok(),
          "active-payload session close failed");
  require(result.has_value(),
          "active-payload scenario produced no result");
  return std::move(*result);
}

void inactiveSameBasePeerCannotInfluenceMixedStage()
{
  const ScenarioResult exact_collision
      = runScenario(0, FinalFactoryMode::kMixed);
  const ScenarioResult historical_poison
      = runScenario(97, FinalFactoryMode::kMixed);
  const ScenarioResult independent_full
      = runScenario(0, FinalFactoryMode::kFull);
  require(exact_collision.functional == historical_poison.functional,
          "inactive same-base key changed Stage 6 functional results");
  require(exact_collision.levelization
              == historical_poison.levelization,
          "inactive same-base key changed reconstructed levels");
  require(exact_collision.mixed.has_value()
              && historical_poison.mixed.has_value()
              && exact_collision.mixed == historical_poison.mixed,
          "inactive same-base key changed deterministic mixed evidence");
  require(exact_collision.functional == independent_full.functional,
          "Stage 6 mixed functional receipt differs from an independent-session public full rebuild");
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_edit::test::NativeTransactionRuntime runtime;
    ista::timing_dual_csr::test::
        inactiveSameBasePeerCannotInfluenceMixedStage();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
