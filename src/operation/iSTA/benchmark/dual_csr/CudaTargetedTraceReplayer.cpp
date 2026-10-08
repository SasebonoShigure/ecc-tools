#include "CudaTargetedTraceReplayer.hpp"

#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "BufferTraceTypes.hpp"
#include "CudaTargetedExecution.hpp"
#include "Database.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaCudaSessionState.hpp"

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

}  // namespace


CudaTargetedTraceResult CudaTargetedTraceReplayer::run(StaCsrSessionContext& session, Database& baseline, const CsrReferenceInput& input,
                                                       const timing_edit::NativeExecutionAuthority& authority, const buffer_trace::ReplayProgram& program,
                                                       std::size_t prefix, StaUpdateStrategy strategy, StaFallbackPolicy fallback, const std::function<void()>& consume_results)
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
  owned.bindEdits(baseline, input, program.baseline(), session.libraryModels(baseline));
  auto& state = *owned.native;
  auto& device = owned.device;
  CudaTargetedTraceResult result;
  result.initialization = device.telemetry();
  result.batches.reserve(prefix);
  static_assert(std::is_nothrow_move_assignable_v<NativeTargetedEditState>);
  static_assert(std::is_nothrow_move_constructible_v<CudaTargetedTraceBatch>);
  result.backend_setup_ms = elapsed(setup_started);
  for (std::size_t index = 0; index < prefix; ++index) {
      const auto started = Clock::now();
      require(authority.active() && state.generation == device.generation(), "native/device generation binding changed");
      auto prepared = NativeTargetedEditPreparer::prepare(baseline, authority, plans[index], state);
      require(prepared.patch.session_token == authority.sessionToken() && prepared.patch.expected_generation.value() + 1 == state.generation.value()
                  && prepared.staged.generation == prepared.patch.target_generation,
              "prepared edit lost its origin binding");
      CudaTargetedTraceBatch batch;
      batch.generation_before = prepared.patch.expected_generation.value();
      batch.generation_after = prepared.staged.generation.value();
      batch.node_count = prepared.staged.node_names.size();
      batch.arc_count = prepared.transaction.database().get_arc_list().size();
      batch.native_prepare_ms = elapsed(started);
      // This optional export is an independent ON diagnostic, never an input to
      // the device patch generator or the default performance workload.
      if constexpr (incregpusta_debug_checks_enabled) {
        auto exported = exportCsrReferenceInputCandidate(prepared.transaction.database(), input.analysis_context, authority.sessionToken().value());
        require(std::holds_alternative<CsrReferenceInput>(exported), "diagnostic canonical export failed");
        batch.diagnostic.emplace();
        batch.diagnostic->canonical_input = std::move(std::get<CsrReferenceInput>(exported));
      }
      auto candidate = detail::stageCudaTargetedBatch(device, prepared.patch, strategy, fallback, batch.execution, batch.rejected_mixed_attempt);
      if constexpr (incregpusta_debug_checks_enabled) {
        batch.diagnostic->values = candidate.timingValues();
        batch.diagnostic->node_names = prepared.staged.node_names;
        batch.diagnostic->arc_native_ranks = prepared.staged.arc_native_ranks;
        batch.diagnostic->device = candidate.debugState();
      }
      // Everything that can throw has finished. Device publication rejects
      // without writes; after success, native publication/ownership moves do not throw.
      require(device.publish(std::move(candidate)), "device candidate no longer belongs to this generation");
      prepared.transaction.commit();
      owned.committed();
      batch.commit_ms = elapsed(started);
      auto phase = Clock::now();
      session.materializeCudaTiming(baseline, authority);
      batch.query_materialize_ms = elapsed(phase);
      phase = Clock::now();
      session.prepareCudaReport(baseline, authority);
      if (consume_results)
        consume_results();
      batch.report_ms = elapsed(phase);
      batch.consumer_end_to_end_ms = elapsed(started);
      batch.device = device.telemetry();
      result.batches.push_back(std::move(batch));
    }
  return result;
}
}  // namespace ista::csr
