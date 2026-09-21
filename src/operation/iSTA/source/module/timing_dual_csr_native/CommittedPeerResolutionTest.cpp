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
  auto resolved = ista::timing_dual_csr::detail::resolvePlannedNodeId(
      committed, staged, empty_plan, peer.key);
  const auto* id = std::get_if<csr::NodeId>(&resolved);
  require(id != nullptr && *id == peer.id
              && id->value() < empty_plan.before.node_slots,
          "exact committed peer did not preserve its existing NodeId");

  TimingEditResolverState shifted = makeSlotShiftedResolver();
  const timing_edit::StableNodeRef peer_ref{
      timing_edit::ExistingNodeRef{peer.key}};
  const auto shifted_binding = shifted.nodeBindings().find(peer_ref);
  require(shifted_binding != shifted.nodeBindings().end()
              && shifted_binding->second.slot.value() != peer.id.value(),
          "slot-mismatch fixture did not shift the committed peer");
  resolved = ista::timing_dual_csr::detail::resolvePlannedNodeId(
      committed, shifted, empty_plan, peer.key);
  const auto* error = std::get_if<NarrowCaptureError>(&resolved);
  require(error != nullptr
              && *error == NarrowCaptureError::kBatchBindingMismatch,
          "slot-shifted staged peer escaped exact committed binding");

  csr::NodeKey unknown = peer.key;
  unknown.full_name = "unknown_post_candidate_peer";
  resolved = ista::timing_dual_csr::detail::resolvePlannedNodeId(
      committed, staged, empty_plan, unknown);
  error = std::get_if<NarrowCaptureError>(&resolved);
  require(error != nullptr
              && *error == NarrowCaptureError::kUnplannedDependency,
          "unknown post-candidate peer escaped the plan boundary");
}

}  // namespace

int main()
{
  checkCommittedPeerFallbackIsExact();
  return 0;
}
