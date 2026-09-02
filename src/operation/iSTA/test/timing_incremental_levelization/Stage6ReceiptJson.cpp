// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "Stage6ReceiptJson.hpp"

#include <stdexcept>

#include "DualCsrFunctionalReceiptJson.hpp"
#include "DualCsrReceiptJsonCommon.hpp"
#include "Stage6FunctionalReceipt.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using Json = nlohmann::json;

Json levelizationToJson(const Stage6SeparatedReceipt& separated)
{
  const auto& graph = separated.functional.graph;
  const auto& levels = separated.levelization;
  if (levels.node_to_level.size() != graph.nodes.size()) {
    throw std::invalid_argument(
        "Stage 6 inverse level map does not cover graph nodes");
  }

  Json inverse = Json::array();
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    inverse.push_back(
        {{"node", json_detail::nodeKey(graph.nodes[index].key)},
         {"level", levels.node_to_level[index]}});
  }
  return {
      {"schema", "ista-stage6-levelization-receipt/1.0.0"},
      {"generation", levels.generation},
      {"complete", levels.complete},
      {"level_offsets", json_detail::scalarArray(levels.level_offsets)},
      {"level_nodes",
       json_detail::arrayOf(levels.level_nodes, json_detail::nodeKey)},
      {"node_to_level", std::move(inverse)},
      {"timing_order",
       json_detail::arrayOf(levels.timing_order, json_detail::nodeKey)}};
}

Json functionalToJson(const FunctionalSemanticReceipt& source,
                      const Stage6SeparatedReceipt& separated)
{
  Json full = functionalSemanticReceiptToJson(source);
  Json timing = std::move(full.at("timing"));
  timing.erase("level_offsets");
  timing.erase("level_nodes");
  timing.erase("node_to_level");
  timing.erase("timing_order");
  return {
      {"graph", detail::functionalGraphToJson(separated.functional.graph)},
      {"timing", std::move(timing)}};
}

}  // namespace

nlohmann::json stage6SeparatedReceiptToJson(
    const FunctionalSemanticReceipt& receipt,
    std::uint64_t generation)
{
  Stage6SeparatedReceipt separated
      = separateStage6FunctionalReceipt(receipt, generation);
  return {
      {"schema", "ista-stage6-separated-receipt/1.0.0"},
      {"functional", functionalToJson(receipt, separated)},
      {"levelization", levelizationToJson(separated)}};
}

}  // namespace ista::timing_dual_csr::test
