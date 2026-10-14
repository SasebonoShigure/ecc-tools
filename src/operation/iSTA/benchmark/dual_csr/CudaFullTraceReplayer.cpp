#include "CudaFullTraceReplayer.hpp"
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include "BufferTraceTypes.hpp"
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
  if (!valid) throw std::runtime_error(std::string("CUDA full trace: ") + message);
}
}  // namespace

CudaFullTraceResult CudaFullTraceReplayer::run(StaCsrSessionContext& session, Database& baseline,
    const CsrReferenceInput& input, const timing_edit::NativeExecutionAuthority& authority,
    const buffer_trace::ReplayProgram& program, std::size_t prefix, const std::function<void()>& consume_results)
{
  using namespace timing_edit;
  const auto setup_started = Clock::now();
  require(buffer_trace::validateReplayProgram(program).empty(), "invalid replay program");
  require(authority.active() && authority.sessionToken().value() == input.source_identity.session_token, "inactive authority");
  const auto& plans = std::visit([](const auto& workload) -> const std::vector<EditBatch>& { return workload.batches; }, program.workload);
  require(prefix > 0 && prefix <= plans.size(), "prefix outside frozen batches");
  auto& owned = session.cudaState(authority);
  try {
    owned.bindEdits(baseline, input, program.baseline());
    auto& host = *owned.host;
    require(owned.device.generation().value() == 0, "trace must start with the loaded baseline");
    CudaFullTraceResult result;
    result.batches.reserve(prefix);
    owned.history.reserve(prefix);
    static_assert(std::is_nothrow_move_constructible_v<EditBatch>);
    result.backend_setup_ms = elapsed(setup_started);
    const auto loop_started = Clock::now();
    for (std::size_t index = 0; index < prefix; ++index) {
      const auto started = Clock::now();
      require(host.graph.generation == owned.device.generation()
                  && plans[index].expected_generation == host.graph.generation, "noncontiguous generation");
      auto record = plans[index];
      auto direct = timing_dual_csr::TimingEditDualCsrDescriptorBuilder::buildDirect(
          host.prepareBatch(plans[index]), authority, session.libraryModels(baseline));
      CudaFullTraceBatch batch;
      batch.generation_before = host.graph.generation.value();
      batch.direct_prepare_ms = elapsed(started);
      auto phase = Clock::now();
      CudaCsrEditState::Patch patch(host, std::move(direct));
      patch.apply();
      batch.host_apply_ms = elapsed(phase);
      auto candidate = owned.device.stageFullCsr(host.graph);
      require(!candidate.computationCode(), "full GPU computation failed");
      batch.node_count = host.graph.nodes.size();
      batch.arc_slots = host.graph.arc_slots.size();
      batch.generation_after = host.graph.generation.value();
      // Capacity and the recovery record precede apply; successful publication cannot allocate.
      require(owned.device.publish(std::move(candidate)), "full GPU commit rejected");
      patch.commit();
      owned.history.push_back(std::move(record));
      owned.committed();
      batch.commit_ms = elapsed(started);
      if (index + 1 == prefix) result.edit_sta_loop_ms = elapsed(loop_started);
      if (consume_results || index + 1 == prefix) {
        phase = Clock::now();
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
      batch.cuda = owned.device.telemetry().cuda;
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
