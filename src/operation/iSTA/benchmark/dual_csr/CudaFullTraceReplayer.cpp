#include "CudaFullTraceReplayer.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "BufferTraceTypes.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NativeDesignEditTranslator.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "TimingEditResolver.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingResultScatterInternal.hpp"
#include "TimingSemanticKey.hpp"

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
void rollback(timing_edit::InstalledNativeDatabase& installed)
{
  require(installed.active() && installed.rollback(), "native rollback failed");
}

}  // namespace
CudaFullTraceResult replayCudaFullTracePrefix(Database& baseline, const StaAnalysisContext& analysis, const timing_edit::NativeExecutionAuthority& authority,
                                              const buffer_trace::ReplayProgram& program, std::size_t prefix)
{
  using namespace timing_edit;
  const auto violations = buffer_trace::validateReplayProgram(program);
  require(violations.empty(), "invalid replay program");
  require(authority.active() && authority.sessionToken().value() != 0, "inactive authority");
  const auto& plans = std::visit([](const auto& workload) -> const std::vector<DesignEditPlan>& { return workload.batches; }, program.workload);
  require(prefix > 0 && prefix <= plans.size(), "prefix outside frozen batches");
  CsrCudaBackend::requireDevice();
  auto resolver = TimingEditResolverState::bindBaseline(baseline);
  auto identity = NativeTimingEditPreparer::captureCurrentIdentity(baseline, TransactionGeneration{0}, analysis, authority.sessionToken());
  require(NativeTimingEditPreparer::matchesDesignName(baseline, plans.front().baseline.design_name)
              && identity.current_fingerprints == plans.front().baseline.fingerprints
              && identity.canonical_pre_state == plans.front().baseline.canonical_pre_state,
          "frozen trace baseline differs from loaded canonical state");
  std::optional<InstalledNativeDatabase> installed;
  CudaFullTraceResult result;
  result.batches.reserve(prefix);
  try {
    for (std::size_t index = 0; index < prefix; ++index) {
      const auto started = Clock::now();
      require(plans[index].expected_generation == identity.generation, "noncontiguous generation");
      Database& current = installed ? installed->database() : baseline;
      const TimingEditPrepareContext context{authority.sessionToken(), plans.front().baseline, identity, analysis};
      auto prepared = NativeTimingEditPreparer::prepare(current, plans[index], context, resolver);
      auto staged_resolver = resolver.stage(prepared);
      CudaFullTraceBatch batch;
      batch.generation_before = identity.generation.value();
      batch.prepare_ms = elapsed(started);
      auto phase = Clock::now();
      auto candidate = NativeDatabaseCandidate::create(current);
      if (installed) {
        rollback(*installed);
        installed.reset();
      }
      const auto translated = NativeDesignEditTranslator::applyNormalizedBatch(candidate.database(), prepared, staged_resolver);
      require(translated.applied() && translated.reasons.empty(), "native edit translation rejected");
      installed.emplace(NativeCandidateExecutor::installAndPrepareGraph(authority, std::move(candidate)));
      batch.native_graph_ms = elapsed(phase);
      phase = Clock::now();
      auto exported = exportCsrReferenceInputCandidate(installed->database(), analysis, authority.sessionToken().value());
      require(std::holds_alternative<CsrReferenceInput>(exported), "full CSR export failed");
      auto input = std::move(std::get<CsrReferenceInput>(exported));
      batch.export_ms = elapsed(phase);
      batch.node_count = input.nodes.size();
      batch.arc_count = input.arcs.size();
      auto output = CsrCudaBackend{}.computeWithTelemetry(input);
      require(std::holds_alternative<CsrTimingResult>(output.computation.outcome), "full GPU computation failed");
      auto timing = std::move(std::get<CsrTimingResult>(output.computation.outcome));
      phase = Clock::now();
      require(detail::publishComputedTimingResult(installed->database(), input, timing, authority.sessionToken().value()) == CsrScatterCode::kSuccess,
              "controlled timing scatter failed");
      auto post_identity
          = NativeTimingEditPreparer::captureCurrentIdentity(installed->database(), prepared.targetGeneration(), analysis, authority.sessionToken());
      batch.scatter_ms = elapsed(phase);
      batch.generation_after = post_identity.generation.value();
      batch.cuda = std::move(output.telemetry);
      batch.end_to_end_ms = elapsed(started);
      if constexpr (incregpusta_debug_checks_enabled) {
        batch.diagnostic_input.emplace(std::move(input));
        batch.diagnostic_timing.emplace(std::move(timing));
      }
      // All throwing preparation precedes publication. These owned values never
      // refer to the candidate that will be restored before the next install.
      result.batches.push_back(std::move(batch));
      resolver = std::move(staged_resolver);
      identity = std::move(post_identity);
    }
    rollback(*installed);
    installed.reset();
    result.native_final_rollback_completed = true;
  } catch (...) {
    if (installed && installed->active() && !installed->rollback())
      throw std::runtime_error("CUDA full trace failure cleanup could not restore native baseline");
    throw;
  }
  return result;
}
}  // namespace ista::csr
