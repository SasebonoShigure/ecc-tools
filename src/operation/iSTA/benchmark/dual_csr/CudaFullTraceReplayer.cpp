#include "CudaFullTraceReplayer.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "BufferTraceTypes.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NativeEditTransaction.hpp"
#include "NativeTimingEditPreparer.hpp"
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
CudaFullTraceResult replayCudaFullTracePrefix(StaCsrSessionContext& session, Database& baseline, const StaAnalysisContext& analysis, const timing_edit::NativeExecutionAuthority& authority,
                                              const buffer_trace::ReplayProgram& program, std::size_t prefix)
{
  using namespace timing_edit;
  const auto violations = buffer_trace::validateReplayProgram(program);
  require(violations.empty(), "invalid replay program");
  require(authority.active() && authority.sessionToken().value() != 0, "inactive authority");
  const auto& plans = std::visit([](const auto& workload) -> const std::vector<EditBatch>& { return workload.batches; }, program.workload);
  require(prefix > 0 && prefix <= plans.size(), "prefix outside frozen batches");
  auto& owned = session.cudaState(authority);
  owned.device.requireCommitted();
  require(owned.device.generation().value() == 0, "trace must start with the loaded baseline");
  auto resolver = TimingEditResolverState::bindBaseline(baseline);
  auto identity = NativeTimingEditPreparer::captureCurrentIdentity(baseline, TransactionGeneration{0}, analysis, authority.sessionToken());
  require(NativeTimingEditPreparer::matchesDesignName(baseline, program.baseline().design_name)
              && identity.current_fingerprints == program.baseline().fingerprints
              && identity.canonical_pre_state == program.baseline().canonical_pre_state,
          "frozen trace baseline differs from loaded canonical state");
  TimingEditPrepareContext context{authority.sessionToken(), program.baseline(), identity, analysis};
  context.library_models = &session.libraryModels(baseline);
  CudaFullTraceResult result;
  result.batches.reserve(prefix);
  for (std::size_t index = 0; index < prefix; ++index) {
      const auto started = Clock::now();
      require(plans[index].expected_generation == identity.generation, "noncontiguous generation");
      Database& current = baseline;
      auto prepared = NativeTimingEditPreparer::prepare(current, plans[index], context, resolver);
      PreparedTimingEditResolverPatch resolver_patch(resolver, resolver.prepare(prepared));
      CudaFullTraceBatch batch;
      batch.generation_before = identity.generation.value();
      batch.prepare_ms = elapsed(started);
      auto phase = Clock::now();
      auto transaction = NativeEditTransaction::prepareFull(authority, prepared, resolver_patch.view());
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
      auto post_identity
          = NativeTimingEditPreparer::identityFromInput(transaction.database(), input, prepared.targetGeneration());
      batch.generation_after = post_identity.generation.value();
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
      resolver_patch.apply();
      resolver_patch.commit();
      identity = std::move(post_identity);
      transaction.commit();
      owned.committedFull(std::move(node_names), std::move(arc_ranks));
      result.batches.back().commit_ms = elapsed(started);
      phase = Clock::now();
      session.materializeCudaTiming(current, authority);
      result.batches.back().query_materialize_ms = elapsed(phase);
      phase = Clock::now();
      session.prepareCudaReport(current, authority);
      result.batches.back().report_ms = elapsed(phase);
      result.batches.back().consumer_end_to_end_ms = elapsed(started);
      result.batches.back().cuda = owned.device.telemetry().cuda;
    }
  return result;
}
}  // namespace ista::csr
