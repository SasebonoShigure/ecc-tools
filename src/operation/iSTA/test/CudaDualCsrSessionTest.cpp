
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCudaPacking.hpp"
#include "AllocationFailureSupport.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CudaDualCsrDebugState.hpp"
#include "CudaDualCsrSession.hpp"
#include "CudaDualCsrTestReader.hpp"
#include "CudaDualCsrTestSupport.hpp"
#include "CudaTargetedExecution.hpp"
#include "CudaTargetedTraceReplayer.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "NativeCandidateExecutor.hpp"
#include "PublicTimingJson.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaCudaSessionState.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"
#include "TimingResultScatterInternal.hpp"
#include "timing_edit/BufferTraceReplayInput.hpp"
#include "timing_dual_csr/DualCsrTargetedArtifactSupport.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "timing_dual_csr/DualCsrTestSupport.hpp"

namespace ista::csr::test {
void checkCudaSemanticFault(const std::filesystem::path&, const std::string&);
}
namespace {
using namespace ista;
using namespace csr;
using namespace timing_dual_csr;
using namespace timing_dual_csr::test;
using csr::test::patchFor;
void compareFreshFull(const CsrReferenceInput& baseline, const CudaDualCsrPatch& first, const CudaDualCsrPatch* second, const CsrTimingValues& actual)
{
  auto input = baseline;
  input.nodes.push_back(first.node_appends[0].payload);
  input.node_ids_by_key.push_back({input.nodes.back().key, NodeId{2}});
  input.arcs.push_back(arcRecord(1, input.nodes[0], input.nodes[2], 0));
  input.arc_ids_by_key.push_back({input.arcs.back().key, ArcId{1}});
  input.out_row_offsets = {0, 2, 2, 2};
  input.out_arc_ids = {ArcId{0}, ArcId{1}};
  input.in_row_offsets = {0, 0, 1, 2};
  input.in_arc_ids = {ArcId{0}, ArcId{1}};
  input.ordered_end_ids = {NodeId{1}, NodeId{2}};
  input.clock_owner_by_node.resize(3);
  input.endpoint_required.push_back(first.appended_endpoints[0]);
  PortConstraintModel sink;
  sink.map_key = sink.embedded_port_name = input.nodes[2].key.full_name;
  sink.resolved_node = NodeId{2};
  sink.output_delay[0] = 2.0;
  input.port_constraints.push_back(sink);
  auto comparable = actual;
  if (second) {
    input.nodes[2].net_name = "n2";
    input.nodes.push_back(second->node_appends[0].payload);
    input.node_ids_by_key.push_back({input.nodes.back().key, NodeId{3}});
    input.arcs[1] = arcRecord(1, input.nodes[3], input.nodes[2], 0);
    input.arcs[1].key.base.owner_name = "n2";
    input.arcs[1].output_load = csr::test::outputLoad(0.0);
    input.arc_ids_by_key[1] = {input.arcs[1].key, ArcId{1}};
    input.out_row_offsets = {0, 1, 1, 1, 2};
    input.out_arc_ids = {ArcId{0}, ArcId{1}};
    input.in_row_offsets = {0, 0, 1, 2, 2};
    input.in_arc_ids = {ArcId{0}, ArcId{1}};
    input.ordered_root_ids = {NodeId{0}, NodeId{3}};
    input.ordered_start_ids = input.ordered_root_ids;
    input.clock_owner_by_node.resize(4);
    input.start_seeds.push_back(second->appended_start_seeds[0]);
    auto source = input.port_constraints[0];
    source.map_key = source.embedded_port_name = input.nodes[3].key.full_name;
    source.resolved_node = NodeId{3};
    source.input_delay = {0.25, 0.25};
    input.port_constraints.push_back(source);
    // This two-net microcase has no child rows. Compact only inactive ArcId 1;
    // every other field is compared whole, including all presences and winners.
    auto eraseLane = []<class T>(OptionalSoa<T>& values, std::size_t width) {
      values.values.erase(values.values.begin() + width, values.values.begin() + 2 * width);
      values.presence.erase(values.presence.begin() + width, values.presence.begin() + 2 * width);
    };
    eraseLane(comparable.arcs.delays, 1);
    eraseLane(comparable.arcs.min_delays, 1);
    eraseLane(comparable.arcs.max_delays, 1);
    eraseLane(comparable.arcs.transition_delays, 4);
    eraseLane(comparable.arcs.input_output_delays, 8);
    eraseLane(comparable.arcs.graph_delays, 8);
    eraseLane(comparable.arcs.transition_types, 2);
    require(comparable.arcs.timing_arc_indices.empty(), "net microcase unexpectedly acquired child rows");
    comparable.arcs.timing_arc_row_offsets.erase(comparable.arcs.timing_arc_row_offsets.begin() + 2);
    auto remap = [](OptionalSoa<ArcId>& ids) {
      for (std::size_t i = 0; i < ids.values.size(); ++i)
        if (ids.presence[i]) {
          require(ids.values[i] != ArcId{1}, "inactive Aux arc survived as winner");
          if (ids.values[i] == ArcId{2})
            ids.values[i] = ArcId{1};
        }
    };
    remap(comparable.nodes.predecessor_arcs);
    remap(comparable.paths.predecessor_arcs);
  }
  csr::test::refreshCpuReferenceIdentity(input);
  auto cpu = CsrCpuReferenceBackend{}.compute(input);
  auto gpu = CsrCudaBackend{}.computeWithTelemetry(input);
  auto* cpu_values = std::get_if<CsrTimingResult>(&cpu.outcome);
  auto* gpu_values = std::get_if<CsrTimingResult>(&gpu.computation.outcome);
  require(cpu_values && gpu_values, "independent fresh full microcase did not compute");
  require(static_cast<const CsrTimingValues&>(*cpu_values) == comparable && static_cast<const CsrTimingValues&>(*gpu_values) == comparable,
          "targeted values differ from independent fresh CPU/CUDA full");
}
void compareIndependentCpu(const CsrReferenceInput& input, const CudaDualCsrPatch& first, const CudaDualCsrPatch* second, const CsrTimingValues& actual)
{
  compareFreshFull(input, first, second, actual);
  auto attempt = DualCsrBaselineFactory::create(input, first.session_token, first.before);
  require(attempt.success(), "independent CPU baseline failed");
  auto state = std::move(*attempt.state);
  state.nodes.push_back(first.node_appends[0].payload);
  state.arc_slots.push_back({first.arc_appends[0].payload, true});
  state.base_out.row_offsets = {0, 1, 1, 1};
  state.base_in.row_offsets = {0, 0, 1, 2};
  state.base_in.physical_slots = {ArcId{0}, ArcId{1}};
  state.aux_out.row_sources = {NodeId{0}};
  state.aux_out.row_arcs = {{ArcId{1}}};
  state.aux_out.source_to_aux_row = {0, -1, -1};
  state.reference_payload.ordered_end_ids = {NodeId{1}, NodeId{2}};
  state.reference_payload.endpoint_required.push_back(first.appended_endpoints[0]);
  state.reference_payload.clock_owner_by_node.resize(3);
  state.high_water = first.after;
  state.telemetry.logical_edges_after = 2;
  if (second) {
    state.nodes[2].net_name = "n2";
    state.nodes.push_back(second->node_appends[0].payload);
    state.arc_slots[1].active = false;
    state.arc_slots.push_back({second->arc_appends[0].payload, true});
    state.base_out.row_offsets = {0, 1, 1, 1, 2};
    state.base_out.physical_slots = {ArcId{0}, ArcId{2}};
    state.base_in.row_offsets = {0, 0, 1, 2, 2};
    state.base_in.physical_slots = {ArcId{0}, ArcId{2}};
    state.aux_out.row_sources.clear();
    state.aux_out.row_arcs.clear();
    state.aux_out.source_to_aux_row = {-1, -1, -1, -1};
    state.reference_payload.ordered_root_ids = {NodeId{0}, NodeId{3}};
    state.reference_payload.ordered_start_ids.push_back(NodeId{3});
    state.reference_payload.start_seeds.push_back(second->appended_start_seeds[0]);
    state.reference_payload.clock_owner_by_node.resize(4);
    state.high_water = second->after;
  }
  DualCsrEdgeViewImpl edges(state);
  DualCsrTimingPayloadViewImpl payloads(state);
  auto cpu = CsrCpuReferenceBackend{}.compute(edges, payloads);
  const auto* result = std::get_if<CsrTimingResult>(&cpu.outcome);
  require(result != nullptr, "independent CPU Aux did not compute");
  CsrTimingResult actual_result;
  static_cast<CsrTimingValues&>(actual_result) = actual;
  actual_result.schema_version = result->schema_version;
  actual_result.source_identity = result->source_identity;
  std::string mismatch;
  require(csr::test::exactTimingResultEqual(actual_result, *result, &mismatch), "independent CPU complete values differ after Aux update: " + mismatch);
  if (!second) {
    state.aux_out.row_arcs.clear();
    state.aux_out.row_sources.clear();
    state.aux_out.source_to_aux_row = {-1, -1, -1};
    DualCsrEdgeViewImpl omitted_edges(state);
    auto omitted = CsrCpuReferenceBackend{}.compute(omitted_edges, payloads);
    const auto* wrong = std::get_if<CsrTimingResult>(&omitted.outcome);
    require(!wrong || static_cast<const CsrTimingValues&>(*wrong) != actual, "Aux omission canary did not change actual computation");
  }
}


void auxSequence(nlohmann::json* snapshots, CudaLevelizationMode mode)
{
  const auto input = csr::test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  CudaDualCsrSession owner(input, water), stranger(input, water);
  if (snapshots)
    snapshots->push_back(CudaDualCsrTestReader::capture(owner));
  if constexpr (incregpusta_debug_checks_enabled) {
    const auto cpu = CsrCpuReferenceBackend{}.compute(input);
    require(owner.timingValues() == static_cast<const CsrTimingValues&>(std::get<CsrTimingResult>(cpu.outcome)), "baseline CPU/GPU values differ");
  }
  auto first = patchFor(input, 0, water, {3, 3, 2});
  const auto sink = appendNode(2, "z_aux_sink", AppendedPortRole::kSink);
  first.node_appends = {sink};
  first.arc_appends = {appendArc(1, NodeId{0}, NodeId{2}, input.nodes[0].key, sink.key, 0)};
  first.aux_rows = {{NodeId{0}, {}, {ArcId{1}}}};
  first.appended_base_out_rows = {{NodeId{2}, {}}};
  first.appended_base_in_rows = {{NodeId{2}, {ArcId{1}}}};
  first.ends = ValueReplacement<std::vector<NodeId>>{{NodeId{1}}, {NodeId{1}, NodeId{2}}};
  first.appended_endpoints = {{NodeId{2}, "clk", 0.0, {2.0, std::nullopt}}};
  auto orphan = patchFor(input, 0, water, {2, 2, 2});
  orphan.arc_appends = {appendArc(1, NodeId{0}, NodeId{1}, input.nodes[0].key, input.nodes[1].key, 1)};
  bool malformed_rejected = false;
  try {
    static_cast<void>(owner.stage(orphan, mode));
  } catch (const std::invalid_argument&) {
    malformed_rejected = true;
  }
  require(malformed_rejected && !owner.poisoned(), "orphan active arc was accepted");
  auto duplicate = first;
  duplicate.aux_rows[0].replacement_arc_ids.push_back(ArcId{1});
  malformed_rejected = false;
  try {
    static_cast<void>(owner.stage(duplicate, mode));
  } catch (const std::invalid_argument&) {
    malformed_rejected = true;
  }
  require(malformed_rejected, "duplicate adjacency was accepted");
  if constexpr (incregpusta_debug_checks_enabled) {
    if (mode == CudaLevelizationMode::kFull) {
      auto omitted = owner.stage(first, mode);
      CudaDualCsrTestReader::omitAuxAndRecompute(omitted);
      require(omitted.computationCode() == CsrComputationCode::kCycle && !owner.publish(std::move(omitted)),
              "actual GPU Aux omission did not fail levelization/publication");
      require(owner.generation().value() == 0 && !owner.poisoned() && owner.timingValues() == stranger.timingValues(),
              "GPU omission canary changed committed owner");
    }
  }
  auto candidate = owner.stage(first, mode);
  require(!candidate.computationCode(), "Aux append failed");
  require(candidate.timingValues().nodes.arrivals.values[2] == 1.0, "Aux-only endpoint arrival missing");
  require(candidate.timingValues().nodes.requireds.values[0] == -2.0, "Aux required pass missing");
  require(candidate.telemetry().cuda.h2d_bytes > 0 && candidate.telemetry().cuda.kernel_launches > 0, "missing patch upload or computation");
  if constexpr (incregpusta_debug_checks_enabled) {
    compareIndependentCpu(input, first, nullptr, candidate.timingValues());
    const auto graph = candidate.debugState();
    require(graph.out_arcs == std::vector<cuda_detail::Index>{0} && graph.aux_rows[0] == std::vector<cuda_detail::Index>{1},
            "Base expanded instead of genuine Aux overflow");
    for (unsigned stage = mode == CudaLevelizationMode::kFull ? 0 : 1; stage < 4; ++stage)
      require(graph.storage_visits[stage * 3 + ((stage == 1 || stage == 2) ? 2 : 1)] > 0,
              "GPU pass did not visit its actual incoming/Aux storage");
  }
  if (snapshots)
    snapshots->push_back(CudaDualCsrTestReader::capture(candidate));
  require(!stranger.publish(std::move(candidate)), "foreign owner accepted candidate");
  bool pending_rejected = false, query_rejected = false;
  try {
    static_cast<void>(owner.stage(first, mode));
  } catch (const std::invalid_argument&) {
    pending_rejected = true;
  }
  try {
    static_cast<void>(owner.timingValues());
  } catch (const std::logic_error&) {
    query_rejected = true;
  }
  require(pending_rejected && query_rejected, "pending transaction admitted a sibling or stale query");
  require(owner.publish(std::move(candidate)) && !owner.publish(std::move(candidate)), "candidate publication is not one-shot");
  if (mode == CudaLevelizationMode::kMixed) {
    auto conflict = patchFor(input, 1, first.after, {3, 3, 3});
    conflict.arc_appends = {appendArc(2, NodeId{2}, NodeId{1}, sink.key, input.nodes[1].key, 0)};
    conflict.removes = {{NodeId{0}, 0, ArcId{0}}};
    conflict.logical_deactivations = {{ArcId{0}, input.arcs[0].key, NodeId{0}, NodeId{1}, LogicalArcDeactivateReason::kRemoved, true}};
    conflict.reverse_changes = {{NodeId{1}, 0, ArcId{0}, ArcId{2}}};
    conflict.aux_rows = {{NodeId{2}, {}, {ArcId{2}}}};
    auto rejected = owner.stage(conflict, mode);
    require(rejected.telemetry().mixed_failure == timing_incremental_levelization::MixedStageFailureCode::kOldLevelOrderConflict
                && rejected.computationCode() && !owner.publish(std::move(rejected)),
            "acyclic old-level order conflict was accepted as mixed");
    {
    auto full = owner.stage(conflict, CudaLevelizationMode::kFull);
    require(!full.computationCode() && full.timingValues().node_to_level == std::vector<std::uint64_t>{1, 3, 2},
            "explicit full retry did not execute the acyclic replacement");
    require(owner.generation().value() == 1 && !owner.poisoned(), "unpublished full retry changed origin");

    }
    // Replacing eligibility must update the CPU topology metadata, not just
    // the device payload. Node 2 is now an independent (noncompact) old root.
    auto eligibility = patchFor(input, 1, first.after, first.after);
    auto disabled = first.arc_appends[0].payload;
    disabled.eligibility = {};
    eligibility.payload_changes = {{ArcId{1}, first.arc_appends[0].payload, disabled}};
    eligibility.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, {NodeId{0}, NodeId{2}}};
    const auto previous_levels = owner.timingValues().node_to_level;
    auto changed_eligibility = owner.stage(eligibility, mode);
    require(!changed_eligibility.computationCode(), "payload eligibility replacement left stale mixed topology");
    require(changed_eligibility.timingValues().node_to_level == previous_levels,
            "noncompact old levels were not preserved after eligibility replacement");
  }
  auto second = patchFor(input, 1, first.after, {4, 4, 3});
  auto source = appendNode(3, "z_new_source", AppendedPortRole::kSource);
  source.payload.net_name = "n2";
  second.node_appends = {source};
  second.arc_appends = {appendArc(2, NodeId{3}, NodeId{2}, source.key, sink.key, 0)};
  second.arc_appends[0].payload.key.base.owner_name = "n2";
  second.arc_appends[0].key = second.arc_appends[0].payload.key;
  second.arc_appends[0].payload.output_load = csr::test::outputLoad(0.0);
  second.aux_rows = {{NodeId{0}, {ArcId{1}}, {}}};
  second.logical_deactivations = {{ArcId{1}, first.arc_appends[0].key, NodeId{0}, NodeId{2}, LogicalArcDeactivateReason::kAuxCleared, true}};
  second.reverse_changes = {{NodeId{2}, 1, ArcId{1}, ArcId{2}}};
  second.appended_base_out_rows = {{NodeId{3}, {ArcId{2}}}};
  second.appended_base_in_rows = {{NodeId{3}, {}}};
  second.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, {NodeId{0}, NodeId{3}}};
  auto seed = input.start_seeds[0];
  seed.seed_node = NodeId{3};
  seed.path_start_node = NodeId{3};
  for (auto& analysis : seed.arrival)
    for (auto& lane : analysis)
      lane = 0.25;
  second.appended_start_seeds = {seed};
  auto cleared = owner.stage(second, mode);
  require(!cleared.computationCode() && cleared.timingValues().nodes.arrivals.values[2] == 0.25, "stale previous computed value survived seed reset");
  if constexpr (incregpusta_debug_checks_enabled) {
    compareIndependentCpu(input, first, &second, cleared.timingValues());
    const auto graph = cleared.debugState();
    require(std::ranges::all_of(graph.aux_rows, [](const auto& row) { return row.empty(); }) && !graph.arcs[1].active && graph.in_arcs[1] == 2, "Aux clear/reverse change failed");
  }
  if (snapshots)
    snapshots->push_back(CudaDualCsrTestReader::capture(cleared));
  require(owner.publish(std::move(cleared)), "second publish failed");
  const auto committed = owner.timingValues();
  auto empty = patchFor(input, 2, second.after, second.after);
  auto bad = empty;
  bad.removes = {{NodeId{0}, 0, ArcId{1}}};
  malformed_rejected = false;
  try {
    static_cast<void>(owner.stage(bad, mode));
  } catch (const std::invalid_argument&) {
    malformed_rejected = true;
  }
  require(malformed_rejected, "inactive expected incidence accepted");
  auto mismatch = empty;
  auto expected = arcPayload(input.nodes[0].key, input.nodes[1].key, NodeId{0}, NodeId{1}, 0);
  auto replacement = expected;
  expected.output_load = csr::test::outputLoad(0.625);
  mismatch.payload_changes = {{ArcId{0}, expected, replacement}};
  auto failed = owner.stage(mismatch, mode);
  require(failed.computationCode() == CsrComputationCode::kInvariantViolation && !owner.publish(std::move(failed)),
          "device payload expected mismatch accepted");
  require(owner.generation().value() == 2 && owner.timingValues() == committed && !owner.poisoned(), "algorithm rejection changed old device state");
  auto no_op = owner.stage(empty, mode);
  require(!no_op.computationCode() && no_op.timingValues() == committed && owner.publish(std::move(no_op)), "healthy empty replay after rejection failed");
  bool rejected = false;
  try {
    static_cast<void>(owner.stage(empty, mode));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "stale generation accepted");
  auto overflow = patchFor(input, 3, second.after, second.after);
  overflow.after.node_slots = std::numeric_limits<std::uint64_t>::max();
  rejected = false;
  try {
    static_cast<void>(owner.stage(overflow, mode));
  } catch (const std::exception&) {
    rejected = true;
  }
  require(rejected && !owner.poisoned(), "capacity rejection poisoned committed owner");
}
void queryAllocationRetry(const timing_edit::oracle::ProductionFreshSessionView& loaded)
{
  if constexpr (incregpusta_debug_checks_enabled) {
    auto& context = loaded.session_context;
    bool failed = false;
    ista_test_allocation::armFailure(0);
    try {
      context.materializeCudaTiming(loaded.database, loaded.authority);
    } catch (const std::bad_alloc&) {
      failed = true;
    } catch (...) {
      static_cast<void>(ista_test_allocation::disable());
      throw;
    }
    static_cast<void>(ista_test_allocation::disable());
    require(failed, "first materialization did not exercise real allocation failure");
    context.materializeCudaTiming(loaded.database, loaded.authority);

    STADC.init();
    TimingAnalyzer::initInst();
    ista_test_allocation::armCountOnly();
    STATA.analyzePropagatedTiming();
    const auto count = ista_test_allocation::disable();
    require(count != 0, "report preparation did not allocate a result");
    auto& summary = loaded.database.get_summary().ta_summary;
    const auto saved_slack = summary.worst_slack;
    summary.worst_slack = 123456.0;
    auto& groups = loaded.database.get_timing_path_group_list();
    const auto* storage = groups.data();
    failed = false;
    ista_test_allocation::armFailure(count - 1);
    try {
      STATA.analyzePropagatedTiming();
    } catch (const std::bad_alloc&) {
      failed = true;
    } catch (...) {
      static_cast<void>(ista_test_allocation::disable());
      throw;
    }
    static_cast<void>(ista_test_allocation::disable());
    require(failed && summary.worst_slack == 123456.0 && groups.data() == storage,
            "failed report preparation partially published paths or summary");
    TimingAnalyzer::destroyInst();
    STADC.destroy();
    failed = false;
    ista_test_allocation::armFailure(count - 1);
    try {
      context.prepareCudaReport(loaded.database, loaded.authority);
    } catch (const std::exception&) {
      failed = true;
    } catch (...) {
      static_cast<void>(ista_test_allocation::disable());
      throw;
    }
    static_cast<void>(ista_test_allocation::disable());
    require(failed, "guarded report query did not exercise allocation failure");
    context.prepareCudaReport(loaded.database, loaded.authority);
    require(summary.worst_slack == saved_slack, "report preparation retry failed");
  }
}

nlohmann::json captureCudaPublic(const timing_edit::oracle::ProductionFreshSessionView& loaded)
{
  loaded.session_context.prepareCudaReport(loaded.database, loaded.authority);
  return ista::test::capturePublicTiming();
}

void replayCrossBuild(const std::filesystem::path& path, CudaLevelizationMode mode, nlohmann::json& snapshots)
{
  const auto program = buffer_trace::test::readReplayInput(path, true);
  const auto& plans = std::visit([](const auto& workload) -> const auto& { return workload.batches; }, program.workload);
  require(plans.size() >= 2, "cross-build recovery needs a committed batch and continuation");
  std::vector<StaExecutionRecord> persisted;
  nlohmann::json original;
  std::uint64_t original_session = 0;
  for (bool recovering : {false, true}) {
    DualCsrTargetedArtifactSupport artifacts(program);
    timing_edit::oracle::ProductionFreshSessionLoader::run(
        artifacts.artifacts(),
        [&](const timing_edit::oracle::ProductionFreshSessionView& loaded) {
          require(!recovering || loaded.session_token != original_session, "recovery did not create a new session");
          if (!recovering)
            original_session = loaded.session_token;
          auto& owned = loaded.session_context.cudaState(loaded.authority);
          owned.bindEdits(loaded.database, loaded.reference_input, program.baseline());
          auto& state = *owned.host;
          owned.history.reserve(plans.size());
          auto& owner = owned.device;
          require(!owner.computationCode(), "cross-build CUDA baseline failed");
          // Recovery repeats the real clock/CTQ/check workload with a different launch shape.
          CudaDualCsrTestReader::setKernelThreads(owner, recovering ? 37U : 256U);
          nlohmann::json observations = nlohmann::json::array();
          observations.push_back({{"execution", "baseline-full"}, {"state", CudaDualCsrTestReader::capture(owner)}});
          const auto requested = mode == CudaLevelizationMode::kMixed ? StaUpdateStrategy::kIncremental : StaUpdateStrategy::kFull;
          for (std::size_t index = 0; index < plans.size(); ++index) {
            const auto strategy = recovering && index == 0 ? persisted.front().executed_update_strategy : requested;
            auto record = plans[index];
            auto& models = loaded.session_context.libraryModels(loaded.database);
            auto direct = timing_dual_csr::TimingEditDualCsrDescriptorBuilder::buildDirect(
                state.prepareBatch(plans[index]), loaded.authority, models);
            auto device_patch = csr::detail::makeCudaTargetedPatch(state.graph, direct.descriptor(), models);
            CudaCsrEditState::Patch prepared(state, std::move(direct));
            prepared.apply();
            bool pending_rejected = false;
            try {
              loaded.session_context.materializeCudaTiming(loaded.database, loaded.authority);
            } catch (const std::logic_error&) {
              pending_rejected = true;
            }
            require(pending_rejected, "CSR pending transaction served cached timing");
            CudaTargetedTraceBatch batch;
            auto candidate = csr::detail::stageCudaTargetedBatch(owner, device_patch, strategy, StaFallbackPolicy::kError, batch.execution, batch.rejected_mixed_attempt);
            require(!candidate.computationCode() && !candidate.poisoned()
                        && batch.execution.executed_update_strategy == strategy && !batch.execution.fallback_to_full,
                    "cross-build replay did not execute the requested CUDA mode");
            require(owner.publish(std::move(candidate)), "cross-build CUDA publication failed");
            prepared.commit();
            owned.history.push_back(std::move(record));
            owned.committed();
            require(owner.generation() == state.graph.generation, "cross-build host/device generation differs");
            if (index == 0)
              queryAllocationRetry(loaded);
            observations.push_back({
                {"requested_update_strategy", toString(batch.execution.requested_update_strategy)},
                {"executed_update_strategy", toString(batch.execution.executed_update_strategy)},
                {"incremental_batch_executed", batch.execution.incremental_batch_executed},
                {"fallback_to_full", batch.execution.fallback_to_full},
                {"state", CudaDualCsrTestReader::capture(owner)},
                {"public", captureCudaPublic(loaded)}});
            const auto d2h_after_query = owner.telemetry().cuda.d2h_bytes;
            require(captureCudaPublic(loaded) == observations.back().at("public")
                        && owner.telemetry().cuda.d2h_bytes == d2h_after_query,
                    "repeat public query changed results or repeated D2H");
            if (!recovering)
              persisted.push_back(batch.execution);
            else
              require(observations.back() == original.at(index + 1), "CUDA restored/continued state differs from saved execution");
          }
          if (!recovering) {
            original = observations;
            snapshots.push_back({{"replay", std::move(observations)}});
          } else {
            snapshots.push_back({{"restored_prefix_batches", 1}, {"continued_batches", plans.size() - 1},
                                 {"recovery_then_continuation", std::move(observations)}});
          }
        },
        StaBackend::kCuda);
  }
}

void emptyGraph(CudaLevelizationMode mode)
{
  CsrReferenceInput input;
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.out_row_offsets = {0};
  input.in_row_offsets = {0};
  csr::test::refreshCpuReferenceIdentity(input);
  CudaDualCsrSession owner(input, {0, 0, 0});
  require(!owner.computationCode() && owner.timingValues().level_offsets == std::vector<std::uint64_t>{0}, "empty GPU graph failed");
  const auto before = owner.timingValues();
  auto candidate = owner.stage(patchFor(input, 0, {0, 0, 0}, {0, 0, 0}), mode);
  require(!candidate.computationCode() && candidate.timingValues() == before && owner.publish(std::move(candidate)), "empty GPU batch failed");
}
void removeChangeAndCycle(CudaLevelizationMode mode)
{
  const auto input = csr::test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  CudaDualCsrSession owner(input, water);
  auto removal = patchFor(input, 0, water, {3, 3, 2});
  const auto source = appendNode(2, "other_source", AppendedPortRole::kSource);
  removal.node_appends = {source};
  removal.arc_appends = {appendArc(1, NodeId{2}, NodeId{1}, source.key, input.nodes[1].key, 0)};
  removal.removes = {{NodeId{0}, 0, ArcId{0}}};
  removal.logical_deactivations = {{ArcId{0}, input.arcs[0].key, NodeId{0}, NodeId{1}, LogicalArcDeactivateReason::kRemoved, true}};
  removal.reverse_changes = {{NodeId{1}, 0, ArcId{0}, ArcId{1}}};
  removal.appended_base_out_rows = {{NodeId{2}, {ArcId{1}}}};
  removal.appended_base_in_rows = {{NodeId{2}, {}}};
  removal.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, {NodeId{0}, NodeId{2}}};
  auto seed = input.start_seeds[0];
  seed.seed_node = NodeId{2};
  seed.path_start_node = NodeId{2};
  for (auto& row : seed.arrival)
    for (auto& value : row)
      value = 0.25;
  removal.appended_start_seeds = {seed};
  auto first = owner.stage(removal, mode);
  require(!first.computationCode() && first.timingValues().nodes.arrivals.values[1] == 0.25 && owner.publish(std::move(first)),
          "Base remove/reverse-change failed");
  auto change = patchFor(input, 1, removal.after, {3, 3, 3});
  change.arc_appends = {appendArc(2, NodeId{0}, NodeId{1}, input.nodes[0].key, input.nodes[1].key, 0)};
  change.removes = {{NodeId{2}, 1, ArcId{1}}};
  change.logical_deactivations = {{ArcId{1}, removal.arc_appends[0].key, NodeId{2}, NodeId{1}, LogicalArcDeactivateReason::kRemoved, true}};
  change.changes = {{NodeId{0}, 0, std::nullopt, ArcId{2}}};
  change.reverse_changes = {{NodeId{1}, 0, ArcId{1}, ArcId{2}}};
  auto second = owner.stage(change, mode);
  require(!second.computationCode() && second.timingValues().nodes.arrivals.values[1] == 1.0 && owner.publish(std::move(second)),
          "Base tombstone reuse/change failed");
  auto cycle = patchFor(input, 2, change.after, {5, 5, 5});
  auto a = appendNode(3, "cycle_a", AppendedPortRole::kSink), b = appendNode(4, "cycle_b", AppendedPortRole::kSink);
  cycle.node_appends = {a, b};
  cycle.arc_appends = {appendArc(3, NodeId{3}, NodeId{4}, a.key, b.key, 0), appendArc(4, NodeId{4}, NodeId{3}, b.key, a.key, 0)};
  cycle.appended_base_out_rows = {{NodeId{3}, {ArcId{3}}}, {NodeId{4}, {ArcId{4}}}};
  cycle.appended_base_in_rows = {{NodeId{3}, {ArcId{4}}}, {NodeId{4}, {ArcId{3}}}};
  auto failed = owner.stage(cycle, mode);
  require(failed.computationCode() == CsrComputationCode::kCycle && !owner.publish(std::move(failed)) && !owner.poisoned(),
          "actual GPU cycle did not preserve old owner");
  auto healthy = owner.stage(patchFor(input, 2, change.after, change.after), mode);
  require(owner.publish(std::move(healthy)), "healthy publication after cycle failed");
}


void multiBlockReconstruction()
{
  const auto input = csr::test::makeCpuReferenceNetFixture();
  const timing_edit::AppendOnlyHighWater water{2, 2, 1};
  CudaDualCsrSession owner(input, water);
  constexpr std::uint64_t count = 300;
  auto patch = patchFor(input, 0, water, {2 + count, 2 + count, 1});
  std::vector<NodeId> roots{NodeId{0}};
  std::vector<NodeId> order{NodeId{0}};
  std::vector<std::uint64_t> inverse(2 + count, 1);
  inverse[1] = 2;
  for (std::uint64_t id = 2; id < 2 + count; ++id) {
    patch.node_appends.push_back(appendNode(id, "independent_source_" + std::to_string(id), AppendedPortRole::kSource));
    patch.appended_base_out_rows.push_back({NodeId{id}, {}});
    patch.appended_base_in_rows.push_back({NodeId{id}, {}});
    roots.push_back(NodeId{id});
    order.push_back(NodeId{id});
  }
  order.push_back(NodeId{1});
  patch.roots = ValueReplacement<std::vector<NodeId>>{{NodeId{0}}, roots};
  auto candidate = owner.stage(patch, CudaLevelizationMode::kMixed);
  require(!candidate.computationCode(), "multiple CUDA blocks failed mixed reconstruction");
  const auto& result = candidate.timingValues();
  require(result.level_offsets == std::vector<std::uint64_t>{0, count + 1, count + 2} && result.level_nodes == order
              && result.node_to_level == inverse,
          "cross-block new-node placement or inverse map is incorrect");
  require(owner.publish(std::move(candidate)), "cross-block reconstruction could not publish");
  const auto committed = owner.timingValues();
  auto replay = owner.stage(patchFor(input, 1, patch.after, patch.after), CudaLevelizationMode::kMixed);
  require(!replay.computationCode() && replay.timingValues() == committed && owner.publish(std::move(replay)),
          "cross-block reconstructed owner could not continue with an empty batch");
}





}  // namespace
int main(int argc, char** argv)
{
  try {
    if (argc == 4 && std::string(argv[1]) == "--semantic-context-fault") {
      if constexpr (incregpusta_debug_checks_enabled)
        csr::test::checkCudaSemanticFault(argv[3], argv[2]);
      else
        throw std::invalid_argument("semantic fault tests require checks ON");
      std::cout << "semantic CUDA session poison passed\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--lazy-sequence") {
      csr::test::runCudaLazySequence(CudaLevelizationMode::kMixed);
      std::cout << "CUDA lazy commit/query sequence passed\n";
      return 0;
    }
    CudaLevelizationMode mode = CudaLevelizationMode::kFull;
    if (argc > 1 && (std::string(argv[1]) == "--mixed" || std::string(argv[1]) == "--mixed-cross-build-output"))
      mode = CudaLevelizationMode::kMixed;
    std::optional<nlohmann::json> snapshots;
    std::filesystem::path output;
    if ((argc == 3 || (argc == 5 && std::string(argv[3]) == "--replay"))
        && (std::string(argv[1]) == "--cross-build-output" || std::string(argv[1]) == "--mixed-cross-build-output")) {
      output = argv[2];
      if (argc == 5)
        require(std::filesystem::symlink_status(output).type() == std::filesystem::file_type::not_found,
                "replay cross-build output must be a new file");
      else
        std::filesystem::remove(output);
      snapshots = nlohmann::json::array();
    } else
      require(argc == 1 || (argc == 2 && mode == CudaLevelizationMode::kMixed), "unknown arguments; hardware fault modes require the ON-only target");
    if (mode == CudaLevelizationMode::kMixed)
      multiBlockReconstruction();
    csr::test::runCudaDualCsrStorageContracts(mode);
    runCudaClockContracts();
    runCudaSequentialContracts();
    runCudaRcPropagationContracts();
    emptyGraph(mode);
    auxSequence(snapshots ? &*snapshots : nullptr, mode);
    removeChangeAndCycle(mode);
    if (argc == 5)
      replayCrossBuild(argv[4], mode, *snapshots);
    if (snapshots) {
      std::ofstream stream(output);
      stream << *snapshots << '\n';
      stream.close();
      require(bool(stream), "cross-build output write failed");
    }
    std::cout << "CUDA persistent Dual-CSR Aux/seed/ownership contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
