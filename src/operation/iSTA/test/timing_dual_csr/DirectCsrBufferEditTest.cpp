// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <iostream>
#include <stdexcept>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "DataManager.hpp"
#include "CpuDualCsrBaseline.hpp"
#include "DualCsrTransactionCoordinator.hpp"
#include "SemanticCsrNativeView.hpp"
#include "Instance.hpp"
#include "Pin.hpp"
#include "TimingPoint.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "LogicExpressionTerm.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "DualCsrStateValidation.hpp"
#include "DualCsrStructuredBatchTestSupport.hpp"
#include "DualCsrUpdaterInternal.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingEditValidation.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::timing_dual_csr::test {
namespace {
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
VerifiedStructuredBatch build(timing_edit::test::GuardedTestSession& session,
                              PendingStructuredBatch pending, csr::detail::NativeLibraryModels& models)
{
  std::optional<VerifiedStructuredBatch> result;
  const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
      session.session(), [&](std::uint64_t, StaCsrSessionContext&, const timing_edit::NativeExecutionAuthority& authority) {
        result.emplace(TimingEditDualCsrDescriptorBuilder::buildDirect(std::move(pending), authority, models));
        return StaStatus::success(session.session().state());
      });
  if (!status.ok()) throw std::invalid_argument(status.message);
  require(result.has_value(), "guarded direct edit produced no result");
  return std::move(*result);
}
void directEdit(bool fanout, bool non_graph_prefix)
{
  timing_edit::test::installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  if (non_graph_prefix) {
    auto& arcs = database.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list();
    auto other = arcs.front();
    other.set_is_timing_graph_arc(false);
    other.set_is_disable_arc(true);
    arcs.insert(arcs.begin(), std::move(other));
  }
  timing_edit::test::GuardedTestSession session;
  auto fixture = fanout ? structured_support::makeStructuredFanoutFixture(database, session.token())
                        : structured_support::makeStructuredFixture(database, session.token());
  const auto native_before = csr::exportCsrGraphSnapshot(database);
  csr::detail::NativeLibraryModels models(database);
  auto foreign = timing_edit::test::makeNativeEditFixture();
  foreign.database().get_timing_library().get_cell_map().at("BUF_X1").get_port_map().at("A").set_capacitance(3.0);
  csr::detail::NativeLibraryModels foreign_models(foreign.database());
  bool foreign_rejected = false;
  try { static_cast<void>(build(session, structured_support::prepare(fixture), foreign_models)); }
  catch (const std::invalid_argument& error) {
    foreign_rejected = std::string(error.what()).find("another Database") != std::string::npos;
  }
  require(foreign_rejected, "direct edit accepted models from another Database");
  auto direct = build(session, structured_support::prepare(fixture), models);
  require(!direct.captureAudit(), "direct CSR edit invented a native capture audit");
  require(csr::exportCsrGraphSnapshot(database) == native_before, "direct edit mutated the native Database");
  if constexpr (incregpusta_debug_checks_enabled) {
    auto native = structured_support::captureAndBuildVerifiedBatch(session, fixture);
    require(native.descriptor() == direct.descriptor(), "direct CSR descriptor differs from independent native candidate");
  }
  const auto before = fixture.graph;
  const auto resolver_before = fixture.resolver;
  const auto index_before = fixture.arc_group_index;
  {
    detail::PreparedDualCsrGraphPatch graph_patch(fixture.graph, direct.descriptor());
    detail::PreparedDualCsrIndexPatch index_patch(fixture.graph, fixture.arc_group_index, direct.descriptor());
    timing_edit::PreparedTimingEditResolverPatch resolver_patch(
        fixture.resolver, fixture.resolver.prepare(direct.preparedBatch()));
    graph_patch.apply();
    index_patch.apply();
    resolver_patch.apply();
    require(validateDualCsrState(fixture.graph).empty(), "direct edit produced an invalid graph");
    require(validateDualCsrArcGroupIndex(fixture.graph, fixture.arc_group_index).empty(), "direct edit produced an invalid index");
    require(fixture.graph.generation.value() == 1
                && fixture.graph.nodes.size() == before.nodes.size() + 2,
            "direct edit did not apply the buffer");
    resolver_patch.rollback();
    index_patch.rollback();
    graph_patch.rollback();
  }
  require(fixture.graph.nodes == before.nodes && fixture.graph.arc_slots == before.arc_slots
              && fixture.graph.reference_payload == before.reference_payload
              && fixture.resolver == resolver_before && fixture.arc_group_index == index_before,
          "direct edit rollback did not restore the actual old values");
  auto stale = structured_support::prepare(fixture);
  fixture.graph.generation = timing_edit::TransactionGeneration{9};
  bool rejected = false;
  try { static_cast<void>(build(session, std::move(stale), models)); }
  catch (const std::invalid_argument&) { rejected = true; }
  fixture.graph.generation = before.generation;
  require(rejected, "direct edit accepted stale preparation");
  require(session.closeAndRelease().ok(), "direct edit session close failed");
}
void lazySemanticView()
{
  using namespace timing_edit;
  timing_edit::test::installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  timing_edit::test::GuardedTestSession session;
  auto fixture = structured_support::makeStructuredFixture(database, session.token());
  csr::detail::NativeLibraryModels models(database);
  DualCsrTransactionCoordinator coordinator(makeCpuDualCsrBaseline(database, fixture.baseline_reference_input, session.token()));
  SemanticCsrNativeView view(session.token());
  std::vector<EditBatch> history;
  const auto commit = [&] {
    const auto generation = coordinator.committed().generation;
    auto edit = fixture.plan.edits.front();
    edit.creation_token.batch_ordinal = generation.value() + 1;
    if (!history.empty())
      edit.original_net = CreatedNetRef{history.back().edits.front().creation_token, CreatedNetRole::kOutputNet};
    history.push_back({generation, {std::move(edit)}});
    auto direct = build(session, coordinator.prepareBatch(history.back()), models);
    auto attempt = makeStagedDualCsrAnalysisState(coordinator, std::move(direct));
    require(attempt.success(), "semantic lazy-view CSR computation failed");
    const auto published = coordinator.publish(std::move(*attempt.staged));
    require(published.ok() && published.published, "semantic lazy-view CSR commit failed");
  };
  const auto original = csr::exportCsrGraphSnapshot(database);
  commit();
  commit();
  require(csr::exportCsrGraphSnapshot(database) == original, "no-query semantic commits changed native graph");
  const auto materialize = [&](const csr::CsrTimingValues& values) {
    const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
        session.session(), [&](std::uint64_t, StaCsrSessionContext&, const NativeExecutionAuthority& authority) {
          const auto& current = coordinator.committed();
          view.materialize(database, authority, current.graph, current.resolver, history, values);
          return StaStatus::success(session.session().state());
        });
    return status.ok();
  };
  auto malformed = coordinator.committed().timing.values;
  malformed.nodes.slacks.values.pop_back();
  require(!materialize(malformed), "semantic view accepted malformed final timing");
  require(csr::exportCsrGraphSnapshot(database) == original,
          "failed two-batch native view left a partially applied suffix");
  require(materialize(coordinator.committed().timing.values), "semantic view could not retry after final scatter failure");
  const auto two_batches = csr::exportCsrGraphSnapshot(database);
  require(two_batches != original, "successful semantic view did not publish new topology");
  require(materialize(coordinator.committed().timing.values)
              && csr::exportCsrGraphSnapshot(database) == two_batches, "same-generation semantic view changed its graph");
  commit();
  require(csr::exportCsrGraphSnapshot(database) == two_batches, "edit after query eagerly changed the native view");
  require(materialize(coordinator.committed().timing.values), "semantic view failed after query and another edit");
  const auto& current = coordinator.committed();
  require(database.get_pin_map().size() == current.graph.nodes.size(), "semantic final view has stale nodes");
  for (const auto& node : current.graph.nodes)
    require(database.get_pin_map().at(node.key.full_name).get_net_name() == node.net_name,
            "semantic final native connection differs from committed CSR");
  require(session.closeAndRelease().ok(), "semantic lazy-view session close failed");
}
void sinkPolicy()
{
  using namespace timing_edit;
  for (int scenario = 0; scenario != 8; ++scenario) {
    TimingCell cell;
    cell.set_cell_name("sink");
    TimingCellPort port;
    port.set_port_name("D");
    port.set_is_input(true);
    cell.get_port_map().emplace("D", port);
    csr::NodeRecord node;
    node.key.pin_name = "D";
    node.direction = csr::PinDirection::kInput;
    if (scenario >= 4) {
      cell.set_is_sequential(true);
      node.is_sequential = true;
      node.is_data_pin = true;
      TimingCheckArc check;
      check.set_clock_port("CK");
      check.set_data_port("D");
      check.set_check_type(TimingCheckType::kSetup);
      cell.get_check_arc_list().push_back(check);
    }
    switch (scenario) {
      case 1: cell.set_is_macro(true); break;
      case 2: cell.set_is_clock_gating(true); break;
      case 3: {
        TimingCellArc control;
        control.set_source_port("D");
        control.set_is_clear_preset_arc(true);
        cell.get_cell_arc_list().push_back(control);
        break;
      }
      case 5: node.is_data_pin = false; break;
      case 6: cell.get_check_arc_list().front().set_check_type(TimingCheckType::kRecovery); break;
      case 7: node.is_sequential = false; break;
    }
    bool accepted = true;
    try { timing_edit::detail::requireSupportedMovedSink(node, cell); }
    catch (const std::invalid_argument&) { accepted = false; }
    require(accepted == (scenario == 0 || scenario == 4),
            "CSR sink selection changed ordinary-data/control/macro semantics");
  }
}
void fullNameCollision()
{
  using namespace timing_edit::test;
  installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  auto& cell = database.get_timing_library().get_cell_map().at("BUF_X1");
  auto port = cell.get_port_map().extract("A");
  port.key() = "A:B:C";
  port.mapped().set_port_name("A:B:C");
  cell.get_port_map().insert(std::move(port));
  cell.get_cell_arc_list().front().set_source_port("A:B:C");
  LogicExpressionTerm term;
  term.set_operation_type(LogicOperationType::kPort);
  term.set_port_name("A:B:C");
  cell.get_port_map().at("Y").get_function_expression().set_term_list({term});
  const auto names = timing_edit::defaultTokenMaterialization(editTestCreationToken(), nativeLibraryCellRef(database, "BUF_X1"));
  TimingCell other;
  other.set_cell_name("COLLISION_X");
  other.set_library_name("slow.lib");
  TimingCellPort input;
  input.set_port_name("B:C");
  input.set_is_input(true);
  other.get_port_map().emplace("B:C", input);
  database.get_timing_library().get_cell_map().emplace("COLLISION_X", std::move(other));
  Instance instance;
  instance.set_instance_name(names.instance_name + ":A");
  instance.set_cell_name("COLLISION_X");
  instance.set_pin_name_list({names.input_pin_name});
  Pin pin;
  pin.set_instance_name(instance.get_instance_name());
  pin.set_pin_name("B:C");
  pin.set_full_name(names.input_pin_name);
  pin.set_direction(PinDirection::kInput);
  database.get_instance_map().emplace(instance.get_instance_name(), std::move(instance));
  database.get_pin_map().emplace(names.input_pin_name, std::move(pin));
  database.get_timing_point_map().emplace(names.input_pin_name, TimingPoint{});
  database.get_timing_order_list().push_back(names.input_pin_name);
  GuardedTestSession session;
  auto fixture = structured_support::makeStructuredFixture(database, session.token());
  csr::detail::NativeLibraryModels models(database);
  bool rejected = false;
  try { static_cast<void>(build(session, structured_support::prepare(fixture), models)); }
  catch (const std::invalid_argument& error) {
    rejected = std::string(error.what()).find("materialized pin name collides") != std::string::npos;
  }
  require(rejected, "direct edit missed full-name collision across colon-containing instance/port names");
  require(session.closeAndRelease().ok(), "collision session close failed");
}
}
}
int main()
{
  try {
    ista::timing_edit::test::NativeTransactionRuntime runtime;
    for (bool fanout : {false, true})
      for (bool non_graph : {false, true})
        ista::timing_dual_csr::test::directEdit(fanout, non_graph);
    ista::timing_dual_csr::test::fullNameCollision();
    ista::timing_dual_csr::test::sinkPolicy();
    ista::timing_dual_csr::test::lazySemanticView();
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
