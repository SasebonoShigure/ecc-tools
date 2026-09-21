// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrResourceObservationJson.hpp"

#include <stdexcept>
#include <string_view>

namespace ista::timing_dual_csr::test {
namespace {

std::string_view computeToken(DualCsrComputeStrategy value)
{
  switch (value) {
    case DualCsrComputeStrategy::kCpu:
      return "cpu";
  }
  throw std::invalid_argument("unknown Dual-CSR compute strategy");
}

std::string_view updateToken(DualCsrObservedUpdateStrategy value)
{
  switch (value) {
    case DualCsrObservedUpdateStrategy::kIncremental:
      return "incremental";
    case DualCsrObservedUpdateStrategy::kTargetedDualCsr:
      return "targeted_dual_csr";
  }
  throw std::invalid_argument("unknown Dual-CSR update strategy");
}

std::string_view fallbackToken(DualCsrObservedFallbackStrategy value)
{
  switch (value) {
    case DualCsrObservedFallbackStrategy::kError:
      return "error";
    case DualCsrObservedFallbackStrategy::kNone:
      return "none";
  }
  throw std::invalid_argument("unknown Dual-CSR fallback strategy");
}

std::string_view propagationToken(
    DualCsrObservedPropagationStrategy value)
{
  switch (value) {
    case DualCsrObservedPropagationStrategy::kFullGraph:
      return "full_graph";
  }
  throw std::invalid_argument("unknown Dual-CSR propagation strategy");
}

std::string_view estimateToken(DualCsrMemoryEstimateKind value)
{
  switch (value) {
    case DualCsrMemoryEstimateKind::kShallowVectorStorage:
      return "shallow_vector_storage";
  }
  throw std::invalid_argument("unknown Dual-CSR memory estimate kind");
}

}  // namespace

std::string_view dualCsrBufferToken(DualCsrBufferKind value)
{
  switch (value) {
    case DualCsrBufferKind::kNodes:
      return "nodes";
    case DualCsrBufferKind::kArcSlots:
      return "arc_slots";
    case DualCsrBufferKind::kBaseOutOffsets:
      return "base_out_offsets";
    case DualCsrBufferKind::kBaseOutSlots:
      return "base_out_slots";
    case DualCsrBufferKind::kBaseInOffsets:
      return "base_in_offsets";
    case DualCsrBufferKind::kBaseInSlots:
      return "base_in_slots";
    case DualCsrBufferKind::kAuxRows:
      return "aux_rows";
    case DualCsrBufferKind::kAuxSources:
      return "aux_sources";
    case DualCsrBufferKind::kAuxArcs:
      return "aux_arcs";
    case DualCsrBufferKind::kAuxSourceMap:
      return "aux_source_map";
  }
  throw std::invalid_argument("unknown Dual-CSR buffer kind");
}

nlohmann::json resourceObservationToJson(
    const DualCsrResourceObservation& observation)
{
  nlohmann::json buffers = nlohmann::json::array();
  for (const auto& item : observation.vector_memory.buffers)
    buffers.push_back(
        {{"buffer", dualCsrBufferToken(item.buffer)},
         {"logical_elements", item.logical_elements},
         {"capacity_elements", item.capacity_elements},
         {"element_size_bytes", item.element_size_bytes},
         {"estimated_reserved_bytes", item.estimated_reserved_bytes}});
  return {
      {"generation", observation.generation.value()},
      {"tombstone_ratio", observation.tombstone_ratio},
      {"requested_strategy",
       {{"compute", computeToken(observation.requested_strategy.compute)},
        {"update", updateToken(observation.requested_strategy.update)},
        {"fallback",
         fallbackToken(observation.requested_strategy.fallback)}}},
      {"executed_strategy",
       {{"compute", computeToken(observation.executed_strategy.compute)},
        {"update", updateToken(observation.executed_strategy.update)},
        {"fallback",
         fallbackToken(observation.executed_strategy.fallback)},
        {"propagation",
         propagationToken(observation.executed_strategy.propagation)}}},
      {"vector_memory",
       {{"estimate_kind",
         estimateToken(observation.vector_memory.estimate_kind)},
        {"buffers", std::move(buffers)}}}};
}

nlohmann::json resourceObservationsToJson(
    const std::vector<DualCsrResourceObservation>& observations)
{
  nlohmann::json result = nlohmann::json::array();
  for (const auto& observation : observations)
    result.push_back(resourceObservationToJson(observation));
  return result;
}

}  // namespace ista::timing_dual_csr::test
