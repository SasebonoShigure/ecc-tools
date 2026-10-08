// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaSessionState.hpp"
#include "StaCsrSessionContext.hpp"
#include "CpuDualCsrBaseline.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "StaProductionLifecycle.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingReporter.hpp"
#include <sstream>
#include "TimingResultScatterInternal.hpp"
#if ISTA_ENABLE_CUDA
#include "StaCudaSessionState.hpp"
#endif

namespace ista {
izh::PhysicalPlacementRows& PhysicalStaSessionState::placementRows()
{
  if (!_placement) _placement = std::make_unique<izh::PhysicalPlacementRows>(design);
  return *_placement;
}
void PhysicalStaSessionState::bindBaseline(Database& database, const csr::CsrReferenceInput& input, StaCsrSessionContext& context)
{
  using namespace timing_edit;
  if (full || cpu || cpu_native || input.source_identity.session_token != context.token()
      || input.source_identity.graph_generation != 0 || &database != &STADM.getDatabase())
    throw std::logic_error("physical baseline binding requires a fresh canonical session");
  if (request.backend == StaBackend::kCpu && request.update_strategy == StaUpdateStrategy::kIncremental) {
    auto initial = timing_dual_csr::makeCpuDualCsrBaseline(database, input, TimingEditSessionToken{context.token()});
    cpu_native.emplace(NativeTargetedEditPreparer::bindBaseline(database, input, *initial.baseline, context.libraryModels(database)));
    cpu = std::make_unique<timing_dual_csr::DualCsrTransactionCoordinator>(std::move(initial));
    return;
  }
  auto identity = NativeTimingEditPreparer::identityFromInput(database, input, TransactionGeneration{0});
  const BaselineSemanticIdentity baseline{database.get_design_name(), std::move(identity.current_fingerprints),
                                          std::move(identity.canonical_pre_state)};
  if (request.update_strategy == StaUpdateStrategy::kFull)
    full.reset(new NativeFullEditState(NativeFullEditPreparer::bindBaseline(database, input, baseline)));
  if (request.backend == StaBackend::kCuda) {
#if ISTA_ENABLE_CUDA
    if (context._cuda) throw std::logic_error("physical baseline already has a CUDA owner");
    context._cuda.reset(new StaCudaSessionState(database, input));
    if (request.update_strategy == StaUpdateStrategy::kIncremental)
      context._cuda->bindEdits(database, input, baseline, context.libraryModels(database));
#else
    throw std::logic_error("CUDA physical STA was not built");
#endif
  } else {
    _timing_generation = TransactionGeneration{0};
    _report_generation = TransactionGeneration{0};
  }
}
timing_edit::TransactionGeneration PhysicalStaSessionState::generation(const StaCsrSessionContext& context) const
{
  if (full) return full->generation();
  if (cpu) return cpu->committed().generation;
#if ISTA_ENABLE_CUDA
  if (context._cuda) {
    context._cuda->device.requireCommitted();
    return context._cuda->device.generation();
  }
#else
  static_cast<void>(context);
#endif
  throw std::logic_error("physical session has no committed backend");
}
void PhysicalStaSessionState::materialize(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority)
{
  if (!authority.readyForQuery() || authority.sessionToken().value() != context.token() || &database != &STADM.getDatabase())
    throw std::logic_error("physical timing query requires the committed session");
  if (request.backend == StaBackend::kCuda) {
#if ISTA_ENABLE_CUDA
    context.materializeCudaTiming(database, authority);
    return;
#else
    throw std::logic_error("CUDA physical STA was not built");
#endif
  }
  const auto current = generation(context);
  if (!cpu) return;
  if (cpu->poisoned() || !cpu_native || cpu_native->generation != current)
    throw std::logic_error("physical CPU result owner is poisoned or stale");
  if (_timing_generation == current) return;
  const auto& committed = cpu->committed();
  const auto& layout = committed.graph.reference_payload;
  auto prepared = csr::detail::prepareStableTimingResult(database, cpu_native->node_names, cpu_native->arc_native_ranks,
      {layout.analysis_order, layout.path_source_order, layout.transition_order, layout.clock_tags}, committed.timing.values);
  auto* result = std::get_if<csr::detail::PreparedTimingResult>(&prepared);
  if (!result) throw std::runtime_error("physical CPU timing cannot be mapped to native state");
  csr::detail::publishPreparedTimingResult(*result);
  _timing_generation = current;
}
void PhysicalStaSessionState::prepareReport(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority)
{
  if (request.backend == StaBackend::kCuda) {
#if ISTA_ENABLE_CUDA
    context.prepareCudaReport(database, authority);
    return;
#else
    throw std::logic_error("CUDA physical STA was not built");
#endif
  }
  materialize(database, context, authority);
  const auto current = generation(context);
  if (!cpu || _report_generation == current) return;
  _timing_generation.reset();
  _report_generation.reset();
  const auto result = executeStaLifecyclePipeline(
      {{"TimingAnalyzer", [] { TimingAnalyzer::initInst(); }, [] { STATA.analyzePropagatedTiming(); }, [] { TimingAnalyzer::destroyInst(); }}},
      StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
  if (result.cleanup_failed) authority.poison();
  if (!result.succeeded) throw std::runtime_error("physical CPU report preparation failed: " + result.message);
  _timing_generation = current;
  _report_generation = current;
}
PhysicalStaCurrentResult PhysicalStaSessionState::read(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority, bool summary_only)
{
  prepareReport(database, context, authority);
  PhysicalStaCurrentResult result;
  result.generation = generation(context).value();
  TimingReporter reporter;
  std::ostringstream qor;
  qor.exceptions(std::ios::badbit | std::ios::failbit);
  reporter.outputQorSummaryReport(&qor);
  result.qor_json = std::move(qor).str();
  if (!summary_only) {
    const auto& points = database.get_timing_point_map();
    result.timing.reserve(points.size());
    for (const auto& [name, point] : points)
      result.timing.emplace_back(name, std::array{point.get_arrival(), point.get_required(), point.get_slack()});
    std::ostringstream paths;
    paths.exceptions(std::ios::badbit | std::ios::failbit);
    reporter.outputTimingPathsJson(&paths);
    result.paths_json = std::move(paths).str();
  }
  return result;
}
}  // namespace ista
