// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <algorithm>
#include <iostream>
#include "IncreGpuStaBuildConfig.hpp"
#include <stdexcept>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "DataManager.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualCsrSemanticIdentity.hpp"
#include "SemanticCsrNativeView.hpp"
#include "Instance.hpp"
#include "CanonicalGraphConstruction.hpp"
#include "NativeDatabaseValidation.hpp"
#include "Pin.hpp"
#include "TimingPoint.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "LogicExpressionTerm.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "DualCsrStateValidation.hpp"
#include "DualCsrValidationInternal.hpp"
#include "DualCsrStructuredBatchTestSupport.hpp"
#include "DualCsrUpdaterInternal.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingEditValidation.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingResultScatterInternal.hpp"

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
void directEdit(bool fanout, bool non_graph_prefix, double input_capacitance, bool partial_fanout, bool reordered = false)
{
  timing_edit::test::installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  database.get_timing_library().get_cell_map().at("BUF_X1").get_port_map().at("A").set_capacitance(input_capacitance);
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
  if (partial_fanout) fixture.plan.edits.front().moved_sinks.pop_back();
  if (reordered) {
    require(fanout, "reordered fixture needs retained fanout");
    auto& moved = fixture.plan.edits.front().moved_sinks;
    std::reverse(moved.begin(), moved.end());
    auto& base = fixture.graph.base_out;
    for (std::size_t row = 0; row + 1 < base.row_offsets.size(); ++row)
      std::reverse(base.physical_slots.begin() + base.row_offsets[row],
                   base.physical_slots.begin() + base.row_offsets[row + 1]);
    require(validateDualCsrState(fixture.graph).empty(), "reordered Base/Aux fixture is invalid");
  }
  auto malformed_graph = fixture.graph;
  require(!malformed_graph.base_out.physical_slots.empty(), "fixture has no outgoing arcs");
  malformed_graph.base_out.physical_slots.front().reset();
  std::vector<DualCsrArcGroupIndexBinding> bindings;
  for (std::size_t id = 0; id < fixture.arc_group_index.slots.size(); ++id)
    bindings.push_back({csr::ArcId{id}, fixture.arc_group_index.slots[id].ref});
  require(!buildDualCsrArcGroupIndex(malformed_graph, bindings).success(),
          "public index factory accepted an incomplete graph");

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
  auto pending = structured_support::prepare(fixture);
  auto direct = build(session, std::move(pending), models);
  require(!direct.captureAudit(), "direct CSR edit invented a native capture audit");
  require(csr::exportCsrGraphSnapshot(database) == native_before, "direct edit mutated the native Database");
  if constexpr (incregpusta_debug_checks_enabled) {
    auto native = structured_support::captureAndBuildVerifiedBatch(session, fixture);
    require(native.descriptor() == direct.descriptor(), "direct CSR descriptor differs from independent native candidate");
  }
  require(detail::validateConstructedDualCsrDescriptor(fixture.graph, direct.descriptor()).empty(),
          "controlled descriptor failed applicability checks");
  for (unsigned mutation = 0; mutation != 6; ++mutation) {
    auto malformed = direct.descriptor();
    switch (mutation) {
      case 0: malformed.expected_generation = timing_edit::TransactionGeneration{9}; break;
      case 1: malformed.before.arc_slots += 1; break;
      case 2: malformed.logical_deactivations.front().expected_active = false; break;
      case 3: malformed.reverse_changes.front().expected_arc = csr::ArcId{malformed.after.arc_slots}; break;
      case 4: malformed.node_appends.front().id = csr::NodeId{malformed.after.node_slots}; break;
      case 5: malformed.reference_payload.source_identity.reset(); break;
    }
    require(!detail::validateConstructedDualCsrDescriptor(fixture.graph, malformed).empty(),
            "controlled descriptor accepted stale, malformed or mismatched old values");
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
void addParallelLibraryInstance(Database& database)
{
  auto& children = database.get_timing_library().get_cell_map().at("INV_X1").get_cell_arc_list();
  auto duplicate = children.front();
  duplicate.set_delay(0.07);
  duplicate.set_delay_min(0.06);
  duplicate.set_delay_max(0.08);
  children.push_back(std::move(duplicate));
  Instance instance;
  instance.set_instance_name("parallel");
  instance.set_cell_name("INV_X1");
  instance.get_output_pin_name() = "parallel:Y";
  instance.set_pin_name_list({"parallel:A", "parallel:Y"});
  database.get_instance_map().emplace("parallel", std::move(instance));
  for (const auto& port : {std::string("A"), std::string("Y")}) {
    const std::string name = "parallel:" + port;
    Pin pin;
    pin.set_full_name(name);
    pin.set_instance_name("parallel");
    pin.set_pin_name(port);
    pin.set_direction(port == "A" ? PinDirection::kInput : PinDirection::kOutput);
    pin.set_net_name(port == "A" ? "n0" : "parallel_out");
    database.get_pin_map().emplace(name, std::move(pin));
    database.get_timing_point_map()[name].set_level(port == "A" ? 2 : 3);
    database.get_timing_order_list().push_back(name);
  }
  auto& input = database.get_net_map().at("n0");
  input.get_pin_name_list().push_back("parallel:A");
  canonical_graph::refreshNetDriverLoad(database, input);
  Net output;
  output.set_net_name("parallel_out");
  output.set_pin_name_list({"parallel:Y"});
  canonical_graph::refreshNetDriverLoad(database, output);
  database.get_net_map().emplace("parallel_out", std::move(output));
  canonical_graph::appendArc(database, canonical_graph::makeNetArc(database, "in", "parallel:A", ArcType::kNet, "n0"));
  require(canonical_graph::appendLibraryCellArcs(database, database.get_instance_map().at("parallel")),
          "parallel fixture did not construct its library arcs");
}
void lazySemanticView(bool move_created_input, bool parallel = false, std::uint64_t inactive_ordinal = 0)
{
  using namespace timing_edit;
  timing_edit::test::installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  if (parallel) {
    addParallelLibraryInstance(database);
    timing_edit::requireValidNativeDatabase(database);
    const auto exported = csr::exportCsrGraphSnapshot(database);
    if (const auto* failure = std::get_if<csr::CsrGraphExportFailure>(&exported))
      throw std::runtime_error("parallel graph export: " + std::string(csr::token(failure->code)));
    const auto reference = csr::exportCsrReferenceInputCandidate(database, timing_edit::test::editTestAnalysisContext(), 31337);
    if (const auto* failure = std::get_if<csr::CsrReferenceInputExportFailure>(&reference))
      throw std::runtime_error("parallel reference export: " + std::string(csr::token(failure->code)) + " phase=" + std::to_string(static_cast<int>(failure->failed_phase)));
  }
  timing_edit::test::GuardedTestSession session;
  auto fixture = structured_support::makeStructuredFixture(database, session.token());
  csr::detail::NativeLibraryModels models(database);
  csr::CsrTimingValues timing;
  SemanticCsrNativeView view(session.token());
  std::vector<EditBatch> history;
  const auto commit = [&] {
    const auto generation = fixture.graph.generation;
    auto edit = fixture.plan.edits.front();
    edit.creation_token.batch_ordinal = generation.value() + 1;
    if (!history.empty()) {
      const auto previous = history.back().edits.front().creation_token;
      if (move_created_input)
        edit.moved_sinks = {CreatedNodeRef{previous, CreatedNodeRole::kInput}};
      else
        edit.original_net = CreatedNetRef{previous, CreatedNetRole::kOutputNet};
    }
    EditBatch edits{generation, {std::move(edit)}};
    history.reserve(history.size() + 1);
    auto plan = fixture.plan;
    plan.expected_generation = generation;
    plan.edits = edits.edits;
    auto direct = build(session, DualCsrStructuredBatchPreparer::prepare(
        plan, fixture.resolver, DualSemanticReadView(fixture.graph), fixture.graph,
        fixture.identity, fixture.arc_group_index), models);
    detail::PreparedDualCsrGraphPatch graph_patch(fixture.graph, direct.descriptor());
    detail::PreparedDualCsrIndexPatch index_patch(fixture.graph, fixture.arc_group_index, direct.descriptor());
    PreparedTimingEditResolverPatch resolver_patch(fixture.resolver, fixture.resolver.prepare(direct.preparedBatch()));
    graph_patch.apply();
    index_patch.apply();
    resolver_patch.apply();
    // The component under test consumes timing; its producer is independent.
    auto computed = csr::CsrCpuReferenceBackend{}.computeWithTelemetry(
        DualCsrEdgeViewImpl(fixture.graph), DualCsrTimingPayloadViewImpl(fixture.graph));
    auto* values = std::get_if<csr::CsrTimingResult>(&computed.computation.outcome);
    require(values != nullptr, "semantic lazy-view fixture timing failed");
    auto identity = deriveDualCsrCommittedIdentity(fixture.graph, fixture.resolver);
    graph_patch.commit();
    index_patch.commit();
    resolver_patch.commit();
    fixture.identity = std::move(identity);
    timing = std::move(*values);
    history.push_back(std::move(edits));
  };
  const auto original = csr::exportCsrGraphSnapshot(database);
  commit();
  if (parallel) {
    auto deleted = std::ranges::find_if(fixture.graph.arc_slots, [](const auto& slot) { return !slot.active; });
    const auto peer = std::ranges::find_if(fixture.graph.arc_slots, [](const auto& slot) {
      return slot.active && slot.payload.key.base.owner_name == "parallel" && slot.payload.key.parallel_ordinal == 0;
    });
    require(deleted != fixture.graph.arc_slots.end() && peer != fixture.graph.arc_slots.end(),
            "inactive-peer fixture lacks a tombstone or live parallel arc");
    const csr::ArcId deleted_id{static_cast<std::uint64_t>(deleted - fixture.graph.arc_slots.begin())};
    const auto identity = deriveDualCsrCommittedIdentity(fixture.graph, fixture.resolver);
    deleted->payload = peer->payload;
    deleted->payload.key.parallel_ordinal = inactive_ordinal;
    const DualSemanticReadView semantic(fixture.graph);
    const auto inactive = semantic.arcById(deleted_id);
    const auto active = semantic.arcByKey(peer->payload.key);
    const auto* indexed = fixture.arc_group_index.slot(deleted_id);
    require(!inactive && inactive.error() == DualSemanticReadError::kInactiveArc
                && active && &active.value() == &peer->payload
                && indexed && !indexed->current_active_key
                && std::ranges::any_of(fixture.resolver.arcBindings(), [&](const auto& item) {
                     return item.second.slot.value() == deleted_id.value() && item.second.tombstone;
                   })
                && deriveDualCsrCommittedIdentity(fixture.graph, fixture.resolver) == identity,
            "inactive same-base payload changed the live identity or resolver");
  }
  commit();
  require(csr::exportCsrGraphSnapshot(database) == original, "no-query semantic commits changed native graph");
  const auto materialize = [&](const csr::CsrTimingValues& values) {
    const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
        session.session(), [&](std::uint64_t, StaCsrSessionContext&, const NativeExecutionAuthority& authority) {
          view.materialize(database, authority, fixture.graph, fixture.resolver, history, values);
          return StaStatus::success(session.session().state());
        });
    return status.ok();
  };
  auto malformed = timing;
  malformed.nodes.slacks.values.pop_back();
  require(!materialize(malformed), "semantic view accepted malformed final timing");
  require(csr::exportCsrGraphSnapshot(database) == original,
          "failed two-batch native view left a partially applied suffix");
  require(materialize(timing), "semantic view could not retry after final scatter failure");
  const auto two_batches = csr::exportCsrGraphSnapshot(database);
  require(two_batches != original, "successful semantic view did not publish new topology");
  if constexpr (incregpusta_debug_checks_enabled)
  for (unsigned mutation = 0; mutation != 3; ++mutation) {
    auto bad_identity = fixture.graph;
    const auto active = std::ranges::find_if(bad_identity.arc_slots, [](const auto& slot) { return slot.active; });
    require(active != bad_identity.arc_slots.end(), "semantic view fixture has no active arc");
    switch (mutation) {
      case 0: ++active->payload.key.parallel_ordinal; break;
      case 1: active->payload.key.base.owner_name += "_wrong"; break;
      case 2: active->payload.key.base.library_source_port += "_wrong"; break;
    }
    bool bad_identity_rejected = false;
    const auto check = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
        session.session(), [&](std::uint64_t, StaCsrSessionContext&, const NativeExecutionAuthority&) {
          try {
            static_cast<void>(csr::detail::prepareDualCsrTimingResult(
                database, bad_identity, timing));
          } catch (const std::exception&) {
            bad_identity_rejected = true;
          }
          return StaStatus::success(session.session().state());
        });
    require(check.ok() && bad_identity_rejected && csr::exportCsrGraphSnapshot(database) == two_batches,
            "native view accepted altered arc identity or published a partial scatter");
  }
  require(materialize(timing)
              && csr::exportCsrGraphSnapshot(database) == two_batches, "same-generation semantic view changed its graph");
  commit();
  require(csr::exportCsrGraphSnapshot(database) == two_batches, "edit after query eagerly changed the native view");
  require(materialize(timing), "semantic view failed after query and another edit");
  require(database.get_pin_map().size() == fixture.graph.nodes.size(), "semantic final view has stale nodes");
  for (const auto& node : fixture.graph.nodes)
    require(database.get_pin_map().at(node.key.full_name).get_net_name() == node.net_name,
            "semantic final native connection differs from committed CSR");
  if (parallel) {
    const auto& children = database.get_timing_library().get_cell_map().at("INV_X1").get_cell_arc_list();
    std::vector<const csr::ArcSemanticPayload*> payloads;
    for (std::size_t id = 0; id < fixture.graph.arc_slots.size(); ++id) {
      const auto& slot = fixture.graph.arc_slots[id];
      if (!slot.active || slot.payload.key.base.owner_name != "parallel") continue;
      const auto& payload = slot.payload;
      payloads.push_back(&payload);
      const auto* child = &children.at(*payload.native_cell_arc_ordinal);
      const auto found = std::ranges::find_if(database.get_arc_list(), [&](const auto& arc) {
        return arc.get_owner_name() == "parallel" && arc.get_timing_cell_arc() == child;
      });
      require(found != database.get_arc_list().end(), "native query lost a parallel library arc");
      const auto& values = timing.arcs;
      require(found->get_delay() == values.delays.values.at(id)
                  && found->get_delay_min() == values.min_delays.values.at(id)
                  && found->get_delay_max() == values.max_delays.values.at(id),
              "parallel child result mapped to another native arc");
    }
    require(payloads.size() == children.size() && payloads.size() > 1
                && payloads.front()->key.base == payloads.back()->key.base
                && payloads.front()->native_cell_arc_ordinal != payloads.back()->native_cell_arc_ordinal,
            "fixture did not exercise distinct children sharing canonical signatures");
  }
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
        for (double capacitance : {0.0, 3.0})
          for (bool partial : {false, true})
            ista::timing_dual_csr::test::directEdit(fanout, non_graph, capacitance, partial);
    ista::timing_dual_csr::test::directEdit(true, false, 3.0, true, true);
    ista::timing_dual_csr::test::directEdit(true, false, 0.123456789, false, true);
    ista::timing_dual_csr::test::fullNameCollision();
    ista::timing_dual_csr::test::sinkPolicy();
    ista::timing_dual_csr::test::lazySemanticView(false);
    ista::timing_dual_csr::test::lazySemanticView(true);
    for (std::uint64_t ordinal : {0, 97}) {
      ista::timing_dual_csr::test::lazySemanticView(false, true, ordinal);
      ista::timing_dual_csr::test::lazySemanticView(true, true, ordinal);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
