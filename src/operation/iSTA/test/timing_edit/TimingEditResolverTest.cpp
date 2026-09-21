// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "Database.hpp"
#include "../AllocationFailureSupport.hpp"
#include "TimingClock.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditValidation.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Callable>
std::string requireInvalid(Callable&& callable, const std::string& message)
{
  try {
    callable();
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  throw std::runtime_error(message);
}

TimingEditPrepareContext prepareContext(const BaselineSemanticIdentity& baseline,
                                        const CommittedEditIdentity& identity,
                                        TimingEditSessionToken token)
{
  return TimingEditPrepareContext{token, baseline, identity, editTestAnalysisContext()};
}

void testBaselineArcBindingsAreOneShot()
{
  NativeEditFixture fixture = makeNativeEditFixture(true);
  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(fixture.database());
  require(resolver.arcBindings().size() == 2, "parallel baseline arcs were not bound independently");

  std::vector<std::pair<StableArcRef, AppendOnlySlot>> original;
  for (const auto& [ref, binding] : resolver.arcBindings()) {
    require(std::holds_alternative<ExistingArcRef>(ref), "baseline arc was not represented by ExistingArcRef");
    original.emplace_back(ref, binding.slot);
  }
  require(original[0].first != original[1].first && original[0].second != original[1].second,
          "parallel arcs did not receive unique identities and slots");

  std::reverse(fixture.database().get_arc_list().begin(), fixture.database().get_arc_list().end());
  std::reverse(fixture.database().get_outgoing_arc_list_map().at("in").begin(),
               fixture.database().get_outgoing_arc_list_map().at("in").end());
  std::reverse(fixture.database().get_incoming_arc_list_map().at("out").begin(),
               fixture.database().get_incoming_arc_list_map().at("out").end());
  for (const auto& [ref, slot] : original) {
    require(resolver.resolveArcSlot(ref) == slot,
            "native traversal reorder reinterpreted a baseline parallel ordinal");
  }
}

void testPreparedBatchStagesExistingAndCreatedRefsAppendOnly()
{
  constexpr TimingEditSessionToken kToken{17};
  constexpr TransactionGeneration kGeneration{0};
  NativeEditFixture fixture = makeNativeEditFixture();
  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(fixture.database());
  const CommittedEditIdentity identity = NativeTimingEditPreparer::captureCurrentIdentity(
      fixture.database(), kGeneration, editTestAnalysisContext(), kToken);
  const BaselineSemanticIdentity baseline
      = BaselineSemanticIdentity{fixture.database().get_design_name(), identity.current_fingerprints,
                                 identity.canonical_pre_state};
  const CreationToken creation_token = editTestCreationToken();
  DesignEditPlan plan = bufferInsertionPlan(fixture.database(), baseline, kGeneration, creation_token);
  PreparedBatch batch = NativeTimingEditPreparer::prepare(
      fixture.database(), plan, prepareContext(baseline, identity, kToken), resolver);

  const AppendOnlyHighWater before = resolver.highWater();
  require(batch.slotPlan().before == before, "prepared batch did not start at resolver high-water");
  require(batch.slotPlan().after.node_slots == before.node_slots + 2
              && batch.slotPlan().after.net_slots == before.net_slots + 1
              && batch.slotPlan().after.arc_slots == before.arc_slots + 3,
          "buffer insertion did not reserve its exact append-only ranges");

  const CreatedNodeRef input{creation_token, CreatedNodeRole::kInput};
  const CreatedNodeRef output{creation_token, CreatedNodeRole::kOutput};
  const CreatedNetRef output_net{creation_token, CreatedNetRole::kOutputNet};
  const TokenMaterialization materialization = defaultTokenMaterialization(creation_token, plan.edits.front().library_cell);
  const auto original = resolver;
  auto delta = resolver.prepare(batch);
  TimingEditResolverView projected(resolver, delta);
  require(projected.resolveNode(StableNodeRef{input}) == materialization.input_pin_name && resolver == original,
          "resolver projection copied changes into the committed owner");
  const auto* old_node = &resolver.nodeBindings().begin()->second;
  {
    PreparedTimingEditResolverPatch patch(resolver, std::move(delta));
    ista_test_allocation::armFailure(1);
    patch.apply();
    patch.rollback();
    const auto allocations = ista_test_allocation::disable();
    require(allocations == 0 && resolver == original && &resolver.nodeBindings().begin()->second == old_node,
            "resolver apply/undo allocated or changed an existing map node");
  }
  // A locally changed owner with the same high-water must reject the old delta.
  auto stale_delta = resolver.prepare(batch);
  const auto touched = std::ranges::find_if(batch.slotPlan().arcs, [](const auto& item) { return item.tombstone; });
  require(touched != batch.slotPlan().arcs.end(), "fixture has no changed arc");
  auto& old_arc = const_cast<NativeSlotBinding&>(resolver.arcBindings().at(touched->ref));
  old_arc.native_name = "different old value";
  requireInvalid([&] { PreparedTimingEditResolverPatch stale(resolver, std::move(stale_delta)); },
                 "resolver patch accepted a different local old value");
  old_arc.native_name.clear();
  require(resolver == original, "local stale-value canary did not restore its input");
  {
    PreparedTimingEditResolverPatch abandoned(resolver, resolver.prepare(batch));
    abandoned.apply();
  }
  require(resolver == original, "abandoned resolver patch did not undo");
  PreparedTimingEditResolverPatch patch(resolver, resolver.prepare(batch));
  patch.apply();
  patch.commit();
  const auto& staged = resolver;
  require(staged.resolveNode(StableNodeRef{input}) == materialization.input_pin_name,
          "created input ref did not resolve to canonical materialization");
  require(staged.resolveNode(StableNodeRef{output}) == materialization.output_pin_name,
          "created output ref did not resolve to canonical materialization");
  require(staged.resolveNet(StableNetRef{output_net}) == materialization.output_net_name,
          "created net ref did not resolve to canonical materialization");
  require(staged.resolveNode(StableNodeRef{ExistingNodeRef{identity.canonical_pre_state.nodes.front()}})
              == identity.canonical_pre_state.nodes.front().full_name,
          "unmodified existing node ref stopped resolving");

  std::set<std::uint64_t> appended_arc_slots;
  StableArcRef removed_arc;
  bool found_removed_arc = false;
  for (const auto& assignment : batch.slotPlan().arcs) {
    if (assignment.tombstone) {
      require(assignment.slot.value() < before.arc_slots,
              "tombstone did not retain its pre-existing slot");
      removed_arc = assignment.ref;
      found_removed_arc = true;
    } else {
      require(assignment.slot.value() >= before.arc_slots,
              "new arc reused a pre-existing slot");
      appended_arc_slots.insert(assignment.slot.value());
    }
  }
  require(found_removed_arc, "moved sink did not tombstone its old net arc");
  require(appended_arc_slots
              == std::set<std::uint64_t>{before.arc_slots, before.arc_slots + 1, before.arc_slots + 2},
          "new arcs did not occupy the full contiguous append-only range");
  requireInvalid([&] { static_cast<void>(staged.resolveArcSlot(removed_arc)); },
                 "tombstoned arc remained live");

  bool saw_created_cell = false;
  bool saw_created_old_net = false;
  bool saw_created_new_net = false;
  for (const auto& [ref, binding] : staged.arcBindings()) {
    static_cast<void>(binding);
    saw_created_cell = saw_created_cell || std::holds_alternative<CreatedCellArcRef>(ref);
    saw_created_old_net = saw_created_old_net || std::holds_alternative<CreatedOldNetArcRef>(ref);
    saw_created_new_net = saw_created_new_net || std::holds_alternative<CreatedNewNetArcRef>(ref);
  }
  require(saw_created_cell && saw_created_old_net && saw_created_new_net,
          "resolver did not preserve all created arc identity families");
}

void testPreparerRejectsResolverWithoutExactLiveCoverage()
{
  constexpr TimingEditSessionToken kToken{19};
  constexpr TransactionGeneration kGeneration{0};
  NativeEditFixture fixture = makeNativeEditFixture();
  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(fixture.database());
  const CommittedEditIdentity identity = NativeTimingEditPreparer::captureCurrentIdentity(
      fixture.database(), kGeneration, editTestAnalysisContext(), kToken);
  const BaselineSemanticIdentity baseline
      = BaselineSemanticIdentity{fixture.database().get_design_name(), identity.current_fingerprints,
                                 identity.canonical_pre_state};
  PreparedBatch first = NativeTimingEditPreparer::prepare(
      fixture.database(), bufferInsertionPlan(fixture.database(), baseline, kGeneration,
                                              editTestCreationToken()),
      prepareContext(baseline, identity, kToken), resolver);
  TimingEditResolverState resolver_ahead_of_database = resolver.stage(first);

  const std::string error = requireInvalid(
      [&] {
        static_cast<void>(NativeTimingEditPreparer::prepare(
            fixture.database(),
            bufferInsertionPlan(fixture.database(), baseline, kGeneration,
                                editTestCreationToken(1, 1)),
            prepareContext(baseline, identity, kToken), resolver_ahead_of_database));
      },
      "preparer accepted resolver state whose live objects exceed the active Database");
  require(error.find("resolver live node/net/arc cardinality") != std::string::npos,
          "live coverage rejection reported the wrong gate: " + error);
}

void testPreparerRejectsStaleGenerationAndSameGenerationTamper()
{
  constexpr TimingEditSessionToken kToken{27};
  constexpr TransactionGeneration kGeneration{0};
  NativeEditFixture fixture = makeNativeEditFixture();
  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(fixture.database());
  const CommittedEditIdentity identity = NativeTimingEditPreparer::captureCurrentIdentity(
      fixture.database(), kGeneration, editTestAnalysisContext(), kToken);
  const BaselineSemanticIdentity baseline
      = BaselineSemanticIdentity{fixture.database().get_design_name(), identity.current_fingerprints,
                                 identity.canonical_pre_state};

  const std::string stale_error = requireInvalid(
      [&] {
        static_cast<void>(NativeTimingEditPreparer::prepare(
            fixture.database(),
            bufferInsertionPlan(fixture.database(), baseline, TransactionGeneration{1},
                                editTestCreationToken(2, 0)),
            prepareContext(baseline, identity, kToken), resolver));
      },
      "stale generation was accepted by native prepare");
  require(stale_error.find("plan generation does not match the committed generation")
              != std::string::npos,
          "stale-generation rejection reported the wrong gate: " + stale_error);

  TimingClock external_clock;
  external_clock.set_clock_name("external_clock");
  external_clock.set_source_list({"in"});
  external_clock.set_period(8.0);
  external_clock.set_fall_edge(4.0);
  fixture.database().get_timing_constraint().get_clock_map().emplace(
      "external_clock", std::move(external_clock));
  const CommittedEditIdentity externally_tampered
      = NativeTimingEditPreparer::captureCurrentIdentity(
          fixture.database(), kGeneration, editTestAnalysisContext(), kToken);
  require(externally_tampered.generation == identity.generation
              && externally_tampered.canonical_pre_state == identity.canonical_pre_state
              && externally_tampered.current_fingerprints.clock_constraint
                     != identity.current_fingerprints.clock_constraint,
          "same-generation tamper fixture did not isolate clock-constraint identity");
  const std::string tamper_error = requireInvalid(
      [&] {
        static_cast<void>(NativeTimingEditPreparer::prepare(
            fixture.database(),
            bufferInsertionPlan(fixture.database(), baseline, kGeneration,
                                editTestCreationToken()),
            prepareContext(baseline, identity, kToken), resolver));
      },
      "same-generation external tamper was accepted by native prepare");
  require(tamper_error.find("active native state no longer matches the committed identity")
              != std::string::npos,
          "same-generation tamper rejection reported the wrong gate: " + tamper_error);
}

void testClockNetIsRejectedByDataOnlyBoundary()
{
  constexpr TimingEditSessionToken kToken{29};
  constexpr TransactionGeneration kGeneration{0};
  NativeEditFixture fixture = makeNativeEditFixture();
  TimingClock clock;
  clock.set_clock_name("fixture_clock");
  clock.set_source_list({"in"});
  clock.set_period(10.0);
  clock.set_fall_edge(5.0);
  fixture.database().get_timing_constraint().get_clock_map().emplace(
      "fixture_clock", std::move(clock));
  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(fixture.database());
  const CommittedEditIdentity identity = NativeTimingEditPreparer::captureCurrentIdentity(
      fixture.database(), kGeneration, editTestAnalysisContext(), kToken);
  const BaselineSemanticIdentity baseline
      = BaselineSemanticIdentity{fixture.database().get_design_name(), identity.current_fingerprints,
                                 identity.canonical_pre_state};
  const std::string error = requireInvalid(
      [&] {
        static_cast<void>(NativeTimingEditPreparer::prepare(
            fixture.database(),
            bufferInsertionPlan(fixture.database(), baseline, kGeneration,
                                editTestCreationToken()),
            prepareContext(baseline, identity, kToken), resolver));
      },
      "clock net was accepted by the Stage 3 data-only boundary");
  require(error.find("clock net is outside") != std::string::npos,
          "clock-net rejection reported the wrong gate: " + error);
}

}  // namespace

int main()
{
  testBaselineArcBindingsAreOneShot();
  testPreparedBatchStagesExistingAndCreatedRefsAppendOnly();
  testPreparerRejectsResolverWithoutExactLiveCoverage();
  testPreparerRejectsStaleGenerationAndSameGenerationTamper();
  testClockNetIsRejectedByDataOnlyBoundary();
  return 0;
}
