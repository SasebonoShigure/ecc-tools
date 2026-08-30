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
#include "CsrNativeGraphWireDecoder.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ista::csr::test {
namespace {

class Reader
{
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) : _bytes(bytes) {}

  std::uint8_t u8()
  {
    need(1);
    return _bytes[_position++];
  }

  std::uint32_t u32()
  {
    need(4);
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      value = static_cast<std::uint32_t>((value << 8) | _bytes[_position++]);
    }
    return value;
  }

  std::int32_t i32() { return std::bit_cast<std::int32_t>(u32()); }

  std::uint64_t u64()
  {
    need(8);
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
      value = (value << 8) | _bytes[_position++];
    }
    return value;
  }

  bool boolean()
  {
    const std::uint8_t value = u8();
    if (value > 1) {
      throw std::runtime_error("wire decoder: bool is not 0 or 1");
    }
    return value == 1;
  }

  std::string string()
  {
    const std::size_t length = checkedLength(u64());
    need(length);
    const char* begin = reinterpret_cast<const char*>(_bytes.data() + _position);
    _position += length;
    return std::string(begin, length);
  }

  std::vector<std::uint8_t> bytes()
  {
    const std::size_t length = checkedLength(u64());
    need(length);
    const auto begin = _bytes.begin() + static_cast<std::ptrdiff_t>(_position);
    _position += length;
    return std::vector<std::uint8_t>(begin, begin + static_cast<std::ptrdiff_t>(length));
  }

  std::size_t count()
  {
    const std::size_t value = checkedLength(u64());
    if (value > remaining()) {
      throw std::runtime_error("wire decoder: impossible vector count");
    }
    return value;
  }

  void domain(std::string_view expected_domain)
  {
    if (string() != "ista-csr" || string() != expected_domain || string() != "1.0.0") {
      throw std::runtime_error("wire decoder: domain/version mismatch");
    }
  }

  void finish() const
  {
    if (_position != _bytes.size()) {
      throw std::runtime_error("wire decoder: trailing bytes");
    }
  }

 private:
  std::size_t checkedLength(std::uint64_t value) const
  {
    if (value > std::numeric_limits<std::size_t>::max()) {
      throw std::runtime_error("wire decoder: length does not fit size_t");
    }
    return static_cast<std::size_t>(value);
  }

  std::size_t remaining() const { return _bytes.size() - _position; }

  void need(std::size_t size) const
  {
    if (size > remaining()) {
      throw std::runtime_error("wire decoder: truncated input");
    }
  }

  std::span<const std::uint8_t> _bytes;
  std::size_t _position{0};
};

NodeKind nodeKind(std::string_view token)
{
  if (token == "port")
    return NodeKind::kPort;
  if (token == "instance_pin")
    return NodeKind::kInstancePin;
  throw std::runtime_error("wire decoder: unknown node kind");
}

ArcKind arcKind(std::string_view token)
{
  if (token == "cell")
    return ArcKind::kCell;
  if (token == "net")
    return ArcKind::kNet;
  throw std::runtime_error("wire decoder: unknown arc kind");
}

PinDirection pinDirection(std::string_view token)
{
  if (token == "none")
    return PinDirection::kNone;
  if (token == "input")
    return PinDirection::kInput;
  if (token == "output")
    return PinDirection::kOutput;
  if (token == "inout")
    return PinDirection::kInout;
  throw std::runtime_error("wire decoder: unknown pin direction");
}

Transition transition(std::string_view token)
{
  if (token == "rise")
    return Transition::kRise;
  if (token == "fall")
    return Transition::kFall;
  if (token == "none")
    return Transition::kNone;
  throw std::runtime_error("wire decoder: unknown transition");
}

TimingSense timingSense(std::string_view token)
{
  if (token == "none_positive_fallback")
    return TimingSense::kNoneUsesPositiveFallback;
  if (token == "positive_unate")
    return TimingSense::kPositiveUnate;
  if (token == "negative_unate")
    return TimingSense::kNegativeUnate;
  if (token == "non_unate")
    return TimingSense::kNonUnate;
  throw std::runtime_error("wire decoder: unknown timing sense");
}

TableVariable tableVariable(std::string_view token)
{
  if (token == "none")
    return TableVariable::kNone;
  if (token == "input_transition")
    return TableVariable::kInputTransition;
  if (token == "output_capacitance")
    return TableVariable::kOutputCapacitance;
  if (token == "constrained_transition")
    return TableVariable::kConstrainedTransition;
  throw std::runtime_error("wire decoder: unknown table variable");
}

NodeKey nodeKeyPayload(Reader& reader)
{
  return NodeKey{nodeKind(reader.string()), reader.string(), reader.string(), reader.string()};
}

NodeKey nodeKey(Reader& reader)
{
  const std::vector<std::uint8_t> encoded = reader.bytes();
  Reader nested(encoded);
  nested.domain("node-key");
  NodeKey key = nodeKeyPayload(nested);
  nested.finish();
  return key;
}

TimingTableSignature tableSignature(Reader& reader)
{
  return TimingTableSignature{reader.boolean(), tableVariable(reader.string()), tableVariable(reader.string())};
}

LibraryArcSignature libraryArc(Reader& reader)
{
  LibraryArcSignature signature;
  signature.library_name = reader.string();
  signature.arc_index = reader.i32();
  signature.sense = timingSense(reader.string());
  signature.trigger_transition = transition(reader.string());
  signature.check_transition = transition(reader.string());
  signature.sdf_condition = reader.string();
  for (auto& table : signature.delay_tables) {
    table = tableSignature(reader);
  }
  for (auto& table : signature.slew_tables) {
    table = tableSignature(reader);
  }
  return signature;
}

ArcKey arcKey(Reader& reader)
{
  const std::vector<std::uint8_t> encoded = reader.bytes();
  Reader nested(encoded);
  nested.domain("arc-key");
  ArcKey key;
  key.base.source = nodeKeyPayload(nested);
  key.base.sink = nodeKeyPayload(nested);
  key.base.kind = arcKind(nested.string());
  key.base.owner_name = nested.string();
  key.base.library_source_port = nested.string();
  key.base.library_sink_port = nested.string();
  const std::size_t child_count = nested.count();
  for (std::size_t index = 0; index < child_count; ++index) {
    key.base.library_arcs.push_back(libraryArc(nested));
  }
  key.parallel_ordinal = nested.u64();
  nested.finish();
  return key;
}

std::vector<NodeKey> nodeKeys(Reader& reader)
{
  std::vector<NodeKey> result;
  const std::size_t count = reader.count();
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(nodeKey(reader));
  }
  return result;
}

NativeGraphNodeProjection node(Reader& reader)
{
  NativeGraphNodeProjection value;
  value.key = nodeKey(reader);
  value.direction = pinDirection(reader.string());
  value.instance_cell = reader.string();
  value.net_name = reader.string();
  value.is_port = reader.boolean();
  value.is_startpoint = reader.boolean();
  value.is_endpoint = reader.boolean();
  value.is_sequential = reader.boolean();
  value.is_clock_pin = reader.boolean();
  value.is_data_pin = reader.boolean();
  value.is_output_pin = reader.boolean();
  return value;
}

NativeGraphInstanceProjection instance(Reader& reader)
{
  NativeGraphInstanceProjection value;
  value.instance_name = reader.string();
  value.cell_name = reader.string();
  value.ordered_pins = nodeKeys(reader);
  value.is_sequential = reader.boolean();
  value.has_timing_checks = reader.boolean();
  value.is_clock_gating = reader.boolean();
  value.is_macro = reader.boolean();
  return value;
}

NativeGraphArcProjection arc(Reader& reader)
{
  NativeGraphArcProjection value;
  value.key = arcKey(reader);
  value.native_traversal_rank = reader.u64();
  value.is_clock_arc = reader.boolean();
  value.is_disabled = reader.boolean();
  value.is_loop_disabled = reader.boolean();
  return value;
}

std::vector<NativeGraphAdjacencyRow> adjacency(Reader& reader)
{
  std::vector<NativeGraphAdjacencyRow> rows;
  const std::size_t count = reader.count();
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    NativeGraphAdjacencyRow row;
    row.node = nodeKey(reader);
    const std::size_t arc_count = reader.count();
    for (std::size_t arc_index = 0; arc_index < arc_count; ++arc_index) {
      row.ordered_arcs.push_back(arcKey(reader));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

NativeGraphProjection decodeIndependentNativeGraphProjectionBytes(std::span<const std::uint8_t> bytes)
{
  Reader reader(bytes);
  reader.domain("native-graph-projection");
  NativeGraphProjection projection;
  projection.schema = reader.string();
  const std::size_t node_count = reader.count();
  for (std::size_t index = 0; index < node_count; ++index) {
    projection.nodes.push_back(node(reader));
  }
  const std::size_t instance_count = reader.count();
  for (std::size_t index = 0; index < instance_count; ++index) {
    projection.instances.push_back(instance(reader));
  }
  const std::size_t arc_count = reader.count();
  for (std::size_t index = 0; index < arc_count; ++index) {
    projection.arcs.push_back(arc(reader));
  }
  projection.base_out = adjacency(reader);
  projection.base_in = adjacency(reader);
  projection.ordered_roots = nodeKeys(reader);
  projection.ordered_starts = nodeKeys(reader);
  projection.ordered_ends = nodeKeys(reader);
  reader.finish();
  return projection;
}

CsrNativeGraphOracle decodeIndependentNativeGraphOracleBytes(std::span<const std::uint8_t> bytes)
{
  Reader reader(bytes);
  reader.domain("native-graph-oracle");
  CsrNativeGraphOracle oracle;
  oracle.schema = reader.string();
  oracle.native_timing_order = nodeKeys(reader);
  const std::size_t level_count = reader.count();
  for (std::size_t index = 0; index < level_count; ++index) {
    oracle.native_node_levels.push_back(CsrNativeNodeLevel{nodeKey(reader), reader.i32()});
  }
  reader.finish();
  return oracle;
}

}  // namespace ista::csr::test
