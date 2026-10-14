#include "CudaTargetedTraceReplayer.hpp"
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include "BufferTraceTypes.hpp"
#include "CudaTargetedExecution.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista::csr {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void require(bool valid, const char* message)
{
  if (!valid) throw std::runtime_error(std::string("CUDA targeted trace: ") + message);
}
}  // namespace

CudaTargetedTraceResult CudaTargetedTraceReplayer::run(StaCsrSessionContext& session, Database& baseline,
    const CsrReferenceInput& input, const timing_edit::NativeExecutionAuthority& authority,
    const buffer_trace::ReplayProgram& program, std::size_t prefix, StaUpdateStrategy strategy,
    StaFallbackPolicy fallback, const std::function<void()>& consume_results)
{
  using namespace timing_edit;
  const auto setup_started = Clock::now();
  require(strategy == StaUpdateStrategy::kFull || strategy == StaUpdateStrategy::kIncremental, "unknown update strategy");
  require(fallback == StaFallbackPolicy::kError || fallback == StaFallbackPolicy::kFullWithRecord, "unknown fallback policy");
  require(authority.active() && authority.sessionToken().value() == input.source_identity.session_token,
          "baseline is not bound to the current operation guard");
  require(buffer_trace::validateReplayProgram(program).empty(), "invalid frozen replay program");
  const auto& plans = std::visit([](const auto& workload) -> const std::vector<EditBatch>& { return workload.batches; }, program.workload);
  require(prefix > 0 && prefix <= plans.size(), "prefix outside frozen batches");
  auto& owned = session.cudaState(authority);
  try {
    owned.bindEdits(baseline, input, program.baseline());
    auto& host = *owned.host;
    auto& device = owned.device;
    require(device.generation().value() == 0, "trace must start with the loaded baseline");
    CudaTargetedTraceResult result;
    result.initialization = device.telemetry();
    result.batches.reserve(prefix);
    owned.history.reserve(prefix);
    static_assert(std::is_nothrow_move_constructible_v<EditBatch>);
    static_assert(std::is_nothrow_move_constructible_v<CudaTargetedTraceBatch>);
    result.backend_setup_ms = elapsed(setup_started);
    const auto loop_started = Clock::now();
    for (std::size_t index = 0; index < prefix; ++index) {
      const auto started = Clock::now();
      require(authority.active() && host.graph.generation == device.generation(), "host/device generation binding changed");
      auto record = plans[index];
      auto& models = session.libraryModels(baseline);
      auto direct = timing_dual_csr::TimingEditDualCsrDescriptorBuilder::buildDirect(
          host.prepareBatch(plans[index]), authority, models);
      auto device_patch = detail::makeCudaTargetedPatch(host.graph, direct.descriptor(), models);
      CudaTargetedTraceBatch batch;
      batch.generation_before = host.graph.generation.value();
      CudaCsrEditState::Patch patch(host, std::move(direct));
      patch.apply();
      batch.generation_after = host.graph.generation.value();
      batch.node_count = host.graph.nodes.size();
      batch.arc_slots = host.graph.arc_slots.size();
      batch.direct_prepare_ms = elapsed(started);
      auto candidate = detail::stageCudaTargetedBatch(
          device, device_patch, strategy, fallback, batch.execution, batch.rejected_mixed_attempt);
      require(device.publish(std::move(candidate)), "device candidate no longer belongs to this generation");
      patch.commit();
      owned.history.push_back(std::move(record));
      owned.committed();
      batch.commit_ms = elapsed(started);
      if (index + 1 == prefix) result.edit_sta_loop_ms = elapsed(loop_started);
      if (consume_results || index + 1 == prefix) {
        auto phase = Clock::now();
        static_cast<void>(owned.device.timingValues());
        batch.query_materialize_ms = elapsed(phase);
        phase = Clock::now();
        static_cast<void>(session.readCudaReport(baseline, authority));
        if (consume_results) consume_results();
        batch.report_ms = elapsed(phase);
        batch.results_consumed = true;
      }
      batch.consumer_end_to_end_ms = elapsed(started);
      if constexpr (incregpusta_debug_checks_enabled)
        batch.diagnostic.emplace(observeCudaTrace(session, baseline, authority));
      batch.device = device.telemetry();
      result.batches.push_back(std::move(batch));
    }
    return result;
  } catch (...) {
    // Unwind host/device undo before propagating a damaged device to the whole session.
    if (owned.device.poisoned()) authority.poison();
    throw;
  }
}
}  // namespace ista::csr
