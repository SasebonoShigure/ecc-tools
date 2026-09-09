#include "CudaTargetedTraceReplayer.hpp"

#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "BufferTraceTypes.hpp"
#include "CudaTargetedExecutionInternal.hpp"
#include "Database.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista::csr {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void require(bool valid, const char* message)
{
  if (!valid)
    throw std::runtime_error(std::string("CUDA targeted trace: ") + message);
}

detail::StableScatterTargets resolveTargets(Database& database, const timing_edit::NativeTargetedEditState& state)
{
  detail::StableScatterTargets result;
  result.nodes.reserve(state.node_names.size());
  for (const auto& name : state.node_names) {
    auto point = database.get_timing_point_map().find(name);
    require(point != database.get_timing_point_map().end(), "stable node no longer exists in candidate");
    result.nodes.push_back({&point->second, &name});
  }
  result.arcs.resize(state.arc_native_ranks.size());
  for (std::size_t id = 0; id < state.arc_native_ranks.size(); ++id) {
    if (!state.arc_native_ranks[id])
      continue;
    const auto rank = *state.arc_native_ranks[id];
    require(rank < database.get_arc_list().size(), "stable Arc rank outside candidate");
    result.arcs[id] = {&database.get_arc_list()[rank], rank};
  }
  result.timing_order = &database.get_timing_order_list();
  return result;
}
}  // namespace

CudaDualCsrSession detail::stageCudaTargetedBatch(CudaDualCsrSession& owner, const CudaDualCsrPatch& patch, StaUpdateStrategy strategy,
                                                  StaFallbackPolicy fallback, CudaTargetedTraceBatch& batch)
{
  const bool incremental = strategy == StaUpdateStrategy::kIncremental;
  batch.execution = {StaBackend::kCuda,
                     strategy,
                     fallback,
                     StaBackend::kCuda,
                     strategy,
                     incremental ? StaExecutionKind::kIncrementalUpdate : StaExecutionKind::kFullUpdate,
                     StaPropagationScope::kFullGraph,
                     incremental,
                     false,
                     std::nullopt};
  std::optional<CudaDualCsrSession> candidate(owner.stage(patch, incremental ? CudaLevelizationMode::kMixed : CudaLevelizationMode::kFull));
  if (candidate->computationCode() && candidate->telemetry().mixed_failure == timing_incremental_levelization::MixedStageFailureCode::kOldLevelOrderConflict
      && fallback == StaFallbackPolicy::kFullWithRecord) {
    batch.rejected_mixed_attempt = candidate->telemetry();
    // Retry the same uncommitted edit against the healthy origin. Never
    // clear a candidate status or use fallback for cycles/CUDA failures.
    candidate.reset();
    candidate.emplace(owner.stage(patch, CudaLevelizationMode::kFull));
    batch.execution.executed_update_strategy = StaUpdateStrategy::kFull;
    batch.execution.execution_kind = StaExecutionKind::kFallbackFull;
    batch.execution.incremental_batch_executed = false;
    batch.execution.fallback_to_full = true;
    batch.execution.fallback_reason = StaFallbackReason::kOldLevelOrderConflict;
  }
  require(!candidate->computationCode(), "targeted GPU update/propagation failed");
  require(!validateStaExecutionRecord(batch.execution), "inconsistent actual CUDA execution record");
  return std::move(*candidate);
}

CudaTargetedTraceResult CudaTargetedTraceReplayer::run(Database& baseline, const CsrReferenceInput& input,
                                                       const timing_edit::NativeExecutionAuthority& authority, const buffer_trace::ReplayProgram& program,
                                                       std::size_t prefix, StaUpdateStrategy strategy, StaFallbackPolicy fallback)
{
  using namespace timing_edit;
  require(strategy == StaUpdateStrategy::kFull || strategy == StaUpdateStrategy::kIncremental, "unknown update strategy");
  require(fallback == StaFallbackPolicy::kError || fallback == StaFallbackPolicy::kFullWithRecord, "unknown fallback policy");
  require(authority.active() && authority.sessionToken().value() == input.source_identity.session_token,
          "baseline is not bound to the current operation guard");
  require(buffer_trace::validateReplayProgram(program).empty(), "invalid frozen replay program");
  const auto& plans = std::visit([](const auto& workload) -> const std::vector<DesignEditPlan>& { return workload.batches; }, program.workload);
  require(prefix > 0 && prefix <= plans.size(), "prefix outside frozen batches");
  auto state = NativeTargetedEditPreparer::bindBaseline(baseline, input, plans.front().baseline);
  CudaDualCsrSession device(input, state.resolver.highWater());
  require(!device.computationCode(), "initial device graph computation failed");
  CudaTargetedTraceResult result;
  result.initialization = device.telemetry();
  result.batches.reserve(prefix);
  std::optional<InstalledNativeDatabase> initial_owner;
  std::optional<InstalledNativeDatabase> previous_owner;
  const detail::TimingScatterLayout layout{input.analysis_order, input.path_source_order, input.transition_order, input.clock_tags};
  static_assert(std::is_nothrow_move_assignable_v<NativeTargetedEditState>);
  static_assert(std::is_nothrow_move_constructible_v<CudaTargetedTraceBatch>);
  try {
    for (std::size_t index = 0; index < prefix; ++index) {
      const auto started = Clock::now();
      require(authority.active() && state.generation == device.generation(), "native/device generation binding changed");
      auto& current = initial_owner ? initial_owner->database() : baseline;
      auto prepared = NativeTargetedEditPreparer::prepare(current, plans[index], state);
      require(prepared.patch.session_token == authority.sessionToken() && prepared.patch.expected_generation == state.generation
                  && prepared.staged.generation == prepared.patch.target_generation,
              "prepared edit lost its origin binding");
      CudaTargetedTraceBatch batch;
      batch.generation_before = state.generation.value();
      batch.generation_after = prepared.staged.generation.value();
      batch.node_count = prepared.staged.node_names.size();
      batch.arc_count = prepared.candidate.database().get_arc_list().size();
      batch.native_prepare_ms = elapsed(started);
      auto phase = Clock::now();
      previous_owner.emplace(NativeCandidateExecutor::installPreparedGraph(authority, std::move(prepared.candidate)));
      batch.native_install_ms = elapsed(phase);
      // This optional export is an independent ON diagnostic, never an input to
      // the device patch generator or the default performance workload.
      if constexpr (incregpusta_debug_checks_enabled) {
        auto exported = exportCsrReferenceInputCandidate(previous_owner->database(), input.analysis_context, authority.sessionToken().value());
        require(std::holds_alternative<CsrReferenceInput>(exported), "diagnostic canonical export failed");
        batch.diagnostic.emplace();
        batch.diagnostic->canonical_input = std::move(std::get<CsrReferenceInput>(exported));
      }
      auto candidate = detail::stageCudaTargetedBatch(device, prepared.patch, strategy, fallback, batch);
      phase = Clock::now();
      auto targets = resolveTargets(previous_owner->database(), prepared.staged);
      auto scatter = detail::prepareResolvedTimingResult(targets, layout, candidate.timingValues());
      auto* publication = std::get_if<detail::PreparedTimingResult>(&scatter);
      require(publication != nullptr, "computed values could not prepare native scatter");
      batch.scatter_ms = elapsed(phase);
      batch.device = candidate.telemetry();
      if constexpr (incregpusta_debug_checks_enabled) {
        batch.diagnostic->values = candidate.timingValues();
        batch.diagnostic->node_names = prepared.staged.node_names;
        batch.diagnostic->arc_native_ranks = prepared.staged.arc_native_ranks;
        batch.diagnostic->device = candidate.debugState();
      }
      // Everything that can throw has finished. Device publication rejects
      // without writes; after success, native publication/ownership moves do not throw.
      require(device.publish(std::move(candidate)), "device candidate no longer belongs to this generation");
      detail::publishPreparedTimingResult(*publication);
      state = std::move(prepared.staged);
      if (!initial_owner)
        initial_owner.emplace(std::move(*previous_owner));
      else
        previous_owner->commit();
      previous_owner.reset();
      batch.end_to_end_ms = elapsed(started);
      result.batches.push_back(std::move(batch));
    }
    require(initial_owner && initial_owner->rollback(), "final baseline rollback failed");
    initial_owner.reset();
    result.native_final_rollback_completed = true;
  } catch (...) {
    const bool previous_restored = !previous_owner || !previous_owner->active() || previous_owner->rollback();
    const bool baseline_restored = !initial_owner || !initial_owner->active() || initial_owner->rollback();
    if (!previous_restored || !baseline_restored)
      throw std::runtime_error("CUDA targeted trace failed to restore native baseline");
    throw;
  }
  return result;
}
}  // namespace ista::csr
