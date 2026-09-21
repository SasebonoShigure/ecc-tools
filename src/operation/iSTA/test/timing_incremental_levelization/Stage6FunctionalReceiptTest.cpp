// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "Stage6FunctionalReceipt.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace ista;
using namespace ista::timing_dual_csr;
using namespace ista::timing_dual_csr::test;

[[noreturn]] void fail(const char* message)
{
  throw std::runtime_error(message);
}

void require(bool condition, const char* message)
{
  if (!condition)
    fail(message);
}

csr::NodeKey key(std::string name)
{
  csr::NodeKey result;
  result.kind = csr::NodeKind::kPort;
  result.full_name = std::move(name);
  return result;
}

DualCsrProjectedNode node(const csr::NodeKey& value)
{
  DualCsrProjectedNode result;
  result.key = value;
  return result;
}

DualCsrProjectedArc edge(const csr::NodeKey& source,
                         const csr::NodeKey& sink)
{
  DualCsrProjectedArc result;
  result.source = source;
  result.sink = sink;
  result.eligibility.levelization = true;
  return result;
}

FunctionalSemanticReceipt makeReceipt(bool noncompact)
{
  const auto a = key("a");
  const auto b = key("b");
  const auto c = key("c");
  FunctionalSemanticReceipt receipt;
  receipt.graph.nodes = {node(a), node(b), node(c)};
  receipt.graph.active_arcs = {edge(a, c), edge(b, c)};
  if (noncompact) {
    receipt.timing.level_offsets = {0, 1, 2, 3};
    receipt.timing.level_nodes = {a, b, c};
    receipt.timing.node_to_level = {1, 2, 3};
  } else {
    receipt.timing.level_offsets = {0, 2, 3};
    receipt.timing.level_nodes = {a, b, c};
    receipt.timing.node_to_level = {1, 1, 2};
  }
  return receipt;
}

}  // namespace

int main()
{
  auto compact = separateStage6FunctionalReceipt(makeReceipt(false), 7);
  auto noncompact = separateStage6FunctionalReceipt(makeReceipt(true), 7);
  require(compact.functional == noncompact.functional,
          "legal alternative levelizations changed Stage 6 functional receipt");
  require(compact.levelization != noncompact.levelization,
          "level receipt did not retain the independent layout");
  require(validateStage6LevelizationReceipt(
              compact.functional.graph, compact.levelization, 7)
              .empty(),
          "compact receipt failed independent validation");
  require(validateStage6LevelizationReceipt(
              noncompact.functional.graph, noncompact.levelization, 7)
              .empty(),
          "legal noncompact receipt failed independent validation");

  auto missing = compact.levelization;
  missing.level_nodes.pop_back();
  require(!validateStage6LevelizationReceipt(
               compact.functional.graph, missing, 7)
               .empty(),
          "missing level node passed");

  auto inverse = compact.levelization;
  inverse.node_to_level[0] = 2;
  require(!validateStage6LevelizationReceipt(
               compact.functional.graph, inverse, 7)
               .empty(),
          "inconsistent inverse passed");

  auto order = compact.levelization;
  order.level_nodes[2] = order.level_nodes[1];
  require(!validateStage6LevelizationReceipt(
               compact.functional.graph, order, 7)
               .empty(),
          "duplicate level-order node passed");

  auto same_level_order = compact.levelization;
  same_level_order.level_nodes = {key("b"), key("a"), key("c")};
  require(validateStage6LevelizationReceipt(
              compact.functional.graph, same_level_order, 7)
              .empty(),
          "legal within-level level-order permutation failed");

  auto reverse_level_order = compact.levelization;
  reverse_level_order.level_nodes = {key("c"), key("a"), key("b")};
  require(!validateStage6LevelizationReceipt(
               compact.functional.graph, reverse_level_order, 7)
               .empty(),
          "cross-level reversed level-order permutation passed");

  auto edge = compact.levelization;
  edge.level_offsets = {0, 2, 3};
  edge.level_nodes = {key("a"), key("c"), key("b")};
  edge.node_to_level = {1, 2, 1};
  require(!validateStage6LevelizationReceipt(
               compact.functional.graph, edge, 7)
               .empty(),
          "eligible non-forward edge passed");

  auto stale = compact.levelization;
  stale.generation = 6;
  require(!validateStage6LevelizationReceipt(
               compact.functional.graph, stale, 7)
               .empty(),
          "stale generation binding passed");
  return 0;
}
