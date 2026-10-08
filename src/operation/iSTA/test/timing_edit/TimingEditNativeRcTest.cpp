// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <limits>
#include <stdexcept>
#include "AllocationFailureSupport.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditRcFixture.hpp"

namespace {
using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
template<class Action> void rejects(Action action)
{
  try { action(); }
  catch (const std::invalid_argument&) { return; }
  throw std::runtime_error("malformed raw RC edit was accepted");
}

}

void testNativeRcLocalUndo()
{
  constexpr TimingEditSessionToken token{83};
  constexpr TransactionGeneration generation{0};
  auto fixture = makeNativeEditFixture();
  auto& database = fixture.database();
  auto resolver = TimingEditResolverState::bindBaseline(database);
  const auto identity = NativeTimingEditPreparer::captureCurrentIdentity(
      database, generation, editTestAnalysisContext(), token);
  const BaselineSemanticIdentity baseline{database.get_design_name(), identity.current_fingerprints,
                                         identity.canonical_pre_state};
  auto plan = bufferInsertionPlan(database, baseline, generation, editTestCreationToken());
  const auto batch = NativeTimingEditPreparer::prepare(
      database, plan, {token, baseline, identity, editTestAnalysisContext()}, resolver);
  auto staged_resolver = resolver.stage(batch);
  const auto& names = staged_resolver.materialization(editTestCreationToken());
  auto edits = batch.normalizedEdits();
  edits.front().rc_inputs = ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree};
  edits.front().placement_inputs = PlacedInputs{5.0, 7.0};

  auto& current = database.get_parasitic_library().get_net_map();
  wire(current, "n0", "in", "out", 125.0);
  const auto* old_owner = &current.at("n0");
  const auto* old_node = &old_owner->get_node_map().at("out");
  const auto old_rc = ista::csr::detail::projectCanonicalParasiticNet("n0", *old_owner);
  const auto old_members = database.get_net_map().at("n0").get_pin_name_list();
  auto replacement = [&] {
    std::map<std::string, ParasiticNet> result;
    wire(result, "n0", "in", names.input_pin_name, 75.0);
    wire(result, names.output_net_name, names.output_pin_name, "out", 50.0);
    return result;
  };
  auto prepare = [&](std::map<std::string, ParasiticNet> rc) {
    return detail::prepareNativeBufferInsertions(
        database, edits, staged_resolver, detail::nextNativeInstanceId(database), std::move(rc));
  };
  rejects([&] { static_cast<void>(prepare({})); });
  for (int fault = 0; fault < 7; ++fault) {
    auto rc = replacement();
    if (fault == 0) rc.erase(names.output_net_name);
    if (fault == 1) rc.at("n0").get_node_map().erase(names.input_pin_name);
    if (fault == 2) rc.at("n0").get_resistor_list().front().set_resistance(std::numeric_limits<double>::infinity());
    if (fault == 3) wire(rc, "unrelated", "in", "out", 1.0);
    if (fault == 4) rc.at("n0").get_resistor_list().clear();
    if (fault == 5) rc.at("n0").get_resistor_list().front().set_sink_node("in");
    if (fault == 6) {
      auto& net = rc.at("n0");
      auto& extra = net.get_node_map()["unused"];
      extra.set_node_name("unused");
      net.get_resistor_list().push_back(net.get_resistor_list().front());
    }
    rejects([&] { static_cast<void>(prepare(std::move(rc))); });
    require(&current.at("n0") == old_owner && database.get_net_map().at("n0").get_pin_name_list() == old_members,
            "failed RC preparation changed native state");
  }
  // Pointer identity and real allocations distinguish transfer from a clone.
  auto prepared = prepare(replacement());
  auto moved = std::move(prepared);
  const auto* next_owner = &moved.parasitics.at("n0");
  ista_test_allocation::armCountOnly();
  moved.apply();
  const auto apply_allocations = ista_test_allocation::disable();
  require(apply_allocations == 0 && &current.at("n0") == next_owner
              && current.contains(names.output_net_name), "RC apply copied or allocated state");
  require(database.get_pin_map().at("out").get_net_name() == names.output_net_name,
          "RC and raw membership did not apply together");
  ista_test_allocation::armCountOnly();
  moved.undo();
  const auto undo_allocations = ista_test_allocation::disable();
  require(undo_allocations == 0 && &current.at("n0") == old_owner
              && &current.at("n0").get_node_map().at("out") == old_node
              && !current.contains(names.output_net_name), "RC undo did not restore actual old owners");
  require(ista::csr::detail::projectCanonicalParasiticNet("n0", current.at("n0")) == old_rc
              && database.get_net_map().at("n0").get_pin_name_list() == old_members
              && database.get_instance_map().empty(), "RC undo changed values or memberships");
  auto zero = replacement();
  zero.at("n0").get_resistor_list().front().set_resistance(0.0);
  auto retry = prepare(std::move(zero));
  retry.apply();
  require(current.at(names.output_net_name).get_resistor_list().front().get_resistance() == 50.0,
          "RC retry did not publish the replacement");
  retry.undo();
}
