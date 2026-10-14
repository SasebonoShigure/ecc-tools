// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "DataManager.hpp"
#include "DualCsrStructuredBatchTestSupport.hpp"
#include "DualCsrDescriptorValidation.hpp"
#include "DualCsrUpdaterInternal.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "../AllocationFailureSupport.hpp"
#include "NarrowTimingRecordReader.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"
#include "TimingEditTransactionNativeSupport.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using timing_edit::NativeSlotBinding;
using timing_edit::StableArcRef;
using timing_edit::TimingEditResolverState;
using structured_support::StructuredFixture;
using structured_support::captureAndBuildVerifiedBatch;
using structured_support::captureCandidateForContractTest;
using structured_support::makeStructuredFanoutFixture;
using structured_support::makeStructuredFixture;
using structured_support::prepare;

static_assert(!std::is_default_constructible_v<ExpectedBeforeBinding>);
static_assert(!std::is_default_constructible_v<PendingStructuredBatch>);
static_assert(!std::is_copy_constructible_v<PendingStructuredBatch>);
static_assert(!std::is_copy_assignable_v<PendingStructuredBatch>);
static_assert(std::is_nothrow_move_constructible_v<PendingStructuredBatch>);
static_assert(std::is_nothrow_move_assignable_v<PendingStructuredBatch>);
static_assert(!std::is_default_constructible_v<CandidateBoundCapture>);
static_assert(!std::is_aggregate_v<CandidateBoundCapture>);
static_assert(!std::is_copy_constructible_v<CandidateBoundCapture>);
static_assert(!std::is_copy_assignable_v<CandidateBoundCapture>);
static_assert(std::is_nothrow_move_constructible_v<CandidateBoundCapture>);
static_assert(std::is_nothrow_move_assignable_v<CandidateBoundCapture>);
static_assert(!std::is_default_constructible_v<VerifiedStructuredBatch>);
static_assert(!std::is_copy_constructible_v<VerifiedStructuredBatch>);
static_assert(!std::is_copy_assignable_v<VerifiedStructuredBatch>);
static_assert(std::is_nothrow_move_constructible_v<VerifiedStructuredBatch>);
static_assert(std::is_nothrow_move_assignable_v<VerifiedStructuredBatch>);

struct ForgedRawCaptureShape
{
  double transfer{0.0};
  std::vector<double> load_matrix;
  std::string library_cell;
  double pin_capacitance{0.0};
  bool node_role{false};
  csr::ArcId new_arc;
};

static_assert(!std::is_invocable_v<
              decltype(&TimingEditDualCsrDescriptorBuilder::build),
              PendingStructuredBatch, ForgedRawCaptureShape&&>);

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Callable>
void requireInvalid(Callable&& callable, std::string_view expected,
                    std::string_view label)
{
  try {
    callable();
  } catch (const std::invalid_argument& error) {
    require(std::string_view{error.what()}.find(expected)
                != std::string_view::npos,
            std::string{label} + " returned the wrong rejection: "
                + error.what());
    return;
  }
  throw std::runtime_error(std::string{label}
                           + " unexpectedly succeeded");
}

void purePrepareRejectsForeignBefore(const StructuredFixture& fixture)
{
  DualCsrState foreign = fixture.graph;
  foreign.session_token = timing_edit::TimingEditSessionToken{
      fixture.token.value() + 1};
  foreign.reference_payload.source_identity.session_token
      = foreign.session_token.value();
  const DualSemanticReadView foreign_view(foreign);
  requireInvalid(
      [&] {
        auto unexpected = DualCsrStructuredBatchPreparer::prepare(
            fixture.plan, fixture.resolver, foreign_view,
            fixture.graph, fixture.identity, fixture.arc_group_index);
        static_cast<void>(unexpected);
      },
      "view boundary", "foreign exact-before view");
  foreign = fixture.graph;
  foreign.session_token = timing_edit::TimingEditSessionToken{0};
  foreign.reference_payload.source_identity.session_token = 0;
  auto zero_index = fixture.arc_group_index;
  zero_index.session_token = foreign.session_token;
  requireInvalid([&] {
    static_cast<void>(DualCsrStructuredBatchPreparer::prepare(
        fixture.plan, fixture.resolver, DualSemanticReadView(foreign),
        foreign, fixture.identity, zero_index));
  }, "view boundary", "zero session exact-before");
}

const NativeSlotBinding& bindingValue(const NativeSlotBinding* binding)
{
  require(binding != nullptr, "fixture resolver binding is missing");
  return *binding;
}

void requireCapturePlan(const PendingStructuredBatch& pending)
{

  const auto& plan = pending.capturePlan();
  const auto& staged = pending.stagedResolver();
  std::uint64_t deactivations = 0;
  for (const auto& request : plan.arcs) {
    const NativeSlotBinding& binding
        = bindingValue(staged.findArcBinding(request.ref));
    const bool active = request.expected_post_multiplicity != 0;
    require(binding.tombstone != active,
            "capture multiplicity disagrees with resolver lifecycle");
    require(request.expected_current_key.has_value()
                == (binding.slot.value() < plan.before.arc_slots),
            "capture current-key binding disagrees with ArcId lifetime");
    if (!active) {
      ++deactivations;
      require(request.expected_current_key.has_value()
                  && request.expected_current_key->base
                         == request.expected_post_base,
              "deactivation did not bind its exact committed base");
      continue;
    }
    const auto same_base = std::ranges::count_if(
        plan.arcs, [&](const auto& peer) {
          return peer.expected_post_multiplicity != 0
                 && peer.expected_post_base
                        == request.expected_post_base;
        });
    require(request.expected_post_multiplicity
                == static_cast<std::uint64_t>(same_base),
            "active request does not cover its complete post base group");
  }
  require(deactivations == 2,
          "two moved sinks did not produce two deactivations");
  require(!plan.node_roles.empty()
              && !plan.cell_pin_capacitances.empty(),
          "fixture did not freeze typed role/capacitance dependencies");
  for (const auto& request : plan.node_roles)
    require(std::ranges::count(plan.node_roles, request.ref,
                               &AffectedNodeModelCapture::ref)
                == 1,
            "node-role dependency repeats a StableNodeRef");
  for (const auto& request : plan.cell_pin_capacitances)
    require(std::ranges::count(plan.cell_pin_capacitances, request.ref,
                               &AffectedNodeModelCapture::ref)
                == 1,
            "capacitance dependency repeats a StableNodeRef");
}

void requireCaptureAudit(const PendingStructuredBatch& pending,
                         const CandidateBoundCapture& capture)
{
  require(capture.success() && capture.audit() != nullptr,
          "real reader did not mint a successful capture capability");
  const auto& audit = *capture.audit();
  const auto& plan = pending.capturePlan();
  const std::uint64_t active = std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity != 0;
      });
  const std::uint64_t deactivated = std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity == 0;
      });
  require(audit.requested_nodes == plan.nodes.size()
              && audit.requested_instances
                     == plan.token_materializations.size()
              && audit.requested_arcs == active
              && audit.requested_deactivations == deactivated
              && audit.requested_load_nets == plan.load_nets.size()
              && audit.requested_node_roles == plan.node_roles.size()
              && audit.requested_cell_pin_capacitances
                     == plan.cell_pin_capacitances.size(),
          "capture audit requests do not equal the frozen typed plan");
  require(audit.accepted_nodes == audit.requested_nodes
              && audit.accepted_instances == audit.requested_instances
              && audit.accepted_arcs == audit.requested_arcs
              && audit.accepted_deactivations
                     == audit.requested_deactivations
              && audit.accepted_load_nets
                     == audit.requested_load_nets
              && audit.accepted_library_cells
                     == audit.requested_library_cells
              && audit.accepted_node_roles
                     == audit.requested_node_roles
              && audit.accepted_cell_pin_capacitances
                     == audit.requested_cell_pin_capacitances,
          "capture audit accepted a non-bijective typed record set");
  require(audit.instance_reads == audit.requested_instances
              && audit.net_reads == audit.requested_load_nets
              && audit.library_cell_reads
                     == audit.requested_library_cells
              && audit.node_role_reads == audit.requested_node_roles
              && audit.cell_pin_capacitance_reads
                     == audit.requested_cell_pin_capacitances
              && audit.deactivated_arc_before_reads
                     == audit.requested_deactivations,
          "actual typed projector/read calls do not equal frozen requests");
  require(audit.node_reads >= audit.accepted_nodes
              && audit.node_reads <= plan.after.node_slots
              && audit.native_arc_candidate_reads
                     >= audit.accepted_arcs
              && audit.native_arc_candidate_reads
                     <= audit.native_arc_candidate_upper_bound
              && plan.after.arc_slots
                     <= std::numeric_limits<std::uint64_t>::max() / 2
              && audit.native_arc_candidate_upper_bound
                     <= plan.after.arc_slots * 2,
          "local native read supersets are outside their exact bounds");
  require(audit.captured_arc_records
                  == audit.accepted_arcs
                         + audit.accepted_deactivations
              && audit.rejected_reads == 0
              && audit.full_export_guard_active
              && !audit.full_export_observed
              && audit.native_full_graph_exports == 0
              && audit.native_full_reference_exports == 0,
          "capture audit is not a guarded narrow-read proof");
}

bool matchesReconnect(const StableArcRef& arc,
                      const timing_edit::StableNodeRef& sink,
                      const timing_edit::StableNetRef& net)
{
  return std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref,
                                     timing_edit::ExistingArcRef>) {
          return ref.baseline_key.base.kind == csr::ArcKind::kNet
                 && timing_edit::StableNodeRef{
                        timing_edit::ExistingNodeRef{
                            ref.baseline_key.base.sink}}
                        == sink
                 && timing_edit::StableNetRef{
                        timing_edit::ExistingNetRef{
                            ref.baseline_key.base.owner_name}}
                        == net;
        } else if constexpr (std::is_same_v<
                                 Ref,
                                 timing_edit::CreatedOldNetArcRef>) {
          return ref.sink == sink && ref.net == net;
        } else if constexpr (std::is_same_v<
                                 Ref,
                                 timing_edit::CreatedNewNetArcRef>) {
          return ref.sink == sink
                 && timing_edit::StableNetRef{
                        timing_edit::CreatedNetRef{
                            ref.creation_token,
                            timing_edit::CreatedNetRole::kOutputNet}}
                        == net;
        } else {
          return false;
        }
      },
      arc);
}

template <typename Operation>
std::vector<StableArcRef> matchingArcs(
    const timing_edit::TimingGraphDelta& delta,
    const timing_edit::StableNodeRef& sink,
    const timing_edit::StableNetRef& net)
{
  std::vector<StableArcRef> result;
  for (const auto& operation : delta.operations)
    if (const auto* item = std::get_if<Operation>(&operation);
        item != nullptr && matchesReconnect(item->arc, sink, net))
      result.push_back(item->arc);
  return result;
}

void requireReversePairs(const StructuredFixture& fixture, const VerifiedStructuredBatch& verified)
{
  const auto& batch = verified.preparedBatch();
  const timing_edit::TimingEditResolverView staged{fixture.resolver, verified.resolverDelta()};
  const auto& descriptor = verified.descriptor();
  std::uint64_t reconnects = 0;
  for (const auto& operation : batch.forwardDelta().operations) {
    const auto* reconnect
        = std::get_if<timing_edit::ReconnectSink>(&operation);
    if (reconnect == nullptr)
      continue;
    ++reconnects;
    const auto removed = matchingArcs<timing_edit::RemoveNetArc>(
        batch.forwardDelta(), reconnect->sink, reconnect->old_net);
    const auto added = matchingArcs<timing_edit::AddNetArc>(
        batch.forwardDelta(), reconnect->sink, reconnect->new_net);
    require(removed.size() == 1 && added.size() == 1,
            "reconnect does not have one exact remove/add StableArcRef");
    const auto& old_binding = bindingValue(staged.findArcBinding(removed.front()));
    const auto& new_binding = bindingValue(staged.findArcBinding(added.front()));
    const auto& sink_binding = bindingValue(staged.findNodeBinding(reconnect->sink));
    require(old_binding.tombstone && !new_binding.tombstone
                && !sink_binding.tombstone,
            "reconnect resolver lifecycles are malformed");
    const auto matches = std::ranges::count_if(
        descriptor.reverse_changes, [&](const auto& change) {
          return change.expected_arc.value()
                         == old_binding.slot.value()
                 && change.replacement_arc.value()
                        == new_binding.slot.value()
                 && change.sink.value() == sink_binding.slot.value();
        });
    require(matches == 1,
            "BaseIn pairing lost exact destination/StableArcRef identity");
  }
  require(reconnects == 2
              && descriptor.reverse_changes.size() == reconnects,
          "reverse pairing did not cover both moved sinks");
}

csr::ArcId createdOldNetArcId(const timing_edit::PreparedBatch& batch)
{
  std::optional<csr::ArcId> result;
  for (const auto& binding : batch.slotPlan().arcs)
    if (!binding.tombstone && std::holds_alternative<timing_edit::CreatedOldNetArcRef>(binding.ref)) {
      require(!result, "fixture has multiple driver-to-buffer arcs");
      result = csr::ArcId{binding.slot.value()};
    }
  require(result.has_value(), "fixture has no driver-to-buffer StableArcRef");
  return *result;
}

csr::ArcId createdNewNetArcId(const timing_edit::PreparedBatch& batch, std::string_view sink_name)
{
  std::optional<csr::ArcId> result;
  for (const auto& binding : batch.slotPlan().arcs) {
    const auto* created = std::get_if<timing_edit::CreatedNewNetArcRef>(&binding.ref);
    if (created == nullptr || binding.tombstone)
      continue;
    const auto* sink = std::get_if<timing_edit::ExistingNodeRef>(&created->sink);
    if (sink == nullptr || sink->baseline_key.full_name != sink_name)
      continue;
    require(!result, "created fanout StableArcRef is not unique");
    result = csr::ArcId{binding.slot.value()};
  }
  require(result.has_value(), "fixture is missing created fanout arc to " + std::string{sink_name});
  return *result;
}

csr::NodeId createdNodeId(const timing_edit::PreparedBatch& batch, timing_edit::CreatedNodeRole role)
{
  std::optional<csr::NodeId> result;
  for (const auto& binding : batch.slotPlan().nodes) {
    const auto* created = std::get_if<timing_edit::CreatedNodeRef>(&binding.ref);
    if (created == nullptr || created->role != role || binding.tombstone)
      continue;
    require(!result, "created node role is not unique");
    result = csr::NodeId{binding.slot.value()};
  }
  require(result.has_value(), "fixture is missing created node role");
  return *result;
}

const ArcAppendRecord& appendedArc(const DualCsrBatchDescriptor& descriptor,
                                   csr::ArcId id)
{
  const auto found = std::ranges::find(descriptor.arc_appends, id,
                                       &ArcAppendRecord::id);
  require(found != descriptor.arc_appends.end(),
          "FIFO row references a non-appended ArcId");
  return *found;
}

void requireCandidatePhysicalOrder(const VerifiedStructuredBatch& verified)
{
  const auto& descriptor = verified.descriptor();
  const auto& staged = verified.preparedBatch();
  const csr::ArcId out = createdNewNetArcId(staged, "out");
  const csr::ArcId z = createdNewNetArcId(staged, "z");
  const csr::NodeId source = createdNodeId(
      staged, timing_edit::CreatedNodeRole::kOutput);
  const auto row = std::ranges::find(
      descriptor.appended_base_out_rows, source,
      &AppendedBaseOutRowDescriptor::source);
  require(row != descriptor.appended_base_out_rows.end()
              && row->arc_ids == std::vector<csr::ArcId>{out, z},
          "new BaseOut row lost candidate native fanout order");
  require(csr::arcKeyLess(appendedArc(descriptor, z).key,
                          appendedArc(descriptor, out).key),
          "FIFO canary is not opposite canonical ArcKey order");
}

void requireVerifiedDescriptor(const StructuredFixture& fixture,
                               VerifiedStructuredBatch verified)
{
  const auto& descriptor = verified.descriptor();
  const auto& before = fixture.graph;
  require(descriptor.reference_payload.instances && descriptor.reference_payload.cell_pin_capacitances,
          "new buffer descriptor lacks instance/capacitance records");
  for (unsigned mutation = 0; mutation != 6; ++mutation) {
    auto malformed = descriptor;
    auto& delta = malformed.reference_payload;
    switch (mutation) {
      case 0: delta.instances.reset(); break;
      case 1: delta.instances->values.front().ordered_pins.pop_back(); break;
      case 2: std::ranges::reverse(delta.instances->values.front().ordered_pins); break;
      case 3: delta.cell_pin_capacitances.reset(); break;
      case 4: delta.cell_pin_capacitances->values.pop_back(); break;
      case 5: delta.cell_pin_capacitances->values.front().default_capacitance += 1.0; break;
    }
    const auto errors = validateDualCsrDescriptor(before, malformed);
    require(std::ranges::any_of(errors, [](const auto& error) {
              return error.field == "reference_payload.instances" || error.field == "reference_payload.cell_pin_capacitances";
            }), "malformed appended pin closure passed the public descriptor boundary");
  }
  {
    auto conflicting = before;
    auto malformed = descriptor;
    conflicting.reference_payload.instances.push_back(descriptor.reference_payload.instances->values.front());
    ++malformed.reference_payload.instances->expected_size;
    const auto errors = validateDualCsrDescriptor(conflicting, malformed);
    require(std::ranges::any_of(errors, [](const auto& error) {
              return error.field == "reference_payload.instances"
                     && error.message == "appended instance names must be nonempty and new";
            }), "public descriptor accepted an existing instance name");
  }
  require(verified.preparedBatch().sessionToken() == before.session_token
              && verified.preparedBatch().expectedGeneration() == before.generation,
          "verified batch lost its session/generation binding");
  require(descriptor.node_appends.size() == 2
              && descriptor.arc_appends.size() == 4
              && descriptor.node_payload_changes.size() == 2
              && descriptor.logical_deactivations.size() == 2
              && descriptor.changes.size() == 1
              && descriptor.removes.empty()
              && descriptor.aux_rows.size() == 1
              && descriptor.appended_base_out_rows.size() == 2
              && descriptor.affected_base_in_rows.size() == 4
              && descriptor.reverse_changes.size() == 2,
          "successful structured descriptor has the wrong delta shape");

  auto replacement = before.reference_payload.source_identity;
  replacement.session_token = fixture.token.value();
  replacement.graph_generation = descriptor.target_generation.value();
  require(descriptor.reference_payload.source_identity
              == std::optional<ValueReplacement<csr::CsrSnapshotIdentity>>{
                  ValueReplacement<csr::CsrSnapshotIdentity>{
                      before.reference_payload.source_identity,
                      replacement}},
          "descriptor lost its typed source-identity replacement");

  const csr::ArcId driver_arc
      = createdOldNetArcId(verified.preparedBatch());
  const auto& change = descriptor.changes.front();
  require(change.expected_arc
                  == std::optional<csr::ArcId>{fixture.base_arc}
              && change.replacement_arc == driver_arc,
          "BaseOut did not reuse the same-batch removed slot first");
  const auto& aux = descriptor.aux_rows.front();
  require(aux.expected_arc_ids
                  == std::vector<csr::ArcId>{fixture.aux_arc}
              && aux.replacement_arc_ids.empty(),
          "Aux row did not remove the exact deactivated ArcId");
  require(std::ranges::all_of(
              descriptor.logical_deactivations,
              [](const auto& item) {
                return item.reason
                       == LogicalArcDeactivateReason::kReplaced;
              }),
          "reconnected arcs did not use typed kReplaced deactivation");
  requireReversePairs(fixture, verified);
  requireCandidatePhysicalOrder(verified);

  {
    auto graph = before;
    auto index = fixture.arc_group_index;
    const auto saved_index = index;
    detail::PreparedDualCsrIndexPatch index_patch(graph, index, descriptor);
    detail::PreparedDualCsrGraphPatch graph_patch(graph, descriptor);
    ista_test_allocation::armFailure(1);
    graph_patch.apply();
    index_patch.apply();
    const auto allocations = ista_test_allocation::disable();
    require(allocations == 0, "prepared graph/index apply allocated");
    require(validateDualCsrArcGroupIndex(graph, index).empty(), "local index patch produced an invalid index");
    ista_test_allocation::armFailure(1);
    index_patch.rollback();
    graph_patch.rollback();
    const auto undo_allocations = ista_test_allocation::disable();
    require(undo_allocations == 0 && index == saved_index && graph.nodes == before.nodes
                && graph.arc_slots == before.arc_slots && graph.reference_payload == before.reference_payload
                && validateDualCsrArcGroupIndex(graph, index).empty(),
            "local graph/index rollback failed to restore actual old values");
  }
  if constexpr (incregpusta_debug_checks_enabled) {
    auto graph = before;
    auto index = fixture.arc_group_index;
    const auto saved_index = index;
    std::size_t allocations;
    {
      ista_test_allocation::armCountOnly();
      detail::PreparedDualCsrIndexPatch patch(graph, index, descriptor);
      allocations = ista_test_allocation::disable();
    }
    for (std::size_t ordinal = 1; ordinal <= allocations; ++ordinal) {
      try {
        ista_test_allocation::armFailure(ordinal);
        detail::PreparedDualCsrIndexPatch patch(graph, index, descriptor);
        static_cast<void>(ista_test_allocation::disable());
      } catch (const std::bad_alloc&) {
        static_cast<void>(ista_test_allocation::disable());
      }
      require(index == saved_index && validateDualCsrArcGroupIndex(graph, index).empty(),
              "index preparation allocation failure changed the committed state");
    }
    // A successfully prepared patch still works after every interrupted preparation.
    detail::PreparedDualCsrIndexPatch index_patch(graph, index, descriptor);
    detail::PreparedDualCsrGraphPatch graph_patch(graph, descriptor);
    graph_patch.apply();
    index_patch.apply();
    require(validateDualCsrArcGroupIndex(graph, index).empty(), "index could not retry after allocation failure");
    index_patch.rollback();
    graph_patch.rollback();
    require(index == saved_index, "index retry rollback lost its original state");
  }
  auto staged = before;
  require(validateDualCsrDescriptor(staged, descriptor).empty(), "verified descriptor was invalid");
  detail::PreparedDualCsrGraphPatch patch(staged, descriptor);
  patch.apply();
  require(staged.generation == descriptor.target_generation
              && !staged.arc_slots.at(static_cast<std::size_t>(fixture.base_arc.value())).active
              && !staged.arc_slots.at(static_cast<std::size_t>(fixture.aux_arc.value())).active,
          "verified descriptor did not apply the expected successor");
  patch.commit();
}

void fullSinkNetFanoutLoadContracts()
{
  timing_edit::test::installNativeTransactionBaseline();
  timing_edit::test::GuardedTestSession session;
  const StructuredFixture fixture = makeStructuredFanoutFixture(
      STADM.getDatabase(), session.token());
  require(fixture.unmoved_fanout_arc.has_value(),
          "fanout fixture has no unmoved ArcId");
  const csr::ArcId fanout = *fixture.unmoved_fanout_arc;
  const auto* indexed = fixture.arc_group_index.slot(fanout);
  const auto* committed_group
      = fixture.arc_group_index.activeSinkNetGroup("n0");
  require(indexed != nullptr && indexed->current_active_key
              && committed_group != nullptr
              && committed_group->active_arc_ids.size() == 3,
          "fanout fixture lacks its exact committed sink-net group");

  auto pending = prepare(fixture);
  const auto request = std::ranges::find(
      pending.capturePlan().arcs, indexed->ref,
      &AffectedArcCapture::ref);
  require(request != pending.capturePlan().arcs.end()
              && request->expected_post_multiplicity == 1
              && request->expected_current_key
                     == indexed->current_active_key
              && std::ranges::none_of(
                     pending.preparedBatch().slotPlan().arcs,
                     [&](const auto& assignment) {
                       return assignment.ref == indexed->ref;
                     }),
          "load closure did not select the unaffected stable fanout arc");
  for (const auto id : committed_group->active_arc_ids) {
    const auto* slot = fixture.arc_group_index.slot(id);
    require(slot != nullptr
                && std::ranges::count(
                       pending.capturePlan().arcs, slot->ref,
                       &AffectedArcCapture::ref) == 1,
            "sink-net closure omitted a committed active ArcId");
  }

  VerifiedStructuredBatch verified
      = captureAndBuildVerifiedBatch(session, fixture);
  auto descriptor = verified.descriptor();
  std::ranges::reverse(descriptor.payload_changes);
  std::ranges::reverse(descriptor.node_payload_changes);
  const auto change = std::ranges::find(
      descriptor.payload_changes, fanout,
      &ArcPayloadChangeDescriptor::id);
  require(change == descriptor.payload_changes.end(),
          "unmoved Net fanout received a redundant load payload change");

  const csr::ArcId driver
      = createdOldNetArcId(verified.preparedBatch());
  auto after = fixture.graph;
  auto index = fixture.arc_group_index;
  detail::PreparedDualCsrIndexPatch index_patch(after, index, descriptor);
  detail::PreparedDualCsrGraphPatch patch(after, descriptor);
  patch.apply();
  index_patch.apply();
  require(validateDualCsrArcGroupIndex(after, index).empty(),
          "fanout membership delta disagrees with independently rebuilt groups");
  const auto source = after.arc_slots.at(fanout.value()).payload.source.value();
  std::vector<csr::ArcId> actual;
  for (auto slot = after.base_out.row_offsets.at(source);
       slot < after.base_out.row_offsets.at(source + 1); ++slot)
    if (after.base_out.physical_slots.at(slot))
      actual.push_back(*after.base_out.physical_slots.at(slot));
  const auto aux = after.aux_out.source_to_aux_row.at(source);
  if (aux >= 0) {
    const auto& row = after.aux_out.row_arcs.at(static_cast<std::size_t>(aux));
    actual.insert(actual.end(), row.begin(), row.end());
  }
  require(actual == std::vector<csr::ArcId>{driver, fanout}
              && csr::arcKeyLess(after.arc_slots.at(fanout.value()).payload.key,
                                 appendedArc(descriptor, driver).key),
          "applied old-source survivor row lost candidate FIFO order");
  const auto driver_append = std::ranges::find(
      descriptor.arc_appends, driver, &ArcAppendRecord::id);
  require(driver_append != descriptor.arc_appends.end()
              && !driver_append->payload.output_load,
          "new Net arc retained a redundant output load");
  for (const auto id : committed_group->active_arc_ids) {
    if (id == fanout)
      continue;
    require(std::ranges::count(
                descriptor.logical_deactivations, id,
                &LogicalArcDeactivate::id) == 1,
            "sink-net closure neither updated nor deactivated an old arc");
  }
  index_patch.rollback();
  patch.rollback();
  require(index == fixture.arc_group_index,
          "fanout membership rollback lost original groups");
  require(session.closeAndRelease().ok(),
          "fanout load test session close failed");
}

void structuredBatchContracts()
{
  timing_edit::test::installNativeTransactionBaseline();
  timing_edit::test::GuardedTestSession session;
  const StructuredFixture fixture
      = makeStructuredFixture(STADM.getDatabase(), session.token());

  purePrepareRejectsForeignBefore(fixture);
  auto pending = prepare(fixture);
  requireCapturePlan(pending);
  CandidateBoundCapture capture = captureCandidateForContractTest(
      session, fixture, pending);
  requireCaptureAudit(pending, capture);
  CandidateBoundCapture owned = std::move(capture);
  require(!capture.success() && capture.audit() == nullptr,
          "moved-from capture capability remained usable");
  auto rejected_pending = prepare(fixture);
  requireInvalid(
      [&] {
        auto unexpected = TimingEditDualCsrDescriptorBuilder::build(
            std::move(rejected_pending), std::move(capture));
        static_cast<void>(unexpected);
      },
      "moved-from", "moved-from capture capability");
  VerifiedStructuredBatch verified
      = TimingEditDualCsrDescriptorBuilder::build(
          std::move(pending), std::move(owned));
  requireVerifiedDescriptor(fixture, std::move(verified));
  require(session.closeAndRelease().ok(),
          "structured test session close failed");
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_edit::test::NativeTransactionRuntime runtime;
    ista::timing_dual_csr::test::structuredBatchContracts();
    ista::timing_dual_csr::test::fullSinkNetFanoutLoadContracts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
