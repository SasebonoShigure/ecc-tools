// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <array>
#include <stdexcept>

#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferenceValidator.hpp"

namespace ista::csr::test {
namespace {

NodeRecord makePort(NodeId id, const char* name, PinDirection direction, const char* net, bool start, bool end)
{
  NodeRecord node;
  node.id = id;
  node.key = NodeKey{NodeKind::kPort, name, {}, {}};
  node.direction = direction;
  node.net_name = net;
  node.is_port = true;
  node.is_startpoint = start;
  node.is_endpoint = end;
  return node;
}

OptionalLaneMatrix zeroSlew()
{
  OptionalLaneMatrix result;
  for (auto& row : result)
    for (auto& value : row)
      value = 0.0;
  return result;
}

}  // namespace

StaAnalysisContext referenceInputTestContext()
{
  return {"slow", "25", "__no_rc_corner__"};
}

CsrReferenceInput makeReferenceInputTestFixture(std::uint64_t session_token)
{
  if (session_token == 0)
    throw std::invalid_argument("reference-input test fixture requires a nonzero session token");

  CsrReferenceInput input;
  input.analysis_context = referenceInputTestContext();
  input.nodes = {makePort(NodeId{0}, "in", PinDirection::kInput, "n", true, false), makePort(NodeId{1}, "out", PinDirection::kOutput, "n", false, true)};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}, {input.nodes[1].key, NodeId{1}}};

  ArcRecord arc;
  arc.id = ArcId{0};
  arc.key.base.source = input.nodes[0].key;
  arc.key.base.sink = input.nodes[1].key;
  arc.key.base.kind = ArcKind::kNet;
  arc.key.base.owner_name = "n";
  arc.source = NodeId{0};
  arc.sink = NodeId{1};
  arc.kind = ArcKind::kNet;
  arc.native_traversal_rank = 0;
  arc.eligibility = {true, true, true, false};
  arc.transfer.delay = 0.0;
  arc.transfer.delay_min = 0.0;
  arc.transfer.delay_max = 0.0;
  arc.output_load = ArcOutputLoadMatrix{};
  input.arcs.push_back(arc);
  input.arc_ids_by_key.push_back({arc.key, ArcId{0}});

  input.out_row_offsets = {0, 1, 1};
  input.out_arc_ids = {ArcId{0}};
  input.in_row_offsets = {0, 0, 1};
  input.in_arc_ids = {ArcId{0}};
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids = {NodeId{1}};
  input.clock_tags = {"clk"};
  input.clock_owner_by_node = {std::nullopt, std::nullopt};

  StartSeedModel seed;
  seed.seed_node = NodeId{0};
  seed.path_start_node = NodeId{0};
  seed.clock_tag = "clk";
  seed.slew = zeroSlew();
  input.start_seeds.push_back(seed);

  EndpointRequiredModel endpoint;
  endpoint.node = NodeId{1};
  endpoint.clock_name = "clk";
  input.endpoint_required.push_back(endpoint);

  const auto intrinsic = validateReferenceInputIntrinsic(input);
  if (!intrinsic.empty())
    throw std::logic_error("hand-built reference-input fixture is not intrinsically valid: " + intrinsic.front().field + ": " + intrinsic.front().message);
  input.source_identity = makeReferenceInputIdentity(input, session_token, 0);
  const auto errors = validateReferenceInputForSession(input, session_token);
  if (!errors.empty())
    throw std::logic_error("hand-built reference-input fixture is not strict-valid: " + errors.front().field + ": " + errors.front().message);
  return input;
}

}  // namespace ista::csr::test
