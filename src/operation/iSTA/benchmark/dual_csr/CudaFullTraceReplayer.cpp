#include "CudaFullTraceReplayer.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "BufferTraceTypes.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NativeEditTransaction.hpp"
#include "NativeFullEditPreparer.hpp"
#include "TimingEditResolver.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingResultScatterInternal.hpp"
#include "TimingSemanticKey.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaCudaSessionState.hpp"
#include "Database.hpp"

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
    throw std::runtime_error(std::string("CUDA full trace: ") + message);
}

}  // namespace
CudaFullTraceResult replayCudaFullTracePrefix(StaCsrSessionContext& session, Database& baseline, const CsrReferenceInput& baseline_input, const timing_edit::NativeExecutionAuthority& authority,
                                              const buffer_trace::ReplayProgram& program, std::size_t prefix,
                                              const std::function<void()>& consume_results)
{
  using namespace timing_edit;
  const auto setup_started = Clock::now();
  const auto violations = buffer_trace::validateReplayProgram(program);
  require(violations.empty(), "invalid replay program");
  require(authority.active() && authority.sessionToken().value() != 0, "inactive authority");
  const auto& plans = std::visit([](const auto& workload) -> const std::vector<EditBatch>& { return workload.batches; }, program.workload);
  require(prefix > 0 && prefix <= plans.size(), "prefix outside frozen batches");
  auto& owned = session.cudaState(authority);
  owned.device.requireCommitted();
  require(owned.device.generation().value() == 0, "trace must start with the loaded baseline");
  auto state = NativeFullEditPreparer::bindBaseline(baseline, baseline_input, program.baseline());
  const auto& analysis = baseline_input.analysis_context;
  CudaFullTraceResult result;
  result.batches.reserve(prefix);
  result.backend_setup_ms = elapsed(setup_started);
  for (std::size_t index = 0; index < prefix; ++index) {
      const auto started = Clock::now();
      require(plans[index].expected_generation == state.generation(), "noncontiguous generation");
      Database& current = baseline;
      auto prepared = NativeFullEditPreparer::prepare(current, authority, plans[index], state);
      CudaFullTraceBatch batch;
      batch.generation_before = state.generation().value();
      batch.prepare_ms = elapsed(started);
      auto phase = Clock::now();
      auto& transaction = prepared.transaction();
      NativeCandidateExecutor::prepareFullGraph(transaction);
      batch.native_graph_ms = elapsed(phase);
      phase = Clock::now();
      auto exported = exportCsrReferenceInputCandidate(transaction.database(), analysis, authority.sessionToken().value(), session.libraryModels(baseline));
      require(std::holds_alternative<CsrReferenceInput>(exported), "full CSR export failed");
      auto input = std::move(std::get<CsrReferenceInput>(exported));
      batch.export_ms = elapsed(phase);
      batch.node_count = input.nodes.size();
      batch.arc_count = input.arcs.size();
      std::vector<std::string> node_names;
      node_names.reserve(input.nodes.size());
      for (const auto& node : input.nodes)
        node_names.push_back(node.key.full_name);
      std::vector<std::optional<std::size_t>> arc_ranks(input.arcs.size());
      for (const auto& arc : input.arcs)
        arc_ranks.at(arc.id.value()) = arc.native_traversal_rank;
      auto candidate = owned.device.stageFullNativeExport(input, {input.nodes.size(), current.get_net_map().size(), input.arcs.size()});
      require(!candidate.computationCode(), "full GPU computation failed");
      batch.generation_after = prepared.targetGeneration().value();
      if constexpr (incregpusta_debug_checks_enabled) {
        CsrTimingResult timing;
        timing.schema_version = input.schema_version;
        timing.source_identity = input.source_identity;
        static_cast<CsrTimingValues&>(timing) = candidate.timingValues();
        batch.diagnostic_input.emplace(std::move(input));
        batch.diagnostic_timing.emplace(std::move(timing));
      }
      // All allocating preparation precedes the no-throw native commit.
      result.batches.push_back(std::move(batch));
      require(owned.device.publish(std::move(candidate)), "full GPU commit rejected");
      require(prepared.commit(), "native full commit rejected");
      owned.committedFull(std::move(node_names), std::move(arc_ranks));
      result.batches.back().commit_ms = elapsed(started);
      phase = Clock::now();
      session.materializeCudaTiming(current, authority);
      result.batches.back().query_materialize_ms = elapsed(phase);
      phase = Clock::now();
      session.prepareCudaReport(current, authority);
      if (consume_results)
        consume_results();
      result.batches.back().report_ms = elapsed(phase);
      result.batches.back().consumer_end_to_end_ms = elapsed(started);
      result.batches.back().cuda = owned.device.telemetry().cuda;
    }
  return result;
}
}  // namespace ista::csr
