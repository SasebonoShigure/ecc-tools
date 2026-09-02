// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualSemanticReadView.hpp"

#include <stdexcept>

namespace {

using ista::csr::ArcId;
using ista::csr::ArcKey;
using ista::csr::ArcKind;
using ista::csr::NodeId;
using ista::csr::NodeKey;
using ista::csr::NodeKind;
using ista::timing_dual_csr::DualCsrArcSlot;
using ista::timing_dual_csr::DualCsrState;
using ista::timing_dual_csr::DualSemanticReadError;
using ista::timing_dual_csr::DualSemanticReadView;
using ista::timing_edit::TimingEditSessionToken;
using ista::timing_edit::TransactionGeneration;

void require(bool condition)
{
  if (!condition)
    throw std::runtime_error("DualSemanticReadView test failure");
}

NodeKey nodeKey(const char* name)
{
  NodeKey result;
  result.kind = NodeKind::kPort;
  result.full_name = name;
  return result;
}

ArcKey arcKey(const NodeKey& source, const NodeKey& sink,
              const char* owner)
{
  ArcKey result;
  result.base.source = source;
  result.base.sink = sink;
  result.base.kind = ArcKind::kNet;
  result.base.owner_name = owner;
  return result;
}

DualCsrArcSlot arcSlot(const ArcKey& key, NodeId source, NodeId sink,
                       bool active)
{
  DualCsrArcSlot result;
  result.payload.key = key;
  result.payload.source = source;
  result.payload.sink = sink;
  result.payload.kind = ArcKind::kNet;
  result.active = active;
  return result;
}

DualCsrState makeState()
{
  DualCsrState state;
  state.session_token = TimingEditSessionToken{17};
  state.generation = TransactionGeneration{4};
  state.high_water = {.node_slots = 2, .net_slots = 3, .arc_slots = 2};
  const NodeKey source = nodeKey("source");
  const NodeKey sink = nodeKey("sink");
  ista::csr::NodeRecord source_record;
  source_record.id = NodeId{0};
  source_record.key = source;
  ista::csr::NodeRecord sink_record;
  sink_record.id = NodeId{1};
  sink_record.key = sink;
  state.nodes = {source_record, sink_record};
  state.arc_slots = {
      arcSlot(arcKey(source, sink, "active"), NodeId{0}, NodeId{1}, true),
      arcSlot(arcKey(source, sink, "inactive"), NodeId{0}, NodeId{1},
              false)};
  return state;
}

void checkQueries()
{
  const DualCsrState state = makeState();
  const DualSemanticReadView view{state};

  const auto node = view.nodeByKey(state.nodes[0].key);
  require(node.hasValue());
  require(&node.value() == &state.nodes[0]);
  require(view.nodeIdByKey(state.nodes[0].key) == NodeId{0});

  const NodeKey missing_node = nodeKey("missing");
  const auto absent_node = view.nodeByKey(missing_node);
  require(!absent_node.hasValue());
  require(absent_node.error() == DualSemanticReadError::kNodeNotFound);
  require(!view.nodeIdByKey(missing_node));

  const auto active = view.arcByKey(state.arc_slots[0].payload.key);
  require(active.hasValue());
  require(&active.value() == &state.arc_slots[0].payload);
  require(view.arcIdByKey(state.arc_slots[0].payload.key) == ArcId{0});

  const auto inactive = view.arcByKey(state.arc_slots[1].payload.key);
  require(!inactive.hasValue());
  require(inactive.error() == DualSemanticReadError::kArcNotFound);
  require(!view.arcIdByKey(state.arc_slots[1].payload.key));
  const auto inactive_by_id = view.arcById(ArcId{1});
  require(!inactive_by_id.hasValue());
  require(inactive_by_id.error() == DualSemanticReadError::kInactiveArc);

  const ArcKey missing_arc
      = arcKey(state.nodes[1].key, state.nodes[0].key, "missing");
  const auto absent_arc = view.arcByKey(missing_arc);
  require(!absent_arc.hasValue());
  require(absent_arc.error() == DualSemanticReadError::kArcNotFound);
  require(!view.arcIdByKey(missing_arc));
}

void checkInactiveKeyCollisionUsesActiveSlot()
{
  DualCsrState state = makeState();
  state.arc_slots[1].payload.key = state.arc_slots[0].payload.key;
  const DualSemanticReadView view{state};
  const auto active = view.arcByKey(state.arc_slots[0].payload.key);
  require(active.hasValue());
  require(&active.value() == &state.arc_slots[0].payload);
  require(view.arcIdByKey(state.arc_slots[0].payload.key) == ArcId{0});
  const auto inactive = view.arcById(ArcId{1});
  require(!inactive.hasValue());
  require(inactive.error() == DualSemanticReadError::kInactiveArc);
}

void checkMetadata()
{
  const DualCsrState state = makeState();
  const DualSemanticReadView view{state};
  require(&view.highWater() == &state.high_water);
  require(view.nodeHighWater() == 2);
  require(view.netHighWater() == 3);
  require(view.arcHighWater() == 2);
  require(view.sessionToken() == TimingEditSessionToken{17});
  require(view.generation() == TransactionGeneration{4});
}

void checkMalformedState()
{
  DualCsrState state = makeState();
  state.high_water.arc_slots = 3;
  const DualSemanticReadView view{state};
  const auto result = view.arcByKey(state.arc_slots[0].payload.key);
  require(!result.hasValue());
  require(result.error() == DualSemanticReadError::kMalformedState);
}

}  // namespace

int main()
{
  checkQueries();
  checkInactiveKeyCollisionUsesActiveSlot();
  checkMetadata();
  checkMalformedState();
  return 0;
}
