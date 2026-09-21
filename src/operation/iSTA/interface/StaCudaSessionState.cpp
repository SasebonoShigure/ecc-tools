// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "StaCudaSessionState.hpp"
#include <stdexcept>
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "StaProductionLifecycle.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista {
StaCudaSessionState::StaCudaSessionState(Database& database, const csr::CsrReferenceInput& input)
    : device(csr::CudaDualCsrSession::fromNativeExport(input, {input.nodes.size(), database.get_net_map().size(), input.arcs.size()})),
      _token(input.source_identity.session_token), _analysis_order(input.analysis_order),
      _path_source_order(input.path_source_order), _transition_order(input.transition_order),
      _clock_tags(input.clock_tags)
{
  device.requireCommitted();
  _baseline_names.reserve(input.nodes.size());
  for (const auto& node : input.nodes)
    _baseline_names.push_back(node.key.full_name);
  _baseline_ranks.resize(input.arcs.size());
  for (const auto& arc : input.arcs)
    _baseline_ranks.at(arc.id.value()) = arc.native_traversal_rank;
}

void StaCudaSessionState::bindEdits(Database& database, const csr::CsrReferenceInput& input,
                                   const timing_edit::BaselineSemanticIdentity& baseline, csr::detail::NativeLibraryModels& models)
{
  device.requireCommitted();
  if (native) {
    if (!native->baseline || *native->baseline != baseline || native->generation != device.generation())
      throw std::invalid_argument("CUDA native edit binding differs from this session");
    return;
  }
  if (device.generation().value() != 0 || input.source_identity.session_token != _token)
    throw std::invalid_argument("CUDA edits require this session's generation-zero baseline");
  native.emplace(timing_edit::NativeTargetedEditPreparer::bindBaseline(database, input, baseline, models));
  std::vector<std::string>{}.swap(_baseline_names);
  std::vector<std::optional<std::size_t>>{}.swap(_baseline_ranks);
}

void StaCudaSessionState::committed() noexcept
{
  _timing_generation.reset();
  _report_generation.reset();
}

void StaCudaSessionState::committedFull(std::vector<std::string>&& names, std::vector<std::optional<std::size_t>>&& ranks) noexcept
{
  native.reset();
  _baseline_names.swap(names);
  _baseline_ranks.swap(ranks);
  committed();
}

void StaCudaSessionState::materialize(Database& database)
{
  // Check even a cache hit: pending edits and poisoned sessions cannot serve old results.
  if (_poisoned)
    throw std::logic_error("CUDA result session is poisoned");
  device.requireCommitted();
  const auto generation = device.generation();
  if (native && native->generation != generation)
    throw std::logic_error("CUDA native/device generation mismatch");
  if (_timing_generation == generation)
    return;
  const auto& values = device.timingValues();
  const auto& names = native ? native->node_names : _baseline_names;
  const auto& ranks = native ? native->arc_native_ranks : _baseline_ranks;
  auto prepared = csr::detail::prepareStableTimingResult(
      database, names, ranks, {_analysis_order, _path_source_order, _transition_order, _clock_tags}, values);
  auto* result = std::get_if<csr::detail::PreparedTimingResult>(&prepared);
  if (!result)
    throw std::runtime_error("CUDA current timing cannot be mapped to native state");
  csr::detail::publishPreparedTimingResult(*result);
  _timing_generation = generation;
}

void StaCudaSessionState::prepareReport(Database& database)
{
  materialize(database);
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
}  // namespace ista
