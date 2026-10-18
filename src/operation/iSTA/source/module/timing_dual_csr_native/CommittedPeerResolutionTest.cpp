#include "NativeDualCsrTypedCaptureInternal.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "Arc.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "Database.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "Net.hpp"
#include "Pin.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingEditResolver.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingPoint.hpp"

namespace {

using namespace ista;
using namespace ista::timing_dual_csr;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

constexpr TimingEditSessionToken kSession{79};

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error("committed peer resolution test: "
                             + message);
}

DualCsrState makeCommittedState(
    Database& database, const TimingEditResolverState& resolver)
{
  auto exported = csr::exportCsrReferenceInputCandidate(
      database, editTestAnalysisContext(), kSession.value());
  const auto* input = std::get_if<csr::CsrReferenceInput>(&exported);
  require(input != nullptr, "baseline reference export failed");
  auto attempt = DualCsrBaselineFactory::create(
      *input, kSession, resolver.highWater());
  require(attempt.success(), "baseline creation failed");
  return std::move(*attempt.state);
}

const csr::NodeRecord& nodeNamed(const DualCsrState& state,
                                 const std::string& name)
{
  const csr::NodeRecord* result = nullptr;
  for (const auto& node : state.nodes) {
    if (node.key.full_name != name)
      continue;
    require(result == nullptr, "committed node key is not unique");
    result = &node;
  }
  require(result != nullptr, "committed peer node is missing");
  return *result;
}

TimingEditResolverState makeSlotShiftedResolver()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  Database& database = fixture.database();

  Pin shifted_peer;
  shifted_peer.set_pin_name("aaa");
  shifted_peer.set_full_name("aaa");
  shifted_peer.set_net_name("n0");
  shifted_peer.set_direction(PinDirection::kOutput);
  shifted_peer.set_is_port(true);
  database.get_pin_map().emplace("aaa", std::move(shifted_peer));

  Net& net = database.get_net_map().at("n0");
  net.set_pin_name_list({"in", "aaa", "out"});
  net.set_load_pin_list({"aaa", "out"});
  database.get_timing_point_map().emplace("aaa", TimingPoint{});
  database.get_timing_point_map().at("aaa").set_level(2);
  database.get_end_point_list() = {"aaa", "out"};
  database.get_timing_order_list() = {"in", "aaa", "out"};

  Arc arc;
  arc.set_arc_name("n0:in->aaa");
  arc.set_source_pin("in");
  arc.set_sink_pin("aaa");
  arc.set_owner_name("n0");
  arc.set_type(ArcType::kNet);
  database.get_arc_list().push_back(std::move(arc));
  const std::size_t index = database.get_arc_list().size() - 1;
  database.get_outgoing_arc_list_map()["in"].push_back(index);
  database.get_incoming_arc_list_map()["aaa"].push_back(index);
  return TimingEditResolverState::bindBaseline(database);
}

ista::timing_dual_csr::detail::PlannedNodeId resolve(
    const DualSemanticReadView& committed, TimingEditResolverView staged,
    const AffectedCapturePlan& plan, const csr::NodeKey& key)
{
  auto indexed = ista::timing_dual_csr::detail::indexPlannedNodeCapture(plan);
  if (const auto* error = std::get_if<NarrowCaptureError>(&indexed))
    return *error;
  return ista::timing_dual_csr::detail::resolvePlannedNodeId(
      committed, staged, plan, std::get<ista::timing_dual_csr::detail::PlannedNodeCaptureIndex>(indexed), key);
}

void checkCommittedPeerFallbackIsExact()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  TimingEditResolverState staged
      = TimingEditResolverState::bindBaseline(fixture.database());
  DualCsrState committed_state
      = makeCommittedState(fixture.database(), staged);
  DualSemanticReadView committed{committed_state};

  AffectedCapturePlan empty_plan{};
  empty_plan.before = staged.highWater();
  empty_plan.after = staged.highWater();
  require(empty_plan.nodes.empty() && empty_plan.node_roles.empty()
              && empty_plan.cell_pin_capacitances.empty()
              && empty_plan.arcs.empty(),
          "peer canary unexpectedly entered the descriptor plan");

  const std::string peer_name{"out"};
  const csr::NodeRecord& peer = nodeNamed(committed_state, peer_name);
  auto resolved = resolve(
      committed, staged, empty_plan, peer.key);
  const auto* id = std::get_if<csr::NodeId>(&resolved);
  require(id != nullptr && *id == peer.id
              && id->value() < empty_plan.before.node_slots,
          "exact committed peer did not preserve its existing NodeId");

  // The same native key cannot name different refs across request collections.
  auto conflicting = empty_plan;
  conflicting.nodes.push_back({ExistingNodeRef{peer.key}, peer.key});
  auto foreign_key = peer.key;
  foreign_key.full_name = "foreign";
  conflicting.node_roles.push_back(
      {ExistingNodeRef{foreign_key}, peer.key, "cell", peer.key.pin_name});
  resolved = resolve(committed, staged, conflicting, peer.key);
  require(std::get<NarrowCaptureError>(resolved) == NarrowCaptureError::kDuplicateRequest,
          "cross-collection key/ref conflict was not rejected");

  TimingEditResolverState shifted = makeSlotShiftedResolver();
  const timing_edit::StableNodeRef peer_ref{
      timing_edit::ExistingNodeRef{peer.key}};
  const auto shifted_binding = shifted.nodeBindings().find(peer_ref);
  require(shifted_binding != shifted.nodeBindings().end()
              && shifted_binding->second.slot.value() != peer.id.value(),
          "slot-mismatch fixture did not shift the committed peer");
  resolved = resolve(
      committed, shifted, empty_plan, peer.key);
  const auto* error = std::get_if<NarrowCaptureError>(&resolved);
  require(error != nullptr
              && *error == NarrowCaptureError::kBatchBindingMismatch,
          "slot-shifted staged peer escaped exact committed binding");

  csr::NodeKey unknown = peer.key;
  unknown.full_name = "unknown_post_candidate_peer";
  resolved = resolve(
      committed, staged, empty_plan, unknown);
  error = std::get_if<NarrowCaptureError>(&resolved);
  require(error != nullptr
              && *error == NarrowCaptureError::kUnplannedDependency,
          "unknown post-candidate peer escaped the plan boundary");
}

void checkCreatedCommittedPeer()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  auto resolver = TimingEditResolverState::bindBaseline(fixture.database());
  auto state = makeCommittedState(fixture.database(), resolver);
  const auto identity = NativeTimingEditPreparer::captureCurrentIdentity(
      fixture.database(), TransactionGeneration{0}, editTestAnalysisContext(), kSession);
  const BaselineSemanticIdentity baseline{
      fixture.database().get_design_name(), identity.current_fingerprints, identity.canonical_pre_state};
  const auto plan = bufferInsertionPlan(
      fixture.database(), baseline, TransactionGeneration{0}, editTestCreationToken());
  const auto batch = NativeTimingEditPreparer::prepare(
      fixture.database(), plan,
      TimingEditPrepareContext{kSession, baseline, identity, editTestAnalysisContext()}, resolver);
  auto delta = resolver.prepare(batch);
  const TimingEditResolverView overlay(resolver, delta);
  auto staged = resolver.stage(batch);
  state.high_water = staged.highWater();
  state.generation = TransactionGeneration{1};
  state.nodes.resize(state.high_water.node_slots);
  const auto& names = batch.tokenMaterializations().front();
  const auto* net_entry = overlay.findNetEntryForName(names.output_net_name);
  const auto* committed_net_entry = TimingEditResolverView(staged).findNetEntryForName(names.output_net_name);
  require(net_entry && committed_net_entry && *net_entry == *committed_net_entry
              && net_entry->first == StableNetRef{CreatedNetRef{names.creation_token, CreatedNetRole::kOutputNet}}
              && !net_entry->second.tombstone,
          "created net entry lost its stable identity across resolver publish");
  for (const auto& assignment : batch.slotPlan().nodes) {
    auto& node = state.nodes.at(assignment.slot.value());
    node.id = csr::NodeId{assignment.slot.value()};
    const auto& created = std::get<CreatedNodeRef>(assignment.ref);
    const bool input = created.role == CreatedNodeRole::kInput;
    node.key = {csr::NodeKind::kInstancePin,
                input ? names.input_pin_name : names.output_pin_name,
                names.instance_name, input ? "A" : "Y"};
  }
  AffectedCapturePlan untouched{};
  untouched.before = untouched.after = staged.highWater();
  const DualSemanticReadView committed{state};
  for (const auto& assignment : batch.slotPlan().nodes) {
    const auto& node = state.nodes.at(assignment.slot.value());
    const auto* overlay_binding = overlay.findNodeEntryForKey(node.key);
    require(overlay_binding && overlay_binding->first == assignment.ref && !overlay_binding->second.tombstone
                && overlay_binding->second.slot == assignment.slot,
            "created peer in unapplied resolver delta was not found");
    auto missing = untouched;
    missing.before = resolver.highWater();
    missing.node_roles.push_back({assignment.ref, node.key, "buffer", node.key.pin_name});
    const auto missing_result = resolve(committed, staged, missing, node.key);
    require(std::get<NarrowCaptureError>(missing_result) == NarrowCaptureError::kUnplannedDependency,
            "new model request without a node declaration was accepted");

    auto native_node = node;
    native_node.instance_cell = plan.edits.front().library_cell.cell_name;
    AffectedNodeModelCapture model{assignment.ref, node.key, "foreign_cell", node.key.pin_name};
    auto projected = ista::timing_dual_csr::detail::projectPlannedCapacitance(
        fixture.database(), &model, staged, untouched.after, native_node);
    require(std::get<NarrowCaptureError>(projected) == NarrowCaptureError::kUnplannedDependency,
            "native cell mismatch was accepted through indexed request");
    model.expected_cell = native_node.instance_cell;
    model.expected_port = "foreign_port";
    projected = ista::timing_dual_csr::detail::projectPlannedCapacitance(
        fixture.database(), &model, staged, untouched.after, native_node);
    require(std::get<NarrowCaptureError>(projected) == NarrowCaptureError::kUnplannedDependency,
            "native port mismatch was accepted through indexed request");

    const auto result = resolve(
        committed, staged, untouched, node.key);
    const auto* id = std::get_if<csr::NodeId>(&result);
    require(id && *id == node.id,
            "previously created peer was incorrectly treated as a baseline ExistingNodeRef");
  }
}

}  // namespace

int main()
{
  checkCommittedPeerFallbackIsExact();
  checkCreatedCommittedPeer();
  return 0;
}
