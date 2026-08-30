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
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::test {
namespace {

static_assert(std::numeric_limits<double>::is_iec559);
static_assert(sizeof(double) == sizeof(std::uint64_t));

void appendSize(CanonicalEncoder& encoder, std::size_t size)
{
  encoder.appendU64(static_cast<std::uint64_t>(size));
}

void appendNodeKey(CanonicalEncoder& encoder, const NodeKey& key)
{
  encoder.appendBytes(canonicalNodeKeyBytes(key));
}

void appendArcKey(CanonicalEncoder& encoder, const ArcKey& key)
{
  encoder.appendBytes(canonicalArcKeyBytes(key));
}

template <typename T, typename Append>
void appendOptional(CanonicalEncoder& encoder, const std::optional<T>& value, Append append)
{
  encoder.appendBool(value.has_value());
  if (value) {
    append(encoder, *value);
  }
}

template <typename T, typename Append>
void appendLaneMatrix(CanonicalEncoder& encoder, const CsrNativeTimingLaneMatrix<T>& matrix, Append append)
{
  for (const auto& analysis : matrix) {
    for (const auto& value : analysis) {
      appendOptional(encoder, value, append);
    }
  }
}

template <typename T, typename Append>
void appendArcLaneMatrix(CanonicalEncoder& encoder, const CsrNativeTimingArcLaneMatrix<T>& matrix, Append append)
{
  for (const auto& analysis : matrix) {
    for (const auto& input : analysis) {
      for (const auto& value : input) {
        appendOptional(encoder, value, append);
      }
    }
  }
}

void appendDouble(CanonicalEncoder& encoder, double value)
{
  encoder.appendU64(std::bit_cast<std::uint64_t>(value));
}

void appendTransition(CanonicalEncoder& encoder, Transition value)
{
  encoder.appendString(token(value));
}

void appendClockState(CanonicalEncoder& encoder, const CsrNativeClockTimingState& state)
{
  appendLaneMatrix(encoder, state.arrivals, appendDouble);
  appendLaneMatrix(encoder, state.slews, appendDouble);
  appendLaneMatrix(encoder, state.predecessor_nodes, appendNodeKey);
  appendLaneMatrix(encoder, state.predecessor_arc_delays, appendDouble);
  appendLaneMatrix(encoder, state.predecessor_transitions, appendTransition);
}

void appendPath(CanonicalEncoder& encoder, const CsrNativePathTimingState& path)
{
  encoder.appendString(token(path.analysis));
  encoder.appendString(token(path.source));
  encoder.appendString(token(path.lane_transition));
  encoder.appendString(path.clock_tag);
  appendDouble(encoder, path.arrival);
  appendDouble(encoder, path.slew);
  appendDouble(encoder, path.launch);
  appendNodeKey(encoder, path.start_node);
  appendOptional(encoder, path.predecessor_node, appendNodeKey);
  appendOptional(encoder, path.predecessor_arc, appendArcKey);
  appendDouble(encoder, path.predecessor_arc_delay);
  encoder.appendString(path.clock_name);
  encoder.appendString(token(path.state_transition));
  encoder.appendString(token(path.predecessor_transition));
  appendOptional(encoder, path.crpr_node, appendNodeKey);
  encoder.appendString(token(path.crpr_transition));
}

void appendNode(CanonicalEncoder& encoder, const CsrNativeTimingNode& node)
{
  appendNodeKey(encoder, node.key);
  encoder.appendI32(node.level);
  encoder.appendBool(node.is_clock_point);
  appendDouble(encoder, node.arrival);
  appendDouble(encoder, node.required);
  appendDouble(encoder, node.slack);
  appendDouble(encoder, node.launch);
  appendOptional(encoder, node.clock_name, [](CanonicalEncoder& output, const std::string& value) { output.appendString(value); });
  appendOptional(encoder, node.predecessor_node, appendNodeKey);
  appendOptional(encoder, node.predecessor_arc, appendArcKey);
  appendClockState(encoder, node.effective_clock);
  appendClockState(encoder, node.physical_clock);
  appendLaneMatrix(encoder, node.data_slews, appendDouble);
  appendSize(encoder, node.paths.size());
  for (const CsrNativePathTimingState& path : node.paths) {
    appendPath(encoder, path);
  }
}

void appendTimingArcDelay(CanonicalEncoder& encoder, const CsrNativeTimingArcDelay& value)
{
  appendDouble(encoder, value.delay);
  encoder.appendBool(value.is_fallback);
}

void appendArc(CanonicalEncoder& encoder, const CsrNativeTimingArc& arc)
{
  appendArcKey(encoder, arc.key);
  appendDouble(encoder, arc.delay);
  appendDouble(encoder, arc.min_delay);
  appendDouble(encoder, arc.max_delay);
  appendLaneMatrix(encoder, arc.transition_delays, appendDouble);
  appendArcLaneMatrix(encoder, arc.input_output_delays, appendDouble);
  appendArcLaneMatrix(encoder, arc.graph_delays, appendDouble);
  for (const std::optional<Transition>& transition : arc.transition_types) {
    appendOptional(encoder, transition, appendTransition);
  }
  appendSize(encoder, arc.timing_arcs.size());
  for (const CsrNativeTimingArcEntry& timing_arc : arc.timing_arcs) {
    encoder.appendI32(timing_arc.timing_arc_index);
    appendArcLaneMatrix(encoder, timing_arc.lanes, appendTimingArcDelay);
  }
}

}  // namespace

std::vector<std::uint8_t> canonicalIndependentNativeTimingOracleBytes(const CsrNativeTimingOracle& oracle)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("native-timing-oracle", "1.0.0");
  encoder.appendString(oracle.schema);
  appendSize(encoder, oracle.native_timing_order.size());
  for (const NodeKey& key : oracle.native_timing_order) {
    appendNodeKey(encoder, key);
  }
  appendSize(encoder, oracle.nodes.size());
  for (const CsrNativeTimingNode& node : oracle.nodes) {
    appendNode(encoder, node);
  }
  appendSize(encoder, oracle.arcs.size());
  for (const CsrNativeTimingArc& arc : oracle.arcs) {
    appendArc(encoder, arc);
  }
  return encoder.bytes();
}

}  // namespace ista::csr::test
