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
#include "CsrReferenceFingerprintPayload.hpp"

#include "CsrReferenceCanonicalEncoderInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr {
namespace {

std::vector<std::uint8_t> topologyPayload(const CsrReferenceInput& input)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("reference-topology-payload", input.schema_version);
  encoder.appendBool(input.nodes.empty() && input.instances.empty() && input.arcs.empty());
  encoder.appendU64(input.nodes.size());
  for (const auto& node : input.nodes) {
    encoder.appendU64(node.id.value());
    encoder.appendBytes(canonicalNodeKeyBytes(node.key));
    encoder.appendString(token(node.direction));
    encoder.appendString(node.instance_cell);
    encoder.appendString(node.net_name);
    encoder.appendBool(node.is_port);
  }
  encoder.appendU64(input.node_ids_by_key.size());
  for (const auto& entry : input.node_ids_by_key) {
    encoder.appendBytes(canonicalNodeKeyBytes(entry.key));
    encoder.appendU64(entry.id.value());
  }
  encoder.appendU64(input.instances.size());
  for (const auto& instance : input.instances) {
    encoder.appendString(instance.instance_name);
    encoder.appendString(instance.cell_name);
    encoder.appendU64(instance.ordered_pins.size());
    for (NodeId pin : instance.ordered_pins)
      encoder.appendU64(pin.value());
  }
  encoder.appendU64(input.arcs.size());
  for (const auto& arc : input.arcs) {
    encoder.appendU64(arc.id.value());
    encoder.appendBytes(canonicalArcKeyBytes(arc.key));
    encoder.appendU64(arc.source.value());
    encoder.appendU64(arc.sink.value());
    encoder.appendString(token(arc.kind));
    encoder.appendU64(arc.native_traversal_rank);
    encoder.appendBool(arc.is_clock_arc);
    encoder.appendBool(arc.is_disabled);
    encoder.appendBool(arc.is_loop_disabled);
  }
  encoder.appendU64(input.arc_ids_by_key.size());
  for (const auto& entry : input.arc_ids_by_key) {
    encoder.appendBytes(canonicalArcKeyBytes(entry.key));
    encoder.appendU64(entry.id.value());
  }
  const auto append_u64s = [&](const auto& values) {
    encoder.appendU64(values.size());
    for (const auto& value : values)
      encoder.appendU64(value.value());
  };
  encoder.appendU64(input.out_row_offsets.size());
  for (auto value : input.out_row_offsets)
    encoder.appendU64(value);
  append_u64s(input.out_arc_ids);
  encoder.appendU64(input.in_row_offsets.size());
  for (auto value : input.in_row_offsets)
    encoder.appendU64(value);
  append_u64s(input.in_arc_ids);
  append_u64s(input.ordered_root_ids);
  return encoder.bytes();
}

std::vector<std::uint8_t> clockConstraintPayload(const CsrReferenceInput& input)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("reference-clock-constraint-payload", input.schema_version);
  encoder.appendBool(input.clocks.empty() && input.port_constraints.empty() && input.case_analysis_constraints.empty());
  encoder.appendU64(input.clocks.size());
  for (const auto& clock : input.clocks) {
    encoder.appendString(clock.map_key);
    encoder.appendString(clock.embedded_clock_name);
    encoder.appendU64(clock.ordered_sources.size());
    for (const auto& source : clock.ordered_sources)
      encoder.appendString(source.full_name);
    encoder.appendDouble(clock.period);
    for (double edge : clock.edges)
      encoder.appendDouble(edge);
    encoder.appendBool(clock.propagated);
  }
  encoder.appendU64(input.port_constraints.size());
  for (const auto& constraint : input.port_constraints) {
    encoder.appendString(constraint.map_key);
    encoder.appendString(constraint.embedded_port_name);
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
    encoder.appendBool(constraint.value);
  }
  return encoder.bytes();
}

std::vector<std::uint8_t> libraryPayload(const CsrReferenceInput& input)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("reference-library-payload", input.schema_version);
  encoder.appendBool(input.referenced_library_cells.empty());
  encoder.appendU64(input.referenced_library_cells.size());
  for (const auto& cell : input.referenced_library_cells)
    detail::appendReferencedLibraryCellModelFields(encoder, cell);
  return encoder.bytes();
}

std::vector<std::uint8_t> cornerPayload(const CsrReferenceInput& input)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("reference-corner-payload", input.schema_version);
  const auto& context = input.analysis_context;
  encoder.appendBool(context.liberty_corner.empty() && context.temperature.empty() && context.rc_corner.empty());
  encoder.appendString(context.liberty_corner);
  encoder.appendString(context.temperature);
  encoder.appendString(context.rc_corner);
  return encoder.bytes();
}

std::vector<std::uint8_t> parasiticPayload(const CsrReferenceInput& input)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("reference-parasitic-payload", input.schema_version);
  encoder.appendBool(input.parasitics.nets.empty());
  detail::appendParasiticModelFields(encoder, input.parasitics);
  return encoder.bytes();
}

std::vector<std::uint8_t> timingInputPayload(const CsrReferenceInput& input)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("reference-timing-input-payload", input.schema_version);
  encoder.appendBool(input.nodes.empty() && input.arcs.empty() && input.start_seeds.empty() && input.endpoint_required.empty());
  for (AnalysisMode analysis : input.analysis_order)
    encoder.appendString(token(analysis));
  for (PathSource source : input.path_source_order)
    encoder.appendString(token(source));
  for (Transition transition : input.transition_order)
    encoder.appendString(token(transition));

  encoder.appendU64(input.nodes.size());
  for (const auto& node : input.nodes) {
    encoder.appendU64(node.id.value());
    encoder.appendBool(node.is_startpoint);
    encoder.appendBool(node.is_endpoint);
    encoder.appendBool(node.is_sequential);
    encoder.appendBool(node.is_clock_pin);
    encoder.appendBool(node.is_data_pin);
    encoder.appendBool(node.is_output_pin);
  }
  encoder.appendU64(input.instances.size());
  for (const auto& instance : input.instances) {
    encoder.appendString(instance.instance_name);
    encoder.appendBool(instance.is_sequential);
    encoder.appendBool(instance.has_timing_checks);
    encoder.appendBool(instance.is_clock_gating);
    encoder.appendBool(instance.is_macro);
  }
  encoder.appendU64(input.arcs.size());
  for (const auto& arc : input.arcs) {
    encoder.appendU64(arc.id.value());
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

  encoder.appendU64(input.clocks.size());
  for (const auto& clock : input.clocks) {
    encoder.appendU64(clock.ordered_sources.size());
    for (const auto& source : clock.ordered_sources)
      detail::appendOptionalNodeIdFields(encoder, source.resolved_node);
  }
  encoder.appendU64(input.clock_owner_by_node.size());
  for (const auto& owner : input.clock_owner_by_node) {
    encoder.appendBool(owner.has_value());
    if (owner)
      encoder.appendString(*owner);
  }
  encoder.appendU64(input.port_constraints.size());
  for (const auto& constraint : input.port_constraints)
    detail::appendOptionalNodeIdFields(encoder, constraint.resolved_node);
  encoder.appendU64(input.case_analysis_constraints.size());
  for (const auto& constraint : input.case_analysis_constraints)
    detail::appendOptionalNodeIdFields(encoder, constraint.resolved_node);

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
  const auto append_node_ids = [&](const std::vector<NodeId>& values) {
    encoder.appendU64(values.size());
    for (NodeId value : values)
      encoder.appendU64(value.value());
  };
  append_node_ids(input.ordered_start_ids);
  append_node_ids(input.ordered_end_ids);
  encoder.appendU64(input.clock_tags.size());
  for (const auto& tag : input.clock_tags)
    encoder.appendString(tag);

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

CsrSnapshotIdentity makeIdentityFromReferencePayloads(std::uint64_t session_token, std::uint64_t graph_generation, const FingerprintPayloads& payloads)
{
  return makeSnapshotIdentity(session_token, graph_generation, payloads);
}

}  // namespace
std::string makeReferenceTopologyFingerprint(const CsrReferenceInput& input)
{
  return componentFingerprint("topology-fingerprint", topologyPayload(input));
}

std::string makeReferenceClockConstraintFingerprint(const CsrReferenceInput& input)
{
  return componentFingerprint("clock-constraint-fingerprint", clockConstraintPayload(input));
}

std::string makeReferenceLibraryFingerprint(const CsrReferenceInput& input)
{
  return componentFingerprint("library-fingerprint", libraryPayload(input));
}

std::string makeReferenceCornerFingerprint(const CsrReferenceInput& input)
{
  return componentFingerprint("corner-fingerprint", cornerPayload(input));
}

std::string makeReferenceParasiticFingerprint(const CsrReferenceInput& input)
{
  return componentFingerprint("parasitic-fingerprint", parasiticPayload(input));
}

std::string makeReferenceTimingInputFingerprint(const CsrReferenceInput& input)
{
  const FingerprintPayloads payloads{topologyPayload(input), clockConstraintPayload(input), libraryPayload(input),
                                     cornerPayload(input),   parasiticPayload(input),       timingInputPayload(input)};
  return makeIdentityFromReferencePayloads(1, 0, payloads).timing_input_fingerprint;
}

FingerprintPayloads buildReferenceFingerprintPayloads(const CsrReferenceInput& input)
{
  return FingerprintPayloads{topologyPayload(input), clockConstraintPayload(input), libraryPayload(input),
                             cornerPayload(input),   parasiticPayload(input),       timingInputPayload(input)};
}

CsrSnapshotIdentity makeReferenceInputIdentity(const CsrReferenceInput& input, std::uint64_t session_token, std::uint64_t graph_generation)
{
  return makeIdentityFromReferencePayloads(session_token, graph_generation, buildReferenceFingerprintPayloads(input));
}

}  // namespace ista::csr
