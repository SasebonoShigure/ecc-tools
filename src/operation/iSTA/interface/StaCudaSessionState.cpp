// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "StaCudaSessionState.hpp"
#include <stdexcept>
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "StaProductionLifecycle.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingReporter.hpp"
#include "TimingResultScatterInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista {
StaCudaSessionState::StaCudaSessionState(Database& database, const csr::CsrReferenceInput& input)
    : device(csr::CudaDualCsrSession::fromNativeExport(input, {input.nodes.size(), database.get_net_map().size(), input.arcs.size()})),
      _native_view(timing_edit::TimingEditSessionToken{input.source_identity.session_token}),
      _token(input.source_identity.session_token), _analysis_order(input.analysis_order),
      _path_source_order(input.path_source_order), _transition_order(input.transition_order),
      _clock_tags(input.clock_tags)
{
  if (const auto code = device.computationCode())
    throw std::runtime_error("CUDA baseline computation failed: " + std::string(csr::token(*code)));
  device.requireCommitted();
  _baseline_names.reserve(input.nodes.size());
  for (const auto& node : input.nodes)
    _baseline_names.push_back(node.key.full_name);
  _baseline_ranks.resize(input.arcs.size());
  for (const auto& arc : input.arcs)
    _baseline_ranks.at(arc.id.value()) = arc.native_traversal_rank;
}

void StaCudaSessionState::bindEdits(Database& database, const csr::CsrReferenceInput& input,
                                   const timing_edit::BaselineSemanticIdentity& baseline)
{
  device.requireCommitted();
  if (host) {
    if (!host->baseline || *host->baseline != baseline || host->graph.generation != device.generation()
        || history.size() != device.generation().value())
      throw std::invalid_argument("CUDA CSR edit binding differs from this session");
    return;
  }
  if (device.generation().value() != 0 || input.source_identity.session_token != _token)
    throw std::invalid_argument("CUDA edits require this session's generation-zero baseline");
  auto prepared = std::unique_ptr<CudaCsrEditState>(new CudaCsrEditState(
      database, input, timing_edit::TimingEditSessionToken{_token}));
  if (*prepared->baseline != baseline)
    throw std::invalid_argument("CUDA replay baseline differs from this session");
  host = std::move(prepared);
  std::vector<std::string>{}.swap(_baseline_names);
  std::vector<std::optional<std::size_t>>{}.swap(_baseline_ranks);
}

void StaCudaSessionState::committed() noexcept
{
  _csr_report_generation.reset();
  _timing_generation.reset();
  _report_generation.reset();
}

void StaCudaSessionState::materialize(Database& database, const timing_edit::NativeExecutionAuthority& authority)
{
  // Check even a cache hit: pending edits and poisoned sessions cannot serve old results.
  if (_poisoned)
    throw std::logic_error("CUDA result session is poisoned");
  device.requireCommitted();
  const auto generation = device.generation();
  if (host && (host->graph.generation != generation || history.size() != generation.value()))
    throw std::logic_error("CUDA host/device generation mismatch");
  if (_timing_generation == generation)
    return;
  if (host) {
    const auto& values = device.timingValues();
    _native_view.materialize(database, authority, host->graph, host->resolver, history, values);
    _timing_generation = generation;
    return;
  }
  materializeBaseline(database);
}

void StaCudaSessionState::materializeBaseline(Database& database)
{
  if (_poisoned || host)
    throw std::logic_error("baseline materialization requires the unedited CUDA session");
  device.requireCommitted();
  if (device.generation().value() != 0)
    throw std::logic_error("baseline materialization cannot serve an edited generation");
  if (_timing_generation == device.generation()) return;
  const auto& values = device.timingValues();
  const auto& names = _baseline_names;
  const auto& ranks = _baseline_ranks;
  auto prepared = csr::detail::prepareStableTimingResult(
      database, names, ranks, {_analysis_order, _path_source_order, _transition_order, _clock_tags}, values);
  auto* result = std::get_if<csr::detail::PreparedTimingResult>(&prepared);
  if (!result)
    throw std::runtime_error("CUDA current timing cannot be mapped to native state");
  csr::detail::publishPreparedTimingResult(*result);
  _timing_generation = device.generation();
}

void StaCudaSessionState::prepareReport()
{
  device.requireCommitted();
  if (_poisoned || _timing_generation != device.generation())
    throw std::logic_error("CUDA report requires current materialized timing");
  if (_report_generation == device.generation())
    return;
  // Invalidate before calling allocating analysis or its error-reporting helpers.
  // Even failure while formatting an error must force a clean scatter on retry.
  _timing_generation.reset();
  _report_generation.reset();
  const auto result = executeStaLifecyclePipeline(
      {{"TimingAnalyzer", [] { TimingAnalyzer::initInst(); },
         [] { STATA.analyzePropagatedTiming(); }, [] { TimingAnalyzer::destroyInst(); }}});
  if (result.cleanup_failed)
    _poisoned = true;
  if (!result.succeeded) {
    // Path-diversion analysis may edit native point maps before failing.
    // Retry from authoritative cached values, without another propagation.
    _timing_generation.reset();
    _report_generation.reset();
    throw std::runtime_error("CUDA propagated timing report preparation failed: " + result.message);
  }
  _timing_generation = device.generation();
  _report_generation = device.generation();
}
const std::pair<std::string, std::string>& StaCudaSessionState::readReport(Database& database,
    const timing_edit::NativeExecutionAuthority& authority)
{
  device.requireCommitted();
  if (_poisoned || !host || host->graph.generation != device.generation()
      || history.size() != device.generation().value())
    throw std::logic_error("CSR report requires a committed CUDA edit owner");
  if (_csr_report_generation == device.generation()) return _csr_report;
  const auto& graph = host->graph;
  _native_view.prepareMetadata(database, authority, graph, host->resolver, history);
  const auto& values = device.reportValues({}, {});
  std::pair<std::string, std::string> report;
  const auto status = executeStaLifecyclePipeline(
      {{"CSR report", [] {}, [&] {
        csr::detail::CsrReportNodeReader reader(database, graph, host->resolver, values, device.reportClockNames(),
            [&](const csr::CsrReportRequest& request) { return device.prepareReport(request); });
        report = TimingReporter::readCsrReport(reader);
      }, [] {}}},
      StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
  if (status.cleanup_failed) _poisoned = true;
  if (!status.succeeded) throw std::runtime_error("CSR report query failed: " + status.message);
  _csr_report.swap(report);
  _csr_report_generation = device.generation();
  return _csr_report;
}
}  // namespace ista
