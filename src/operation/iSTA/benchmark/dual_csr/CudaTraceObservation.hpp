// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <map>
#include <stdexcept>
#include "CudaDualCsrDebugState.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaCudaSessionState.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr {
struct CudaTraceObservation
{
  CsrReferenceInput canonical_input;
  CsrTimingValues values;
  std::vector<std::string> node_names;
  std::vector<std::optional<std::size_t>> arc_native_ranks;
  CudaDualCsrDebugState device;
};

// Diagnostic encoding only; the independent fresh native computation stays in its loader.
inline CudaTraceObservation observeCudaTrace(StaCsrSessionContext& context, Database& database,
    const timing_edit::NativeExecutionAuthority& authority)
{
  if constexpr (!incregpusta_debug_checks_enabled)
    throw std::logic_error("CUDA trace observation requires debug checks");
  context.materializeCudaTiming(database, authority);
  auto& owner = context.cudaState(authority);
  auto exported = exportCsrReferenceInputCandidate(database, context.analysisContext(), context.token());
  auto* input = std::get_if<CsrReferenceInput>(&exported);
  if (!input || !owner.host) throw std::logic_error("CUDA diagnostic canonical view is unavailable");
  CudaTraceObservation result;
  result.canonical_input = std::move(*input);
  result.values = owner.device.timingValues();
  result.device = owner.device.debugState();
  const auto& graph = owner.host->graph;
  for (const auto& node : graph.nodes) result.node_names.push_back(node.key.full_name);
  std::map<ArcKey, std::size_t, decltype(&arcKeyLess)> ranks(arcKeyLess);
  for (const auto& arc : result.canonical_input.arcs)
    if (!ranks.emplace(arc.key, arc.native_traversal_rank).second)
      throw std::logic_error("CUDA diagnostic native arc key collision");
  result.arc_native_ranks.resize(graph.arc_slots.size());
  std::size_t active = 0;
  for (std::size_t id = 0; id < graph.arc_slots.size(); ++id) {
    const auto& slot = graph.arc_slots[id];
    if (!slot.active) continue;
    result.arc_native_ranks[id] = ranks.at(slot.payload.key);
    ++active;
  }
  if (active != ranks.size()) throw std::logic_error("CUDA diagnostic native arc count differs");
  return result;
}
}  // namespace ista::csr
