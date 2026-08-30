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
#include <cstddef>
#include <cstdint>
#include <vector>

#include "CsrFingerprint.hpp"
#include "CsrNativeGraphTestSupport.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::test {
namespace {

void appendSize(CanonicalEncoder& encoder, std::size_t size)
{
  encoder.appendU64(static_cast<std::uint64_t>(size));
}

void appendNodeKey(CanonicalEncoder& encoder, const NodeKey& key)
{
  const auto bytes = canonicalNodeKeyBytes(key);
  encoder.appendBytes(bytes);
}

void appendArcKey(CanonicalEncoder& encoder, const ArcKey& key)
{
  const auto bytes = canonicalArcKeyBytes(key);
  encoder.appendBytes(bytes);
}

void appendNodeKeys(CanonicalEncoder& encoder, const std::vector<NodeKey>& keys)
{
  appendSize(encoder, keys.size());
  for (const NodeKey& key : keys) {
    appendNodeKey(encoder, key);
  }
}

void appendNode(CanonicalEncoder& encoder, const NativeGraphNodeProjection& node)
{
  appendNodeKey(encoder, node.key);
  encoder.appendString(token(node.direction));
  encoder.appendString(node.instance_cell);
  encoder.appendString(node.net_name);
  encoder.appendBool(node.is_port);
  encoder.appendBool(node.is_startpoint);
  encoder.appendBool(node.is_endpoint);
  encoder.appendBool(node.is_sequential);
  encoder.appendBool(node.is_clock_pin);
  encoder.appendBool(node.is_data_pin);
  encoder.appendBool(node.is_output_pin);
}

void appendInstance(CanonicalEncoder& encoder, const NativeGraphInstanceProjection& instance)
{
  encoder.appendString(instance.instance_name);
  encoder.appendString(instance.cell_name);
  appendNodeKeys(encoder, instance.ordered_pins);
  encoder.appendBool(instance.is_sequential);
  encoder.appendBool(instance.has_timing_checks);
  encoder.appendBool(instance.is_clock_gating);
  encoder.appendBool(instance.is_macro);
}

void appendArc(CanonicalEncoder& encoder, const NativeGraphArcProjection& arc)
{
  appendArcKey(encoder, arc.key);
  encoder.appendU64(arc.native_traversal_rank);
  encoder.appendBool(arc.is_clock_arc);
  encoder.appendBool(arc.is_disabled);
  encoder.appendBool(arc.is_loop_disabled);
}

void appendRows(CanonicalEncoder& encoder, const std::vector<NativeGraphAdjacencyRow>& rows)
{
  appendSize(encoder, rows.size());
  for (const NativeGraphAdjacencyRow& row : rows) {
    appendNodeKey(encoder, row.node);
    appendSize(encoder, row.ordered_arcs.size());
    for (const ArcKey& key : row.ordered_arcs) {
      appendArcKey(encoder, key);
    }
  }
}

}  // namespace

std::vector<std::uint8_t> canonicalIndependentNativeGraphProjectionBytes(const NativeGraphProjection& projection)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("native-graph-projection", "1.0.0");
  encoder.appendString(projection.schema);
  appendSize(encoder, projection.nodes.size());
  for (const NativeGraphNodeProjection& node : projection.nodes) {
    appendNode(encoder, node);
  }
  appendSize(encoder, projection.instances.size());
  for (const NativeGraphInstanceProjection& instance : projection.instances) {
    appendInstance(encoder, instance);
  }
  appendSize(encoder, projection.arcs.size());
  for (const NativeGraphArcProjection& arc : projection.arcs) {
    appendArc(encoder, arc);
  }
  appendRows(encoder, projection.base_out);
  appendRows(encoder, projection.base_in);
  appendNodeKeys(encoder, projection.ordered_roots);
  appendNodeKeys(encoder, projection.ordered_starts);
  appendNodeKeys(encoder, projection.ordered_ends);
  return encoder.bytes();
}

std::vector<std::uint8_t> canonicalIndependentNativeGraphOracleBytes(const CsrNativeGraphOracle& oracle)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("native-graph-oracle", "1.0.0");
  encoder.appendString(oracle.schema);
  appendNodeKeys(encoder, oracle.native_timing_order);
  appendSize(encoder, oracle.native_node_levels.size());
  for (const CsrNativeNodeLevel& level : oracle.native_node_levels) {
    appendNodeKey(encoder, level.node_key);
    encoder.appendI32(level.native_level);
  }
  return encoder.bytes();
}

}  // namespace ista::csr::test
