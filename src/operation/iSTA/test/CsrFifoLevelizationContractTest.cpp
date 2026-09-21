// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "CsrCoreCompiledContractTestSupport.hpp"
#include "CsrFifoLevelization.hpp"
#include "CsrValidatorTestFixture.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

constexpr std::array<CsrCoreCompiledContract, 3> kCompiledContracts = {
    CsrCoreCompiledContract{"fifo_acyclic_cycle_code_rejected", "fifo.chain.invariant", "replace_value", "computation.outcome", "cycle", "-", "invalid",
                            "computation.outcome", "-", "cycle code must exactly match FIFO levelization completeness"},
    CsrCoreCompiledContract{"fifo_cycle_code_accepted", "fifo.cycle.invariant", "replace_value", "computation.outcome", "cycle", "-", "valid", "-", "-", "-"},
    CsrCoreCompiledContract{"fifo_cycle_wrong_code_rejected", "fifo.cycle.cycle", "replace_value", "computation.outcome", "invariant_violation", "-", "invalid",
                            "computation.outcome", "-", "cycle code must exactly match FIFO levelization completeness"},
};

CsrReferenceInput emptyInput()
{
  CsrReferenceInput input;
  input.source_identity = identity();
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.out_row_offsets = {0};
  input.in_row_offsets = {0};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput singleInput()
{
  CsrReferenceInput input = emptyInput();
  NodeRecord node;
  node.id = NodeId{0};
  node.key = port("only");
  node.direction = PinDirection::kInput;
  node.is_port = true;
  node.is_startpoint = true;
  node.is_endpoint = true;
  input.nodes = {node};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}};
  input.out_row_offsets = {0, 0};
  input.in_row_offsets = {0, 0};
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids = {NodeId{0}};
  StartSeedModel seed;
  seed.seed_node = NodeId{0};
  seed.path_start_node = NodeId{0};
  seed.clock_tag = "clk";
  for (auto& analysis : seed.slew)
    for (auto& transition : analysis)
      transition = 0.0;
  input.start_seeds = {seed};
  input.clock_tags = {"clk"};
  input.clock_owner_by_node = {std::nullopt};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput multiRootInput()
{
  CsrReferenceInput input = referenceInput();
  NodeRecord node;
  node.id = NodeId{2};
  node.key = port("root");
  node.direction = PinDirection::kInput;
  node.is_port = true;
  node.is_startpoint = true;
  node.is_endpoint = true;
  input.nodes.push_back(node);
  input.node_ids_by_key.push_back({input.nodes[2].key, NodeId{2}});
  input.out_row_offsets = {0, 1, 1, 1};
  input.in_row_offsets = {0, 0, 1, 1};
  input.ordered_root_ids = {NodeId{0}, NodeId{2}};
  input.ordered_start_ids = {NodeId{0}, NodeId{2}};
  input.ordered_end_ids = {NodeId{1}, NodeId{2}};
  input.clock_owner_by_node.push_back(std::nullopt);
  StartSeedModel seed;
  seed.seed_node = NodeId{2};
  seed.path_start_node = NodeId{2};
  seed.clock_tag = "clk";
  for (auto& analysis : seed.slew)
    for (auto& transition : analysis)
      transition = 0.0;
  input.start_seeds.push_back(seed);
  refreshIdentity(input);
  return input;
}

CsrReferenceInput parallelInput()
{
  CsrReferenceInput input = cellInput();
  ArcRecord parallel = input.arcs[0];
  parallel.id = ArcId{1};
  parallel.key.parallel_ordinal = 1;
  parallel.native_traversal_rank = 1;
  input.arcs.push_back(parallel);
  input.arc_ids_by_key.push_back({parallel.key, parallel.id});
  input.out_row_offsets = {0, 2, 2};
  input.out_arc_ids = {ArcId{0}, ArcId{1}};
  input.in_row_offsets = {0, 0, 2};
  input.in_arc_ids = {ArcId{0}, ArcId{1}};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput maskedInput(bool loop_disabled)
{
  CsrReferenceInput input = cellInput();
  input.arcs[0].is_disabled = !loop_disabled;
  input.arcs[0].transfer.is_disabled = !loop_disabled;
  input.arcs[0].is_loop_disabled = loop_disabled;
  input.arcs[0].eligibility = {};
  input.ordered_root_ids = {NodeId{0}, NodeId{1}};
  input.referenced_library_cells[0].cell_arcs[0].transfer = input.arcs[0].transfer;
  refreshIdentity(input);
  return input;
}

CsrFifoLevelization slowFifoOracle(const CsrReferenceInput& input)
{
  CsrFifoLevelization replay;
  replay.node_to_level.assign(input.nodes.size(), 0);
  std::vector<std::uint64_t> indegree(input.nodes.size(), 0);
  for (const auto& arc : input.arcs)
    if (arc.eligibility.levelization)
      ++indegree[arc.sink.value()];

  std::vector<NodeId> ready = input.ordered_root_ids;
  for (NodeId root : ready)
    replay.node_to_level[root.value()] = 1;
  for (std::size_t head = 0; head < ready.size(); ++head) {
    const NodeId node = ready[head];
    replay.level_nodes.push_back(node);
    std::vector<const ArcRecord*> outgoing;
    for (const auto id : input.out_arc_ids) {
      const auto& arc = input.arcs[id.value()];
      if (arc.source == node && arc.eligibility.levelization)
        outgoing.push_back(&arc);
    }
    for (const ArcRecord* arc : outgoing) {
      const std::uint64_t candidate = replay.node_to_level[node.value()] + 1;
      replay.node_to_level[arc->sink.value()] = std::max(replay.node_to_level[arc->sink.value()], candidate);
      if (--indegree[arc->sink.value()] == 0)
        ready.push_back(arc->sink);
    }
  }

  replay.level_offsets = {0};
  const std::uint64_t maximum_level = replay.node_to_level.empty() ? 0 : *std::ranges::max_element(replay.node_to_level);
  std::uint64_t cumulative = 0;
  for (std::uint64_t level = 1; level <= maximum_level; ++level) {
    cumulative += std::ranges::count_if(replay.level_nodes, [&](NodeId node) { return replay.node_to_level[node.value()] == level; });
    replay.level_offsets.push_back(cumulative);
  }
  replay.complete = replay.level_nodes.size() == input.nodes.size();
  return replay;
}

void requireReplay(const CsrReferenceInput& input, const CsrFifoLevelization& expected, const std::string& name)
{
  requireValid(validateReferenceInput(input), name + " input");
  const CsrFifoLevelization actual = replayFifoLevelization(input);
  require(actual == expected, name + " literal FIFO replay mismatch");
  require(actual == slowFifoOracle(input), name + " independent slow FIFO oracle mismatch");
}

void testExactReplays()
{
  requireReplay(emptyInput(), CsrFifoLevelization{true, {0}, {}, {}}, "empty");
  requireReplay(singleInput(), CsrFifoLevelization{true, {0, 1}, {NodeId{0}}, {1}}, "single");
  requireReplay(referenceInput(), CsrFifoLevelization{true, {0, 1, 2}, {NodeId{0}, NodeId{1}}, {1, 2}}, "chain");
  requireReplay(multiRootInput(), CsrFifoLevelization{true, {0, 2, 3}, {NodeId{0}, NodeId{2}, NodeId{1}}, {1, 2, 1}}, "multi-root");
  requireReplay(parallelInput(), CsrFifoLevelization{true, {0, 1, 2}, {NodeId{0}, NodeId{1}}, {1, 2}}, "parallel");
  requireReplay(maskedInput(false), CsrFifoLevelization{true, {0, 2}, {NodeId{0}, NodeId{1}}, {1, 1}}, "disabled");
  requireReplay(maskedInput(true), CsrFifoLevelization{true, {0, 2}, {NodeId{0}, NodeId{1}}, {1, 1}}, "loop-disabled");
  requireReplay(cycleInput(), CsrFifoLevelization{false, {0}, {}, {0, 0}}, "active cycle");
}

CsrComputationCode computationCode(std::string_view token_value)
{
  if (token_value == "cycle")
    return CsrComputationCode::kCycle;
  if (token_value == "invariant_violation")
    return CsrComputationCode::kInvariantViolation;
  throw std::runtime_error("compiled FIFO computation token is not implemented");
}

std::vector<CsrContractViolation> evaluateFifoCase(const CsrCoreCompiledContract& contract)
{
  CsrReferenceInput input;
  CsrComputationResult result;
  if (contract.baseline == "fifo.chain.invariant") {
    input = referenceInput();
    result.outcome = CsrComputationCode::kInvariantViolation;
  } else if (contract.baseline == "fifo.cycle.invariant") {
    input = cycleInput();
    result.outcome = CsrComputationCode::kInvariantViolation;
  } else if (contract.baseline == "fifo.cycle.cycle") {
    input = cycleInput();
    result.outcome = CsrComputationCode::kCycle;
  } else {
    throw std::runtime_error("compiled FIFO baseline is not implemented");
  }
  requireValid(validateReferenceInput(input), std::string(contract.case_id) + " baseline input");
  require(contract.recipe_op == "replace_value" && contract.target == "computation.outcome", "FIFO recipe must replace computation.outcome");
  result.outcome = computationCode(contract.operand);
  return validateComputationResult(input, result);
}

void testCompiledContracts()
{
  for (const auto& contract : kCompiledContracts)
    requireCoreCompiledOutcome(contract, evaluateFifoCase(contract));
}

void testProductionConsumers()
{
  const CsrReferenceInput chain = referenceInput();
  requireValid(validateTimingResult(chain, timingResult(chain)), "timing-result FIFO consumer");
  const CsrReferenceInput cycle = cycleInput();
  requireValid(validateReferenceInput(cycle), "reference validator must accept a valid active cycle");
  require(!replayFifoLevelization(cycle).complete, "active cycle replay must be incomplete");
}

}  // namespace

int main(int argc, char* argv[])
{
  if (handleCoreCompiledContractCli(argc, argv, kCompiledContracts))
    return 0;
  testExactReplays();
  testCompiledContracts();
  testProductionConsumers();
  return 0;
}
