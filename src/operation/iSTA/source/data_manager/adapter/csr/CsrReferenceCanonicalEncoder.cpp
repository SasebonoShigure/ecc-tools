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
#include "CsrReferenceCanonicalEncoder.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "CsrReferenceCanonicalEncoderInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr {

void CanonicalEncoder::appendDomain(std::string_view domain, std::string_view version)
{
  appendString("ista-csr");
  appendString(domain);
  appendString(version);
}

void CanonicalEncoder::appendBool(bool value)
{
  appendU8(value ? 1 : 0);
}

void CanonicalEncoder::appendU8(std::uint8_t value)
{
  _bytes.push_back(value);
}

void CanonicalEncoder::appendU32(std::uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8)
    _bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void CanonicalEncoder::appendI32(std::int32_t value)
{
  appendU32(std::bit_cast<std::uint32_t>(value));
}

void CanonicalEncoder::appendU64(std::uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8)
    _bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void CanonicalEncoder::appendString(std::string_view value)
{
  appendU64(value.size());
  _bytes.insert(_bytes.end(), value.begin(), value.end());
}

void CanonicalEncoder::appendBytes(std::span<const std::uint8_t> value)
{
  appendU64(value.size());
  _bytes.insert(_bytes.end(), value.begin(), value.end());
}

void CanonicalEncoder::appendDouble(double value)
{
  if (std::isnan(value))
    throw std::invalid_argument("NaN is not canonical");
  if (value == std::numeric_limits<double>::infinity()) {
    appendU8(1);
    return;
  }
  if (value == -std::numeric_limits<double>::infinity()) {
    appendU8(2);
    return;
  }
  appendU8(0);
  if (value == 0.0)
    value = 0.0;
  appendU64(std::bit_cast<std::uint64_t>(value));
}

void CanonicalEncoder::appendOptionalDouble(const std::optional<double>& value)
{
  appendBool(value.has_value());
  if (value)
    appendDouble(*value);
}

namespace {

void appendNodeKey(CanonicalEncoder& encoder, const NodeKey& key)
{
  encoder.appendString(token(key.kind));
  encoder.appendString(key.full_name);
  encoder.appendString(key.instance_name);
  encoder.appendString(key.pin_name);
}

void appendTableSignature(CanonicalEncoder& encoder, const TimingTableSignature& signature)
{
  encoder.appendBool(signature.present);
  encoder.appendString(token(signature.variable1));
  encoder.appendString(token(signature.variable2));
}

void appendLibraryArcSignature(CanonicalEncoder& encoder, const LibraryArcSignature& signature)
{
  encoder.appendString(signature.library_name);
  encoder.appendI32(signature.arc_index);
  encoder.appendString(token(signature.sense));
  encoder.appendString(token(signature.trigger_transition));
  encoder.appendString(token(signature.check_transition));
  encoder.appendString(signature.sdf_condition);
  for (const auto& table : signature.delay_tables)
    appendTableSignature(encoder, table);
  for (const auto& table : signature.slew_tables)
    appendTableSignature(encoder, table);
}

void appendArcKey(CanonicalEncoder& encoder, const ArcKey& key)
{
  appendNodeKey(encoder, key.base.source);
  appendNodeKey(encoder, key.base.sink);
  encoder.appendString(token(key.base.kind));
  encoder.appendString(key.base.owner_name);
  encoder.appendString(key.base.library_source_port);
  encoder.appendString(key.base.library_sink_port);
  encoder.appendU64(key.base.library_arcs.size());
  for (const auto& signature : key.base.library_arcs)
    detail::appendLibraryArcSignatureFields(encoder, signature);
  encoder.appendU64(key.parallel_ordinal);
}

void appendIdentity(CanonicalEncoder& encoder, const CsrSnapshotIdentity& identity)
{
  encoder.appendString(identity.fingerprint_schema_version);
  encoder.appendU64(identity.session_token);
  encoder.appendU64(identity.graph_generation);
  encoder.appendString(identity.topology_fingerprint);
  encoder.appendString(identity.clock_constraint_fingerprint);
  encoder.appendString(identity.library_fingerprint);
  encoder.appendString(identity.corner_fingerprint);
  encoder.appendString(identity.parasitic_fingerprint);
  encoder.appendString(identity.timing_input_fingerprint);
}

void appendOptionalNodeId(CanonicalEncoder& encoder, const std::optional<NodeId>& node)
{
  encoder.appendBool(node.has_value());
  if (node)
    encoder.appendU64(node->value());
}

void appendTableModel(CanonicalEncoder& encoder, const TimingTableModel& table)
{
  encoder.appendBool(table.present);
  if (!table.present)
    return;
  encoder.appendString(token(table.variable1));
  encoder.appendString(token(table.variable2));
  encoder.appendU64(table.axes.size());
  for (const auto& axis : table.axes) {
    encoder.appendU64(axis.size());
    for (double value : axis)
      encoder.appendDouble(value);
  }
  encoder.appendU64(table.values.size());
  for (double value : table.values)
    encoder.appendDouble(value);
}

void appendChildArc(CanonicalEncoder& encoder, const ChildTimingArcModel& child)
{
  detail::appendLibraryArcSignatureFields(encoder, child.signature);
  encoder.appendDouble(child.time_unit_scale);
  encoder.appendDouble(child.capacitance_unit_scale);
  encoder.appendDouble(child.slew_derate);
  for (double value : child.slew_lower_threshold_pct)
    encoder.appendDouble(value);
  for (double value : child.slew_upper_threshold_pct)
    encoder.appendDouble(value);
  for (double value : child.input_threshold_pct)
    encoder.appendDouble(value);
  for (double value : child.output_threshold_pct)
    encoder.appendDouble(value);
  for (const auto& table : child.delay_tables)
    detail::appendTimingTableModelFields(encoder, table);
  for (const auto& table : child.slew_tables)
    detail::appendTimingTableModelFields(encoder, table);
  for (const auto& table : child.check_tables)
    detail::appendTimingTableModelFields(encoder, table);
}

void appendTransfer(CanonicalEncoder& encoder, const CellTransferModel& transfer)
{
  encoder.appendDouble(transfer.delay);
  encoder.appendDouble(transfer.delay_min);
  encoder.appendDouble(transfer.delay_max);
  encoder.appendBool(transfer.is_clock_arc);
  encoder.appendBool(transfer.is_disabled);
  encoder.appendBool(transfer.is_clear_or_preset);
  encoder.appendU64(transfer.children().size());
  for (const auto& child : transfer.children())
    detail::appendChildTimingArcModelFields(encoder, child);
}

void appendOptionalMatrix(CanonicalEncoder& encoder, const OptionalLaneMatrix& matrix)
{
  for (const auto& analysis : matrix)
    for (const auto& value : analysis)
      encoder.appendOptionalDouble(value);
}

void appendNodeIds(CanonicalEncoder& encoder, const std::vector<NodeId>& values)
{
  encoder.appendU64(values.size());
  for (NodeId value : values)
    encoder.appendU64(value.value());
}

void appendArcIds(CanonicalEncoder& encoder, const std::vector<ArcId>& values)
{
  encoder.appendU64(values.size());
  for (ArcId value : values)
    encoder.appendU64(value.value());
}

void appendOffsets(CanonicalEncoder& encoder, const std::vector<std::uint64_t>& values)
{
  encoder.appendU64(values.size());
  for (std::uint64_t value : values)
    encoder.appendU64(value);
}

void appendLibraryCell(CanonicalEncoder& encoder, const ReferencedLibraryCellModel& cell)
{
  encoder.appendString(cell.cell_name);
  encoder.appendBool(cell.present);
  if (!cell.present)
    return;
  encoder.appendString(cell.library_name);
  encoder.appendBool(cell.is_sequential);
  encoder.appendBool(cell.is_clock_gating);
  encoder.appendBool(cell.is_macro);
  encoder.appendString(cell.clock_pin_name);
  encoder.appendString(cell.data_pin_name);
  encoder.appendString(cell.output_pin_name);
  encoder.appendU64(cell.ports.size());
  for (const auto& port : cell.ports) {
    encoder.appendString(port.name);
    encoder.appendString(token(port.direction));
    encoder.appendBool(port.is_clock);
    encoder.appendBool(port.is_output);
    encoder.appendDouble(port.default_capacitance);
    detail::appendOptionalLaneMatrixFields(encoder, port.transition_capacitance);
  }
  encoder.appendU64(cell.cell_arcs.size());
  for (const auto& arc : cell.cell_arcs) {
    encoder.appendString(arc.source_port);
    encoder.appendString(arc.sink_port);
    detail::appendCellTransferModelFields(encoder, arc.transfer);
  }
  encoder.appendU64(cell.timing_checks.size());
  for (const auto& check : cell.timing_checks) {
    encoder.appendString(check.clock_port);
    encoder.appendString(check.data_port);
    encoder.appendString(token(check.kind));
    encoder.appendDouble(check.check_time);
    encoder.appendString(token(check.clock_transition));
    encoder.appendU64(check.children().size());
    for (const auto& child : check.children())
      detail::appendChildTimingArcModelFields(encoder, child);
  }
}

void appendParasitics(CanonicalEncoder& encoder, const ParasiticModel& parasitics)
{
  encoder.appendString(parasitics.capacitance_unit);
  encoder.appendString(parasitics.resistance_unit);
  encoder.appendU64(parasitics.nets.size());
  for (const auto& net : parasitics.nets) {
    encoder.appendString(net.map_key);
    encoder.appendString(net.embedded_name);
    encoder.appendDouble(net.lumped_capacitance);
    encoder.appendU64(net.nodes.size());
    for (const auto& node : net.nodes) {
      encoder.appendString(node.map_key);
      encoder.appendString(node.embedded_name);
      encoder.appendDouble(node.capacitance);
      encoder.appendDouble(node.x);
      encoder.appendDouble(node.y);
    }
    encoder.appendU64(net.resistors.size());
    for (const auto& resistor : net.resistors) {
      encoder.appendString(resistor.source);
      encoder.appendString(resistor.sink);
      encoder.appendDouble(resistor.resistance);
    }
  }
}

}  // namespace

namespace detail {

void appendLibraryArcSignatureFields(CanonicalEncoder& encoder, const LibraryArcSignature& signature)
{
  appendLibraryArcSignature(encoder, signature);
}

void appendOptionalNodeIdFields(CanonicalEncoder& encoder, const std::optional<NodeId>& node)
{
  appendOptionalNodeId(encoder, node);
}

void appendOptionalLaneMatrixFields(CanonicalEncoder& encoder, const OptionalLaneMatrix& matrix)
{
  appendOptionalMatrix(encoder, matrix);
}

void appendTimingTableModelFields(CanonicalEncoder& encoder, const TimingTableModel& table)
{
  appendTableModel(encoder, table);
}

void appendChildTimingArcModelFields(CanonicalEncoder& encoder, const ChildTimingArcModel& child)
{
  appendChildArc(encoder, child);
}

void appendCellTransferModelFields(CanonicalEncoder& encoder, const CellTransferModel& transfer)
{
  appendTransfer(encoder, transfer);
}

void appendReferencedLibraryCellModelFields(CanonicalEncoder& encoder, const ReferencedLibraryCellModel& cell)
{
  appendLibraryCell(encoder, cell);
}

void appendParasiticModelFields(CanonicalEncoder& encoder, const ParasiticModel& parasitics)
{
  appendParasitics(encoder, parasitics);
}

}  // namespace detail

std::vector<std::uint8_t> canonicalNodeKeyBytes(const NodeKey& key)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("node-key");
  appendNodeKey(encoder, key);
  return encoder.bytes();
}

std::vector<std::uint8_t> canonicalArcKeyBytes(const ArcKey& key)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("arc-key");
  appendArcKey(encoder, key);
  return encoder.bytes();
}

std::vector<std::uint8_t> canonicalLibraryArcSignatureBytes(const LibraryArcSignature& signature)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("library-arc-signature");
  detail::appendLibraryArcSignatureFields(encoder, signature);
  return encoder.bytes();
}

std::vector<std::uint8_t> canonicalAnalysisContextBytes(const StaAnalysisContext& context)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("analysis-context");
  encoder.appendString(context.liberty_corner);
  encoder.appendString(context.temperature);
  encoder.appendString(context.rc_corner);
  return encoder.bytes();
}

std::vector<std::uint8_t> canonicalSnapshotIdentityBytes(const CsrSnapshotIdentity& identity)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("snapshot-identity", identity.fingerprint_schema_version);
  appendIdentity(encoder, identity);
  return encoder.bytes();
}

std::vector<std::uint8_t> canonicalReferenceInputBytes(const CsrReferenceInput& input)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("reference-input", input.schema_version);
  encoder.appendString(input.analysis_context.liberty_corner);
  encoder.appendString(input.analysis_context.temperature);
  encoder.appendString(input.analysis_context.rc_corner);
  for (AnalysisMode analysis : input.analysis_order)
    encoder.appendString(token(analysis));
  for (PathSource source : input.path_source_order)
    encoder.appendString(token(source));
  for (Transition transition : input.transition_order)
    encoder.appendString(token(transition));

  encoder.appendU64(input.nodes.size());
  for (const auto& node : input.nodes) {
    encoder.appendU64(node.id.value());
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

  encoder.appendU64(input.node_ids_by_key.size());
  for (const auto& entry : input.node_ids_by_key) {
    appendNodeKey(encoder, entry.key);
    encoder.appendU64(entry.id.value());
  }

  encoder.appendU64(input.instances.size());
  for (const auto& instance : input.instances) {
    encoder.appendString(instance.instance_name);
    encoder.appendString(instance.cell_name);
    appendNodeIds(encoder, instance.ordered_pins);
    encoder.appendBool(instance.is_sequential);
    encoder.appendBool(instance.has_timing_checks);
    encoder.appendBool(instance.is_clock_gating);
    encoder.appendBool(instance.is_macro);
  }

  encoder.appendU64(input.arcs.size());
  for (const auto& arc : input.arcs) {
    encoder.appendU64(arc.id.value());
    appendArcKey(encoder, arc.key);
    encoder.appendU64(arc.source.value());
    encoder.appendU64(arc.sink.value());
    encoder.appendString(token(arc.kind));
    encoder.appendU64(arc.native_traversal_rank);
    encoder.appendBool(arc.is_clock_arc);
    encoder.appendBool(arc.is_disabled);
    encoder.appendBool(arc.is_loop_disabled);
    encoder.appendBool(arc.eligibility.levelization);
    encoder.appendBool(arc.eligibility.data_arrival);
    encoder.appendBool(arc.eligibility.data_slew);
    encoder.appendBool(arc.eligibility.clock);
    detail::appendCellTransferModelFields(encoder, arc.transfer);
    encoder.appendBool(arc.output_load.has_value());
    if (arc.output_load)
      for (const auto& analysis : *arc.output_load)
        for (double value : analysis)
          encoder.appendDouble(value);
  }

  encoder.appendU64(input.arc_ids_by_key.size());
  for (const auto& entry : input.arc_ids_by_key) {
    appendArcKey(encoder, entry.key);
    encoder.appendU64(entry.id.value());
  }
  appendOffsets(encoder, input.out_row_offsets);
  appendArcIds(encoder, input.out_arc_ids);
  appendOffsets(encoder, input.in_row_offsets);
  appendArcIds(encoder, input.in_arc_ids);
  appendNodeIds(encoder, input.ordered_root_ids);
  appendNodeIds(encoder, input.ordered_start_ids);
  appendNodeIds(encoder, input.ordered_end_ids);

  encoder.appendU64(input.clock_tags.size());
  for (const auto& tag : input.clock_tags)
    encoder.appendString(tag);
  encoder.appendU64(input.clocks.size());
  for (const auto& clock : input.clocks) {
    encoder.appendString(clock.map_key);
    encoder.appendString(clock.embedded_clock_name);
    encoder.appendU64(clock.ordered_sources.size());
    for (const auto& source : clock.ordered_sources) {
      encoder.appendString(source.full_name);
      detail::appendOptionalNodeIdFields(encoder, source.resolved_node);
    }
    encoder.appendDouble(clock.period);
    for (double edge : clock.edges)
      encoder.appendDouble(edge);
    encoder.appendBool(clock.propagated);
  }
  encoder.appendU64(input.clock_owner_by_node.size());
  for (const auto& owner : input.clock_owner_by_node) {
    encoder.appendBool(owner.has_value());
    if (owner)
      encoder.appendString(*owner);
  }
  encoder.appendU64(input.port_constraints.size());
  for (const auto& constraint : input.port_constraints) {
    encoder.appendString(constraint.map_key);
    encoder.appendString(constraint.embedded_port_name);
    detail::appendOptionalNodeIdFields(encoder, constraint.resolved_node);
    for (const auto& value : constraint.input_delay)
      encoder.appendOptionalDouble(value);
    encoder.appendOptionalDouble(constraint.input_transition);
    for (const auto& value : constraint.output_delay)
      encoder.appendOptionalDouble(value);
    encoder.appendOptionalDouble(constraint.output_load);
    encoder.appendString(constraint.clock_name);
  }
  encoder.appendU64(input.case_analysis_constraints.size());
  for (const auto& constraint : input.case_analysis_constraints) {
    encoder.appendString(constraint.raw_name);
    detail::appendOptionalNodeIdFields(encoder, constraint.resolved_node);
    encoder.appendBool(constraint.value);
  }
  encoder.appendU64(input.timing_check_endpoints.size());
  for (const auto& check : input.timing_check_endpoints) {
    encoder.appendString(check.instance_name);
    encoder.appendU64(check.referenced_check_ordinal);
    encoder.appendString(token(check.check_kind));
    encoder.appendString(check.clock_pin_raw_name);
    encoder.appendString(check.data_pin_raw_name);
    encoder.appendString(check.clock_pin_full_name);
    encoder.appendString(check.data_pin_full_name);
    detail::appendOptionalNodeIdFields(encoder, check.resolved_clock_node);
    detail::appendOptionalNodeIdFields(encoder, check.resolved_data_node);
  }
  encoder.appendU64(input.referenced_library_cells.size());
  for (const auto& cell : input.referenced_library_cells)
    detail::appendReferencedLibraryCellModelFields(encoder, cell);
  detail::appendParasiticModelFields(encoder, input.parasitics);

  encoder.appendU64(input.start_seeds.size());
  for (const auto& seed : input.start_seeds) {
    encoder.appendU64(seed.seed_node.value());
    encoder.appendU64(seed.path_start_node.value());
    encoder.appendString(token(seed.availability));
    encoder.appendBool(seed.path_source.has_value());
    if (seed.path_source)
      encoder.appendString(token(*seed.path_source));
    encoder.appendString(seed.clock_tag);
    detail::appendOptionalLaneMatrixFields(encoder, seed.arrival);
    detail::appendOptionalLaneMatrixFields(encoder, seed.slew);
    detail::appendOptionalNodeIdFields(encoder, seed.crpr_node);
    encoder.appendBool(seed.crpr_transition.has_value());
    if (seed.crpr_transition)
      encoder.appendString(token(*seed.crpr_transition));
  }
  encoder.appendU64(input.endpoint_required.size());
  for (const auto& endpoint : input.endpoint_required) {
    encoder.appendU64(endpoint.node.value());
    encoder.appendString(endpoint.clock_name);
    encoder.appendDouble(endpoint.clock_period);
    for (const auto& delay : endpoint.output_delay)
      encoder.appendOptionalDouble(delay);
  }
  encoder.appendU64(input.cell_pin_capacitances.size());
  for (const auto& capacitance : input.cell_pin_capacitances) {
    encoder.appendU64(capacitance.node.value());
    encoder.appendDouble(capacitance.default_capacitance);
    detail::appendOptionalLaneMatrixFields(encoder, capacitance.transition_capacitance);
  }
  encoder.appendU64(input.unsupported_reasons.size());
  for (auto reason : input.unsupported_reasons)
    encoder.appendString(token(reason));
  return encoder.bytes();
}

}  // namespace ista::csr
