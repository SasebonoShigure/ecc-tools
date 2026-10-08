// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <algorithm>
#include <stdexcept>
#include <limits>
#include "../CsrReferenceInputTestSupport.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "DataManager.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditRcFixture.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingEditValidation.hpp"
#include "TimingGraphCsrAdapter.hpp"
#ifdef ISTA_RC_CUDA_TRANSACTION_TEST
#include <iostream>
#include "CudaDualCsrTestReader.hpp"
#include "CsrRcTimingComparison.hpp"
#include "CsrCpuReferenceBackendInternal.hpp"
#include "CsrFifoLevelizationValidation.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"
#include "timing_dual_csr/CudaTargetedTimingProjection.hpp"
#endif

namespace {
using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
csr::CsrReferenceInput exportRc(const Database& database, std::uint64_t token)
{
  auto context = editTestAnalysisContext();
  context.rc_corner = "pre_route";
  auto input = csr::exportCsrReferenceInputCandidate(database, context, token);
  if (const auto* failure = std::get_if<csr::CsrReferenceInputExportFailure>(&input))
    throw std::runtime_error("targeted RC export failed: code=" + std::to_string(static_cast<int>(failure->code))
                            + ", phase=" + std::to_string(static_cast<int>(failure->failed_phase)));
  return std::get<csr::CsrReferenceInput>(std::move(input));
}
void retainedSink(std::map<std::string, ParasiticNet>& trees)
{
  auto& net = trees.at("n0");
  auto& node = net.get_node_map()["out_keep"];
  node.set_node_name("out_keep");
  node.set_capacitance(0.012);
  auto& resistor = net.get_resistor_list().emplace_back();
  resistor.set_source_node("in");
  resistor.set_sink_node("out_keep");
  resistor.set_resistance(80.0);
  net.set_lumped_capacitance(net.get_lumped_capacitance() + 0.012);
}
}

static void testTransactions(bool sequential, [[maybe_unused]] bool mixed = false)
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
      session.session(), [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
        requireReleasedSnapshotForNativeMutation(context, authority.sessionToken());
        auto& database = STADM.getDatabase();
        std::string source = sequential ? "out_net" : "n0", driver = sequential ? "ff1:Q" : "in";
        if (sequential) {
          auto fixture = csr::test::makeReferenceNativeFixture(csr::test::ReferenceNativeFixtureCase::kChain);
          fixture.database().get_timing_library().get_cell_map().merge(database.get_timing_library().get_cell_map());
          database = std::move(fixture.database());
          // The export fixture deliberately contains a clock divider and
          // case analysis. Build a supported two-register clock fanout from
          // its raw objects; the real native baseline below creates the graph.
          database.get_timing_constraint().get_case_analysis_map().clear();
          auto& clocks = database.get_timing_constraint().get_clock_map();
          clocks.erase("z_empty_clock");
          clocks.at("a_clock_key").set_source_list({"clk"});
          auto& clock_net = database.get_net_map().at("clk_net");
          clock_net.get_pin_name_list().push_back("ff1:CK");
          database.get_pin_map().at("ff1:CK").set_net_name("clk_net");
          database.get_pin_map().at("ff0:Q").set_net_name("");
          database.get_net_map().erase("div_net");
          auto& ctq = database.get_timing_library().get_cell_map().at("DFF_X1").get_cell_arc_list().front();
          auto& child = ctq.get_timing_arc_list().front();
          for (auto transition : {TransType::kRise, TransType::kFall}) {
            TimingTable delay, slew;
            delay.set_value_list({0.12});
            slew.set_value_list({0.07});
            child.get_delay_table_map()[transition] = delay;
            child.get_slew_table_map()[transition] = slew;
          }
          for (auto& arc : database.get_timing_library().get_cell_map().at("DFF_X1").get_cell_arc_list())
            arc.set_is_timing_graph_arc(false);
          for (auto& [name, instance] : database.get_instance_map()) {
            static_cast<void>(name);
            if (instance.get_cell_name() == "DFF_X1") instance.set_clock_to_q_arc(ctq);
          }
          database.set_design_name("targeted_rc_q");
        } else {
          auto extra = database.get_pin_map().at("out");
          extra.set_pin_name("out_keep");
          extra.set_full_name("out_keep");
          database.get_pin_map().emplace("out_keep", extra);
          database.get_net_map().at("n0").get_pin_name_list().push_back("out_keep");
        }
        auto& rc_library = database.get_parasitic_library();
        rc_library.set_capacitive_unit("PF");
        rc_library.set_resistance_unit("OHM");
        wire(rc_library.get_net_map(), source, driver, "out", 125.0);
        if (!sequential) retainedSink(rc_library.get_net_map());
        {
          auto initial = NativeEditTransaction::prepareFull(authority);
          NativeCandidateExecutor::executeFull(initial);
          initial.commit();
        }
        const auto input = exportRc(database, authority.sessionToken().value());
        const auto identity = NativeTimingEditPreparer::identityFromInput(database, input, TransactionGeneration{0});
        const BaselineSemanticIdentity baseline{database.get_design_name(), identity.current_fingerprints, identity.canonical_pre_state};
        csr::detail::NativeLibraryModels models(database);
        auto state = NativeTargetedEditPreparer::bindBaseline(database, input, baseline, models);
#ifdef ISTA_RC_CUDA_TRANSACTION_TEST
        auto device = csr::CudaDualCsrTestReader::computeRc(input, 64, state.resolver.highWater());
        require(!device.computationCode(), "native RC CUDA baseline failed");
        const auto level_mode = mixed ? csr::CudaLevelizationMode::kMixed : csr::CudaLevelizationMode::kFull;
#endif
        auto plan = bufferInsertionPlan(database, baseline, TransactionGeneration{0}, editTestCreationToken());
        plan.edits.front().original_net = ExistingNetRef{source};
        const auto q = state.node_ids.at(driver);
        if (sequential)
          require(std::ranges::any_of(input.start_seeds, [&](const auto& seed) {
                    return seed.seed_node == q && seed.availability == csr::StartSeedAvailability::kDeferredSequential
                        && seed.parasitic == state.parasitics.at(q);
                  }), "targeted Q fixture has no deferred RC start");
        for (std::uint64_t generation = 0; generation < 2; ++generation) {
          plan.expected_generation = state.generation;
          auto& edit = plan.edits.front();
          edit.creation_token = editTestCreationToken(generation + 1);
          edit.rc_inputs = ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree};
          edit.placement_inputs = PlacedInputs{5.0 + generation, 7.0};
          const auto names = defaultTokenMaterialization(edit.creation_token, edit.library_cell);
          const auto old_bindings = state.parasitics;
          const auto* old_native = &rc_library.get_net_map().at(source);
          // Finite raw values overflow only while reducing the already applied
          // native RC tree. The transaction must unwind raw, graph and metadata.
          {
            std::map<std::string, ParasiticNet> trees;
            wire(trees, source, driver, names.input_pin_name, 75.0);
            if (source == "n0") retainedSink(trees);
            wire(trees, names.output_net_name, names.output_pin_name, "out", 50.0);
            for (auto& [name, node] : trees.at(source).get_node_map()) {
              static_cast<void>(name);
              node.set_capacitance(std::numeric_limits<double>::max());
            }
            bool rejected = false;
            try {
              auto batch = NativeTargetedEditPreparer::prepare(database, authority,
                  {plan.expected_generation, plan.edits}, state, std::move(trees));
            } catch (const std::overflow_error&) { rejected = true; }
            require(rejected && authority.readyForQuery() && state.generation.value() == generation
                        && state.parasitics == old_bindings && &rc_library.get_net_map().at(source) == old_native
                        && !database.get_net_map().contains(names.output_net_name)
                        && !database.get_pin_map().contains(names.input_pin_name),
                    "RC reduction failure did not restore the applied transaction");
          }
          for (bool commit : {false, true}) {
#ifdef ISTA_RC_CUDA_TRANSACTION_TEST
            const auto old_timing = device.timingValues();
#endif
            {
              std::map<std::string, ParasiticNet> trees;
              wire(trees, source, driver, names.input_pin_name, 75.0);
              if (source == "n0") retainedSink(trees);
              wire(trees, names.output_net_name, names.output_pin_name, "out", 50.0);
              auto batch = NativeTargetedEditPreparer::prepare(database, authority,
                  {plan.expected_generation, plan.edits}, state, std::move(trees));
              require(!batch.patch.rc_bindings.empty(), "targeted RC edit omitted binding changes");
              for (const auto& delta : batch.patch.rc_bindings) {
                const auto old = old_bindings.find(delta.node);
                require(delta.expected == (old == old_bindings.end() ? csr::CsrParasiticBinding{} : old->second),
                        "targeted RC expected binding did not retain the committed owner");
                require(delta.replacement == state.parasitics.at(delta.node)
                            && delta.replacement.tree != delta.expected.tree, "targeted RC replacement is stale");
              }
              if (sequential && generation == 0)
                require(std::ranges::any_of(batch.patch.rc_bindings, [&](const auto& delta) {
                          return delta.node == q && delta.expected == old_bindings.at(q)
                              && delta.replacement.tree->terminals.front() == q && delta.replacement.terminal == 0;
                        }), "targeted patch omitted the deferred Q driver update");
              if (!sequential && generation == 0) {
                const auto keep = state.node_ids.at("out_keep");
                require(std::ranges::any_of(batch.patch.payload_changes, [&](const auto& value) {
                          return value.replacement.sink == keep && value.replacement.parasitic.tree;
                        }), "targeted RC did not refresh the unmoved sink");
              }
              const auto exported = exportRc(database, authority.sessionToken().value());
              const auto compare = [&](const csr::ArcSemanticPayload& payload) {
                require(payload.parasitic.tree && !payload.output_load, "targeted RC payload retained no-RC load");
                const auto found = std::ranges::find_if(exported.arcs, [&](const auto& arc) { return arc.key == payload.key; });
                require(found != exported.arcs.end() && found->parasitic.tree, "full export lost targeted RC payload");
                require(found->parasitic.tree->net_name == payload.parasitic.tree->net_name
                            && found->parasitic.terminal == payload.parasitic.terminal, "targeted RC terminal differs from full export");
                for (std::size_t lane = 0; lane < payload.parasitic.tree->lanes.size(); ++lane)
                  require(*found->parasitic.tree->lanes[lane] == *payload.parasitic.tree->lanes[lane],
                          "targeted affected-net model differs from full export");
              };
              for (const auto& arc : batch.patch.arc_appends) compare(arc.payload);
              for (const auto& arc : batch.patch.payload_changes) compare(arc.replacement);
              #ifdef ISTA_RC_CUDA_TRANSACTION_TEST
              auto candidate = device.stage(batch.patch, level_mode);
              require(device.canPublish(candidate) && !candidate.canPublish(device) && !candidate.computationCode() && candidate.telemetry().materialize_ms == 0.0,
                      "native RC patch failed or forced full materialization");
              if constexpr (incregpusta_debug_checks_enabled) {
                auto actual = csr::test::projectCudaTargetedTiming(exported, candidate.timingValues(),
                                                                 state.node_names, state.arc_native_ranks);
                require(exported.unsupported_reasons.empty(),
                        "pre-route RC component has an unsupported semantic");
                csr::FlatCsrEdgeView edges(exported);
                csr::FlatTimingPayloadView payload(exported);
                require(csr::validateCsrFifoLevelization(edges,
                    {true, actual.level_offsets, actual.level_nodes, actual.node_to_level}).valid(),
                    "native RC CUDA levels are invalid");
                const auto computed = csr::detail::computeCsrCpuReferenceForStage(edges, payload).computation;
                const auto* expected = std::get_if<csr::CsrTimingResult>(&computed.outcome);
                require(expected != nullptr, "native RC fresh reference failed");
                actual.level_offsets = expected->level_offsets;
                actual.level_nodes = expected->level_nodes;
                actual.node_to_level = expected->node_to_level;
                csr::test::compareRcTiming(actual, *expected);
              }
              if (commit) require(device.publish(std::move(candidate)), "native RC device commit failed");
#endif
              if (commit) batch.transaction.commit();
            }
            require(authority.readyForQuery(), "targeted RC transaction retained pending state");
#ifdef ISTA_RC_CUDA_TRANSACTION_TEST
            if (!commit) {
              csr::CudaDualCsrPatch noop;
              noop.session_token = state.session_token;
              noop.expected_generation = state.generation;
              noop.target_generation = TransactionGeneration{generation + 1};
              noop.before = noop.after = state.resolver.highWater();
              auto replay = device.stage(noop, level_mode);
              require(!replay.computationCode() && replay.timingValues() == old_timing,
                      "native RC abandoned patch did not restore actual device state");
            }
#endif
            if (!commit)
              require(state.generation.value() == generation && state.parasitics == old_bindings
                          && &rc_library.get_net_map().at(source) == old_native
                          && !database.get_net_map().contains(names.output_net_name),
                      "targeted RC abandonment lost native or CSR old owners");
          }
          require(state.generation.value() == generation + 1, "targeted RC commit generation mismatch");
          source = names.output_net_name;
          driver = names.output_pin_name;
          edit.original_net = CreatedNetRef{edit.creation_token, CreatedNetRole::kOutputNet};
        }
        #ifdef ISTA_RC_CUDA_TRANSACTION_TEST
        require(device.generation() == state.generation && device.close(), "native/device RC cleanup or generation mismatch");
#endif
        return StaStatus::success(session.session().state());
      });
  if (!status.ok()) throw std::runtime_error(status.message);
  require(session.closeAndRelease().ok(), "targeted RC session cleanup failed");
}

void testNativeTargetedRcTransactions()
{
  testTransactions(false);
  testTransactions(true);
}

#ifdef ISTA_RC_CUDA_TRANSACTION_TEST
int main()
{
  try {
    NativeTransactionRuntime runtime;
    for (bool mixed : {false, true})
      for (bool sequential : {false, true}) {
        try { testTransactions(sequential, mixed); }
        catch (const std::exception& error) {
          throw std::runtime_error(std::string(sequential ? "Q" : "port") + (mixed ? "/mixed: " : "/full: ") + error.what());
        }
      }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
#endif
