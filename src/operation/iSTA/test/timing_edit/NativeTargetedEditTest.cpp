#include "CanonicalTimingRecordProjectorInternal.hpp"
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include "../AllocationFailureSupport.hpp"
#include "DataManager.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "Instance.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeFullEditPreparer.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "NativeTargetedEditInternal.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingClock.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingGraphCsrAdapter.hpp"

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
  try { work(); } catch (const std::invalid_argument&) { return; }
  throw std::runtime_error("expected native targeted rejection");
}
csr::CsrReferenceInput input(const Database& database, TimingEditSessionToken token)
{
  auto result = csr::exportCsrReferenceInputCandidate(database, editTestAnalysisContext(), token.value());
  require(std::holds_alternative<csr::CsrReferenceInput>(result), "test reference export failed");
  return std::get<csr::CsrReferenceInput>(std::move(result));
}
csr::CsrGraphSnapshot graph(const Database& database)
{
  auto result = csr::exportCsrGraphSnapshot(database);
  require(std::holds_alternative<csr::CsrGraphSnapshot>(result), "test graph export failed");
  auto value = std::get<csr::CsrGraphSnapshot>(std::move(result));
  // Compare all semantic fields and ordered rows, excluding physical slots.
  for (auto& arc : value.arcs)
    arc.native_traversal_rank = 0;
  return value;
}
csr::CsrGraphSnapshot freshGraph(const NativeExecutionAuthority& authority,
                                  const DesignEditPlan& plan, const NativeTargetedEditState& state,
                                  csr::detail::NativeLibraryModels& models)
{
  auto& database = STADM.getDatabase();
  const auto identity = NativeTimingEditPreparer::captureCurrentIdentity(
      database, state.generation, editTestAnalysisContext(), authority.sessionToken());
  TimingEditPrepareContext context{authority.sessionToken(), plan.baseline, identity, editTestAnalysisContext()};
  context.library_models = &models;
  auto prepared = NativeTimingEditPreparer::prepare(database, plan, context, state.resolver);
  auto resolver = state.resolver.stage(prepared);
  auto transaction = NativeEditTransaction::prepareFull(authority, prepared, resolver);
  NativeCandidateExecutor::prepareFullGraph(transaction);
  const auto exported = input(transaction.database(), authority.sessionToken());
  require(NativeTimingEditPreparer::identityFromInput(transaction.database(), exported, prepared.targetGeneration())
              == NativeTimingEditPreparer::captureCurrentIdentity(transaction.database(), prepared.targetGeneration(),
                                                                   editTestAnalysisContext(), authority.sessionToken()),
          "identity from the computed full input differs from an independent export");
  auto expected = graph(transaction.database());
  require(transaction.rollback(), "independent full graph rollback failed");
  return expected;
}
void requireArcComparatorOrder(const TimingEditResolverState& resolver)
{
  std::vector<StableArcRef> refs;
  for (const auto& [ref, binding] : resolver.arcBindings()) {
    static_cast<void>(binding);
    refs.push_back(ref);
    if (const auto* cell = std::get_if<CreatedCellArcRef>(&ref)) {
      for (unsigned field = 0; field < 8; ++field) {
        auto changed = *cell;
        auto& signature = changed.ordered_library_arcs.at(0);
        switch (field) {
          case 0: signature.library_name += "-long"; break;
          case 1: signature.arc_index = -1; break;
          case 2: signature.arc_index = 256; break;
          case 3: signature.sense = csr::TimingSense::kNonUnate; break;
          case 4: signature.delay_tables[0].variable1 = csr::TableVariable::kOutputCapacitance; break;
          case 5: signature.slew_tables[1].present = !signature.slew_tables[1].present; break;
          case 6: signature.sdf_condition = std::string(1, static_cast<char>(0xff)); break;
          case 7: changed.ordered_library_arcs.push_back(signature); break;
        }
        refs.push_back(std::move(changed));
      }
    } else if (const auto* net = std::get_if<CreatedNewNetArcRef>(&ref)) {
      auto changed = *net;
      changed.driver = ExistingNodeRef{{csr::NodeKind::kPort, "z", {}, {}}};
      refs.push_back(changed);
      changed.driver = ExistingNodeRef{{csr::NodeKind::kPort, "aaaa", {}, {}}};
      refs.push_back(std::move(changed));
    }
  }
  for (const auto& left : refs)
    for (const auto& right : refs) {
      if (left.index() != right.index() || left.index() == 0)
        continue;
      const auto expected = canonicalStableArcRefBytes(left) < canonicalStableArcRefBytes(right);
      ista_test_allocation::armCountOnly();
      const auto actual = left < right;
      const auto allocations = ista_test_allocation::disable();
      require(expected == actual && allocations == 0, "stable Arc ordering allocated or changed");
    }
}
void parallelArcProjection()
{
  installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  GuardedTestSession session;
  const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
      session.session(), [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
        requireReleasedSnapshotForNativeMutation(context, authority.sessionToken());
        auto prepared = prepareNativeTransaction(authority.sessionToken());
        auto resolver = prepared.resolver.stage(prepared.prepared);
        auto inserted = NativeEditTransaction::prepareFull(authority, prepared.prepared, resolver);
        NativeCandidateExecutor::prepareFullGraph(inserted);
        inserted.commit();
        auto cell = database.get_timing_library().get_cell_map().at("BUF_X1");
        cell.set_cell_name("PAR_BUF_X1");
        cell.get_cell_arc_list().push_back(cell.get_cell_arc_list().front());
        database.get_timing_library().get_cell_map().emplace("PAR_BUF_X1", std::move(cell));
        database.get_instance_map().begin()->second.set_cell_name("PAR_BUF_X1");
        auto rebuild = NativeEditTransaction::prepareFull(authority);
        NativeCandidateExecutor::prepareFullGraph(rebuild);
        rebuild.commit();
        const auto exported = input(database, authority.sessionToken());
        const auto baseline = baselineIdentity(database, TransactionGeneration{0}, authority.sessionToken());
        csr::detail::NativeLibraryModels models(database);
        const auto state = NativeTargetedEditPreparer::bindBaseline(database, exported, baseline, models);
        const std::set<std::string> starts(database.get_start_point_list().begin(), database.get_start_point_list().end());
        const std::set<std::string> ends(database.get_end_point_list().begin(), database.get_end_point_list().end());
        std::map<std::string, csr::ArcOutputLoadMatrix> loads;
        require(std::ranges::any_of(exported.arcs, [](const auto& arc) { return arc.key.parallel_ordinal != 0; }),
                "targeted projection fixture lacks parallel identities");
        for (const auto& arc : exported.arcs) {
          const auto actual = ista::timing_edit::detail::targetedArc(database, state, arc.native_traversal_rank, loads, starts, ends);
          require(actual.key == arc.key, "targeted projection changed a canonical parallel arc identity");
        }
        return StaStatus::success(session.session().state());
      });
  if (!status.ok())
    throw std::runtime_error(status.message);
  require(session.closeAndRelease().ok(), "parallel arc projection cleanup failed");
}
void runTargetedContracts()
{
  installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  GuardedTestSession session;
  const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
      session.session(), [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
        requireReleasedSnapshotForNativeMutation(context, authority.sessionToken());
        auto retained = database.get_pin_map().at("out");
        retained.set_pin_name("aaa_out");
        retained.set_full_name("aaa_out");
        database.get_pin_map().emplace("aaa_out", retained);
        database.get_net_map().at("n0").get_pin_name_list().push_back("aaa_out");
        auto rebuild = NativeEditTransaction::prepareFull(authority);
        NativeCandidateExecutor::prepareFullGraph(rebuild);
        rebuild.commit();
        const auto baseline_input = input(database, authority.sessionToken());
        const auto baseline = baselineIdentity(database, TransactionGeneration{0}, authority.sessionToken());
        csr::detail::NativeLibraryModels models(database);
        const std::string buffer_name = "BUF_X1";
        const auto& cached_cell = models.cell(buffer_name);
        const auto& native_model = database.get_timing_library().get_cell_map().at(buffer_name).get_cell_arc_list().front();
        const auto cached_ordinal = models.arcOrdinal(buffer_name, native_model);
        ista_test_allocation::armCountOnly();
        bool same_models = true;
        for (unsigned i = 0; i < 100; ++i)
          same_models = same_models && &models.cell(buffer_name) == &cached_cell
                        && models.arcOrdinal(buffer_name, native_model) == cached_ordinal;
        const auto model_allocations = ista_test_allocation::disable();
        require(same_models && model_allocations == 0, "cached model lookup allocated or projected another table");
        for (unsigned field = 0; field < 4; ++field) {
          auto foreign = baseline;
          switch (field) {
            case 0: foreign.design_name += "_foreign"; break;
            case 1: foreign.fingerprints.library.assign(64, 'a'); break;
            case 2: foreign.canonical_pre_state.instances.push_back("foreign_instance"); break;
            case 3: foreign.canonical_pre_state.arcs.pop_back(); break;
          }
          rejects([&] { static_cast<void>(NativeTargetedEditPreparer::bindBaseline(database, baseline_input, foreign, models)); });
        }
        auto state = NativeTargetedEditPreparer::bindBaseline(database, baseline_input, baseline, models);
        auto plan = bufferInsertionPlan(database, baseline, TransactionGeneration{0}, editTestCreationToken());
        const auto* library = &database.get_timing_library();
        const auto* cell = &library->get_cell_map().at("BUF_X1");
        const auto retained_slot = database.get_incoming_arc_list_map().at("aaa_out").front();
        database.get_timing_point_map().at("aaa_out").set_predecessor_arc_idx(retained_slot);
        const auto baseline_graph = graph(database);
        const auto baseline_resolver = state.resolver;
        const auto baseline_names = state.node_names;
        const auto baseline_ids = state.node_ids;
        const auto baseline_native_ids = state.native_arc_ids;
        const auto baseline_ranks = state.arc_native_ranks;
        const auto metadata_restored = [&] {
          return state.generation.value() == 0 && state.resolver == baseline_resolver
                 && state.node_names == baseline_names && state.node_ids == baseline_ids
                 && state.native_arc_ids == baseline_native_ids && state.arc_native_ranks == baseline_ranks;
        };
        {
          auto batch = NativeTargetedEditPreparer::prepare(database, authority, {plan.expected_generation, plan.edits}, state);
          ista_test_allocation::armCountOnly();
          const bool restored = batch.transaction.rollback();
          const auto allocations = ista_test_allocation::disable();
          require(restored && allocations == 0 && metadata_restored() && graph(database) == baseline_graph,
                  "targeted local rollback allocated or changed original graph");
        }
        {
          auto abandoned = NativeTargetedEditPreparer::prepare(database, authority, {plan.expected_generation, plan.edits}, state);
          static_cast<void>(abandoned);
        }
        require(graph(database) == baseline_graph && metadata_restored() && authority.active(), "abandoned targeted edit retained mutation or ownership");
        for (std::uint64_t generation = 0; generation < 2; ++generation) {
          const auto expected = freshGraph(authority, plan, state, models);
          const auto saved_graph = graph(database);
          const auto fields = [](const NativeTargetedEditState& value) {
            return std::tuple{value.session_token, value.generation, value.baseline, value.clock_arcs, value.resolver,
                              value.node_names, value.node_ids, value.arc_refs, value.native_arc_ids, value.arc_native_ranks,
                              value.base_out_offsets, value.base_in_offsets};
          };
          const auto saved_state = fields(state);
          if constexpr (incregpusta_debug_checks_enabled) {
            struct BeforeNativeApply {
              const NativeTargetedEditState& state;
              const Database& database;
              std::uint64_t generation;
              std::size_t instances;
            } boundary{state, database, generation, database.get_instance_map().size()};
            ista_test_allocation::armFailureWhen([](void* raw) noexcept {
              const auto& value = *static_cast<BeforeNativeApply*>(raw);
              return value.state.generation.value() != value.generation
                     && value.database.get_instance_map().size() == value.instances;
            }, &boundary);
            bool failed = false;
            try {
              auto attempt = NativeTargetedEditPreparer::prepare(database, authority, {plan.expected_generation, plan.edits}, state);
            } catch (const std::bad_alloc&) {
              failed = true;
            }
            static_cast<void>(ista_test_allocation::disable());
            require(!failed && fields(state) == saved_state && graph(database) == saved_graph && authority.readyForQuery(),
                    "targeted publication allocated between metadata and native apply");
            ista_test_allocation::armFailureWhen([](void* raw) noexcept {
              const auto& value = *static_cast<BeforeNativeApply*>(raw);
              return value.state.generation.value() != value.generation
                     && value.database.get_instance_map().size() > value.instances;
            }, &boundary);
            failed = false;
            try {
              auto attempt = NativeTargetedEditPreparer::prepare(database, authority, {plan.expected_generation, plan.edits}, state);
            } catch (const std::bad_alloc&) {
              failed = true;
            }
            static_cast<void>(ista_test_allocation::disable());
            require(failed && fields(state) == saved_state && graph(database) == saved_graph && authority.readyForQuery(),
                    "post-apply allocation failure leaked native state or resolver key ownership");
          }
          {
            auto attempt = NativeTargetedEditPreparer::prepare(database, authority, {plan.expected_generation, plan.edits}, state);
            ista_test_allocation::armCountOnly();
            const bool restored = attempt.transaction.rollback();
            const auto allocations = ista_test_allocation::disable();
            require(restored && allocations == 0 && fields(state) == saved_state && graph(database) == saved_graph,
                    "consecutive targeted rollback allocated or retained state");
          }
          auto batch = NativeTargetedEditPreparer::prepare(database, authority, {plan.expected_generation, plan.edits}, state);
          requireArcComparatorOrder(state.resolver);
          const auto* arc_storage = database.get_arc_list().data();
          const auto arc_capacity = database.get_arc_list().capacity();
          auto conflicting = plan;
          conflicting.expected_generation = batch.staged.generation;
          conflicting.edits.front().original_net = CreatedNetRef{plan.edits.front().creation_token, CreatedNetRole::kOutputNet};
          conflicting.edits.front().creation_token = editTestCreationToken(100 + generation);
          bool conflict_rejected = false;
          try {
            static_cast<void>(NativeTargetedEditPreparer::prepare(database, authority, {conflicting.expected_generation, conflicting.edits}, batch.staged));
          } catch (const std::logic_error&) {
            conflict_rejected = true;
          }
          require(conflict_rejected && database.get_arc_list().data() == arc_storage
                      && database.get_arc_list().capacity() == arc_capacity,
                  "pending conflict changed canonical Arc storage before rejection");
          require(&batch.transaction.database() == &database && &database.get_timing_library() == library
                      && &database.get_timing_library().get_cell_map().at("BUF_X1") == cell,
                  "targeted edit replaced Database or immutable model storage");
          require(database.get_arc_list().at(retained_slot).get_sink_pin() == "aaa_out"
                      && database.get_timing_point_map().at("aaa_out").get_predecessor_arc_idx() == retained_slot,
                  "targeted edit moved an unaffected Arc or predecessor");
          const auto actual = graph(database);
          const auto equal = [&](const auto& lhs, const auto& rhs, const char* field) {
            if (lhs != rhs)
              throw std::runtime_error(std::string("targeted/fresh graph differs: ") + field);
          };
          equal(actual.nodes, expected.nodes, "nodes");
          equal(actual.instances, expected.instances, "instances");
          equal(actual.arcs, expected.arcs, "arcs");
          equal(actual.out_row_offsets, expected.out_row_offsets, "out offsets");
          equal(actual.out_arc_ids, expected.out_arc_ids, "out order");
          equal(actual.in_row_offsets, expected.in_row_offsets, "in offsets");
          equal(actual.in_arc_ids, expected.in_arc_ids, "in order");
          equal(actual.ordered_start_ids, expected.ordered_start_ids, "starts");
          equal(actual.ordered_end_ids, expected.ordered_end_ids, "ends");
          equal(actual, expected, "remaining graph fields");
          std::map<csr::ArcId, csr::ArcKey> canonical_keys;
          for (const auto& arc : input(database, authority.sessionToken()).arcs)
            canonical_keys.emplace(state.native_arc_ids.at(arc.native_traversal_rank), arc.key);
          for (const auto& arc : batch.patch.arc_appends)
            require(arc.key == canonical_keys.at(arc.id) && arc.payload.key == arc.key,
                    "targeted appended arc identity differs from independent export");
          for (const auto& change : batch.patch.payload_changes)
            require(change.replacement.key == canonical_keys.at(change.id),
                    "targeted changed payload identity differs from independent export");
          require(batch.patch.arc_appends.size() == 3 && batch.patch.logical_deactivations.size() == 1,
                  "targeted edit did not produce physical patch and logical replacement");
          if (generation == 0)
            require(batch.patch.changes.size() == 2, "new load ordering did not replace both Base positions");
          batch.transaction.commit();
          auto stale = plan;
          rejects([&] { static_cast<void>(NativeTargetedEditPreparer::prepare(database, authority, {stale.expected_generation, stale.edits}, state)); });
          plan.expected_generation = state.generation;
          plan.edits.front().original_net = CreatedNetRef{editTestCreationToken(generation + 1), CreatedNetRole::kOutputNet};
          plan.edits.front().creation_token = editTestCreationToken(generation + 2);
        }
        TimingClock clock;
        clock.set_clock_name("source_clock");
        clock.set_source_list({"in"});
        clock.set_period(10.0);
        clock.set_fall_edge(5.0);
        database.get_timing_constraint().get_clock_map().emplace("source_clock", clock);
        const auto clock_input = input(database, authority.sessionToken());
        const auto clock_baseline = baselineIdentity(database, TransactionGeneration{0}, authority.sessionToken());
        auto clock_state = NativeTargetedEditPreparer::bindBaseline(database, clock_input, clock_baseline, models);
        require(!clock_state.clock_arcs->empty(), "clock fixture lost propagated clock identities");
        auto clock_plan = bufferInsertionPlan(database, clock_baseline, TransactionGeneration{0}, editTestCreationToken(3));
        clock_plan.edits.front().original_net = ExistingNetRef{state.resolver.materialization(editTestCreationToken(2)).output_net_name};
        rejects([&] { static_cast<void>(NativeTargetedEditPreparer::prepare(database, authority, {clock_plan.expected_generation, clock_plan.edits}, clock_state)); });
        {
          auto baseline = NativeEditTransaction::prepareFull(authority);
          NativeCandidateExecutor::executeFullTiming(baseline);
          baseline.commit();
        }
        auto full_clock_state = NativeFullEditPreparer::bindBaseline(database, authority);
        const auto& clock_net = std::get<ExistingNetRef>(clock_plan.edits.front().original_net).baseline_name;
        require(clock_net != database.get_pin_map().at("in").get_net_name(),
                "clock rejection fixture must reach beyond the clock source net");
        rejects([&] { static_cast<void>(NativeFullEditPreparer::prepare(
            database, authority, {clock_plan.expected_generation, clock_plan.edits}, full_clock_state)); });
        return StaStatus::success(session.session().state());
      });
  if (!status.ok())
    throw std::runtime_error(status.message);
  require(session.closeAndRelease().ok(), "targeted session cleanup failed");
}
void disabledArcRejection()
{
  installNativeTransactionBaseline();
  auto& database = STADM.getDatabase();
  auto& arcs = database.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list();
  auto disabled = arcs.front();
  disabled.set_is_disable_arc(true);
  arcs.push_back(disabled);
  GuardedTestSession session;
  const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
      session.session(), [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
        requireReleasedSnapshotForNativeMutation(context, authority.sessionToken());
        const auto baseline = baselineIdentity(database, TransactionGeneration{0}, authority.sessionToken());
        csr::detail::NativeLibraryModels models(database);
        auto state = NativeTargetedEditPreparer::bindBaseline(database, input(database, authority.sessionToken()), baseline, models);
        const auto plan = bufferInsertionPlan(database, baseline, TransactionGeneration{0}, editTestCreationToken());
        rejects([&] { static_cast<void>(NativeTargetedEditPreparer::prepare(database, authority, {plan.expected_generation, plan.edits}, state)); });
        require(database.get_instance_map().empty(), "unsupported graph arc mutated canonical Database");
        return StaStatus::success(session.session().state());
      });
  require(status.ok() && session.closeAndRelease().ok(), "unsupported edit cleanup failed");
}
}  // namespace
int main()
{
  NativeTransactionRuntime runtime;
  try {
    parallelArcProjection();
    runTargetedContracts();
    disabledArcRejection();
    std::cout << "native targeted local transaction, fresh graph, consecutive batch and boundary contracts passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
