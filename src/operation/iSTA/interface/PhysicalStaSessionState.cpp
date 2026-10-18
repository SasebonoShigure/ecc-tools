// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaSessionState.hpp"
#include "PhysicalGreedyScoring.hpp"
#include "StaCsrSessionContext.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "StaProductionLifecycle.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingReporter.hpp"
#include <sstream>
#include <algorithm>
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
void PhysicalStaSessionState::bindNativeBaseline(Database& database, const timing_edit::NativeExecutionAuthority& authority)
{
  if (full || request.backend != StaBackend::kCpu || request.update_strategy != StaUpdateStrategy::kFull)
    throw std::logic_error("physical native baseline requires a fresh CPU/full session");
  full.reset(new timing_edit::NativeFullEditState(timing_edit::NativeFullEditPreparer::bindBaseline(database, authority)));
  _timing_generation = timing_edit::TransactionGeneration{0};
}
void PhysicalStaSessionState::bindBaseline(Database& database, const csr::CsrReferenceInput& input, StaCsrSessionContext& context)
{
  using namespace timing_edit;
  if (full || input.source_identity.session_token != context.token()
      || input.source_identity.graph_generation != 0 || &database != &STADM.getDatabase())
    throw std::logic_error("physical baseline binding requires a fresh canonical session");
  if (request.backend == StaBackend::kCuda) {
#if ISTA_ENABLE_CUDA
    if (context._cuda || cuda_graph) throw std::logic_error("physical baseline already has a CUDA owner");
    cuda_graph.reset(new CudaCsrEditState(database, input, TimingEditSessionToken{context.token()}));
    context._cuda.reset(new StaCudaSessionState(database, input));
    // Physical queries resolve the current native view from stable CSR keys.
    std::vector<std::string>{}.swap(context._cuda->_baseline_names);
    std::vector<std::optional<std::size_t>>{}.swap(context._cuda->_baseline_ranks);
    _native_generation = TransactionGeneration{0};
#else
    throw std::logic_error("CUDA physical STA was not built");
#endif
  } else {
    throw std::logic_error("physical CSR binding requires a CSR backend");
  }
}
timing_edit::TransactionGeneration PhysicalStaSessionState::generation(const StaCsrSessionContext& context) const
{
  if (full) return full->generation();
#if ISTA_ENABLE_CUDA
  if (context._cuda) {
    context._cuda->device.requireCommitted();
    if (!cuda_graph || context._cuda->_poisoned || cuda_graph->graph.generation != context._cuda->device.generation())
      throw std::logic_error("physical host/device generation differs");
    return cuda_graph->graph.generation;
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
  const auto current = generation(context);
  if (full) return;
  if (_timing_generation == current) return;
#if ISTA_ENABLE_CUDA
  if (cuda_graph) {
    try {
      materializeCsr(database, authority, cuda_graph->graph, context._cuda->device.timingValues());
    } catch (...) {
      if (context._cuda->_poisoned || context._cuda->device.poisoned()) authority.poison();
      throw;
    }
    return;
  }
#endif
  throw std::logic_error("physical session has no committed backend");
}
void PhysicalStaSessionState::prepareReport(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority)
{
  materialize(database, context, authority);
  const auto current = generation(context);
  if (_report_generation == current) return;
  _timing_generation.reset();
  _report_generation.reset();
  const auto result = executeStaLifecyclePipeline(
      {{"TimingAnalyzer", [] { TimingAnalyzer::initInst(); }, [] { STATA.analyzePropagatedTiming(); }, [] { TimingAnalyzer::destroyInst(); }}},
      StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
  if (result.cleanup_failed) authority.poison();
  if (!result.succeeded) throw std::runtime_error("physical propagated report preparation failed: " + result.message);
  _timing_generation = current;
  _report_generation = current;
}
PhysicalGreedySelection PhysicalStaSessionState::selectBuffers(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority, std::span<const std::string> cells,
    std::uint64_t top_k, std::uint64_t max_buffers)
{
  if (!authority.readyForQuery() || authority.sessionToken().value() != context.token() || &database != &STADM.getDatabase())
    throw std::logic_error("physical candidate query requires the committed session");
  const auto current = generation(context);
  auto& models = context.libraryModels(database);
  if (full)
    return selectPhysicalBuffers(database, models, full->resolver(), current.value(), cells, top_k, max_buffers, technology.dbu_per_um);
#if ISTA_ENABLE_CUDA
  if (cuda_graph) {
    try {
      const auto prepared = prepareCsrPhysicalBuffers(database, models, design, cuda_graph->graph, cuda_graph->index, cuda_graph->resolver);
      return selectCsrPhysicalBuffers(database, models, design, cuda_graph->graph, prepared,
          context._cuda->device.candidateTiming(prepared.node_ids), cells, top_k, max_buffers, technology.dbu_per_um);
    } catch (...) {
      if (context._cuda->_poisoned || context._cuda->device.poisoned()) authority.poison();
      throw;
    }
  }
#endif
  throw std::logic_error("physical session has no committed backend");
}
PhysicalStaCurrentResult PhysicalStaSessionState::read(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority, bool summary_only)
{
  if (!authority.readyForQuery() || authority.sessionToken().value() != context.token() || &database != &STADM.getDatabase())
    throw std::logic_error("physical result query requires the committed session");
  static_cast<void>(generation(context));
#if ISTA_ENABLE_CUDA
  if (cuda_graph) {
    try {
      return readCsr(database, authority, cuda_graph->graph, context._cuda->device.timingValues(), summary_only);
    } catch (...) {
      if (context._cuda->_poisoned || context._cuda->device.poisoned()) authority.poison();
      throw;
    }
  }
#endif
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
PhysicalStaCurrentResult PhysicalStaSessionState::readCsr(Database& database,
    const timing_edit::NativeExecutionAuthority& authority, const timing_dual_csr::DualCsrState& graph,
    const csr::CsrTimingValues& values, bool summary_only)
{
  prepareCsrView(database, authority, graph, nullptr);
  if (_csr_report_generation != graph.generation || (!summary_only && _csr_report.second.empty())) {
    std::pair<std::string, std::string> report;
    const auto status = executeStaLifecyclePipeline(
        {{"CSR report", [] {}, [&] {
          const auto targets = csr::detail::resolveDualCsrTimingTargets(database, graph);
          const auto& layout = graph.reference_payload;
          csr::detail::CsrReportNodeReader reader(targets,
              {layout.analysis_order, layout.path_source_order, layout.transition_order, layout.clock_tags}, values);
          report = TimingReporter::readCsrReport(reader, summary_only);
        }, [] {}}},
        StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
    if (status.cleanup_failed) authority.poison();
    if (!status.succeeded) throw std::runtime_error("CSR report query failed: " + status.message);
    _csr_report.swap(report);
    _csr_report_generation = graph.generation;
  }
  PhysicalStaCurrentResult result;
  result.generation = graph.generation.value();
  result.qor_json = _csr_report.first;
  if (!summary_only) {
    result.paths_json = _csr_report.second;
    result.timing.reserve(graph.nodes.size());
    for (const auto& node : graph.nodes) {
      const auto id = node.id.value();
      result.timing.emplace_back(node.key.full_name,
          std::array{values.nodes.arrivals.values[id], values.nodes.requireds.values[id], values.nodes.slacks.values[id]});
    }
    std::ranges::sort(result.timing, {}, [](const auto& entry) -> const std::string& { return entry.first; });
  }
  return result;
}
}  // namespace ista
