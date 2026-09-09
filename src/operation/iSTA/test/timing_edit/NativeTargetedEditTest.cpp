// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#include <iostream>
#include <stdexcept>

#include "Arc.hpp"
#include "DataManager.hpp"
#include "GraphBuilder.hpp"
#include "Logger.hpp"
#include "NativeTargetedEditInternal.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "TimingClock.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "Utility.hpp"

namespace {
using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;
void require(bool value, const char* message)
{
  if (!value)
    throw std::runtime_error(message);
}
template <class F>
void rejects(F&& work)
{
  try {
    work();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("expected native targeted rejection");
}
csr::CsrReferenceInput input(const Database& database)
{
  auto result = csr::exportCsrReferenceInputCandidate(database, editTestAnalysisContext(), 17);
  require(std::holds_alternative<csr::CsrReferenceInput>(result), "test reference export failed");
  return std::get<csr::CsrReferenceInput>(std::move(result));
}
void compareFresh(const Database& database)
{
  auto raw = NativeDatabaseCandidate::create(database);
  STADM.getDatabase() = raw.database();
  STAGB.buildStructure();
  const auto actual = csr::exportCsrGraphSnapshot(database);
  const auto fresh = csr::exportCsrGraphSnapshot(STADM.getDatabase());
  require(std::holds_alternative<csr::CsrGraphSnapshot>(actual) && std::holds_alternative<csr::CsrGraphSnapshot>(fresh),
          "targeted/fresh graph export rejected");
  require(std::get<csr::CsrGraphSnapshot>(actual) == std::get<csr::CsrGraphSnapshot>(fresh), "targeted graph differs from independent fresh GraphBuilder");
}
void fallbackOrderContract()
{
  auto fixture = makeNativeEditFixture();
  auto& database = fixture.database();
  Instance instance;
  instance.set_instance_name("unbound");
  instance.set_cell_name("MISSING_LIBRARY_CELL");
  instance.set_pin_name_list({"unbound:z", "unbound:a", "unbound:o"});
  for (const auto& port : {std::string("z"), std::string("a"), std::string("o")}) {
    Pin pin;
    pin.set_full_name("unbound:" + port);
    pin.set_pin_name(port);
    pin.set_instance_name("unbound");
    pin.set_direction(port == "o" ? PinDirection::kOutput : PinDirection::kInput);
    pin.set_is_port(false);
    database.get_pin_map().emplace(pin.get_full_name(), pin);
  }
  database.get_instance_map().emplace("unbound", std::move(instance));
  auto raw = NativeDatabaseCandidate::create(database);
  STADM.getDatabase() = raw.database();
  STAGB.buildStructure();
  auto baseline_owner = NativeDatabaseCandidate::createPreservingGraph(STADM.getDatabase());
  // Noncanonical pin order is outside the CSR reference input domain, but
  // representation permutation must still preserve native fallback semantics.
  NativeTargetedEditState metadata;
  for (std::size_t rank = 0; rank < baseline_owner.database().get_arc_list().size(); ++rank) {
    metadata.native_arc_ids.push_back(csr::ArcId{rank});
    metadata.arc_native_ranks.push_back(rank);
  }
  detail::canonicalizeTargetedNativeOrder(baseline_owner.database(), metadata);
  compareFresh(baseline_owner.database());
  const auto& row = baseline_owner.database().get_incoming_arc_list_map().at("unbound:o");
  require(row.size() == 2 && baseline_owner.database().get_arc_list().at(row[0]).get_source_pin() == "unbound:z",
          "unbound cell pin-list ordering was replaced by name sorting");
}

void runTargetedContracts()
{
  auto fixture = makeNativeEditFixture();
  // Retained load sorts after newly materialized input, exercising rank movement.
  auto retained = fixture.database().get_pin_map().at("out");
  retained.set_pin_name("aaa_out");
  retained.set_full_name("aaa_out");
  fixture.database().get_pin_map().emplace("aaa_out", retained);
  fixture.database().get_net_map().at("n0").get_pin_name_list().push_back("aaa_out");
  auto raw = NativeDatabaseCandidate::create(fixture.database());
  STADM.getDatabase() = raw.database();
  STAGB.buildStructure();
  auto baseline_owner = NativeDatabaseCandidate::createPreservingGraph(STADM.getDatabase());
  const auto baseline_input = input(baseline_owner.database());
  const auto baseline = baselineIdentity(baseline_owner.database(), TransactionGeneration{0}, TimingEditSessionToken{17});
  auto state = NativeTargetedEditPreparer::bindBaseline(baseline_owner.database(), baseline_input, baseline);
  auto plan = bufferInsertionPlan(baseline_owner.database(), baseline, TransactionGeneration{0}, editTestCreationToken());
  auto batch = NativeTargetedEditPreparer::prepare(baseline_owner.database(), plan, state);
  compareFresh(batch.candidate.database());
  require(batch.patch.arc_appends.size() == 3 && batch.patch.logical_deactivations.size() == 1,
          "targeted edit did not produce physical patch and logical replacement");
  require(batch.patch.changes.size() == 2, "new load preceding retained load did not replace both Base positions");
  auto stale = plan;
  stale.expected_generation = TransactionGeneration{1};
  rejects([&] { static_cast<void>(NativeTargetedEditPreparer::prepare(baseline_owner.database(), stale, state)); });
  // Library pointer ownership survives release of the original candidate.
  auto clone = NativeDatabaseCandidate::createPreservingGraph(batch.candidate.database());
  for (std::size_t i = 0; i < clone.database().get_arc_list().size(); ++i) {
    auto* copied = clone.database().get_arc_list()[i].get_timing_cell_arc();
    auto* original = batch.candidate.database().get_arc_list()[i].get_timing_cell_arc();
    if (original)
      require(copied && copied != original, "graph-preserving clone retained foreign library pointer");
  }
  batch.candidate = NativeDatabaseCandidate::create(fixture.database());
  compareFresh(clone.database());
  {
    auto clocked = NativeDatabaseCandidate::createPreservingGraph(clone.database());
    TimingClock clock;
    clock.set_clock_name("source_clock");
    clock.set_source_list({"in"});
    clock.set_period(10.0);
    clock.set_fall_edge(5.0);
    clocked.database().get_timing_constraint().get_clock_map().emplace("source_clock", clock);
    const auto clock_input = input(clocked.database());
    const auto clock_baseline = baselineIdentity(clocked.database(), TransactionGeneration{0}, TimingEditSessionToken{17});
    const auto clock_state = NativeTargetedEditPreparer::bindBaseline(clocked.database(), clock_input, clock_baseline);
    require(!clock_state.clock_arcs->empty(), "clock fixture did not export actual clock reachability");
    auto clock_plan = bufferInsertionPlan(clocked.database(), clock_baseline, TransactionGeneration{0}, editTestCreationToken(1, 1));
    clock_plan.edits.front().original_net = ExistingNetRef{batch.staged.resolver.materialization(editTestCreationToken()).output_net_name};
    bool rejected_clock = false;
    try {
      static_cast<void>(NativeTargetedEditPreparer::prepare(clocked.database(), clock_plan, clock_state));
    } catch (const std::invalid_argument& error) {
      rejected_clock = std::string(error.what()).find("propagated clock net") != std::string::npos;
      if (!rejected_clock)
        throw std::runtime_error(std::string("clock fixture rejected by unexpected gate: ") + error.what());
    }
    require(rejected_clock, "downstream clock net escaped baseline clock ArcId binding");
  }

  auto second = plan;
  second.expected_generation = TransactionGeneration{1};
  second.edits.front().creation_token = editTestCreationToken(2);
  second.edits.front().original_net = CreatedNetRef{editTestCreationToken(), CreatedNetRole::kOutputNet};
  auto next = NativeTargetedEditPreparer::prepare(clone.database(), second, batch.staged);
  compareFresh(next.candidate.database());
  // A legal cell may contain an additional disabled graph arc; this protocol
  // rejects it before mutation instead of dropping the native arc silently.
  auto unsupported = NativeDatabaseCandidate::createPreservingGraph(baseline_owner.database());
  auto& arcs = unsupported.database().get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list();
  auto disabled = arcs.front();
  disabled.set_is_disable_arc(true);
  arcs.push_back(disabled);
  rejects([&] { static_cast<void>(NativeTargetedEditPreparer::prepare(unsupported.database(), plan, state)); });
  require(unsupported.database().get_instance_map().empty(), "unsupported graph arc mutated candidate");
  std::cout << "native targeted: fresh canonical, consecutive batch, pointer lifetime, stale and disabled-arc rejection passed\n";
}
}  // namespace
int main()
{
  ista::Logger::initInst();
  ista::Utility::initInst();
  ista::DataManager::initInst();
  ista::GraphBuilder::initInst();
  int code = 0;
  try {
    runTargetedContracts();
    fallbackOrderContract();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    code = 1;
  }
  ista::GraphBuilder::destroyInst();
  ista::DataManager::destroyInst();
  ista::Utility::destroyInst();
  ista::Logger::destroyInst();
  return code;
}
