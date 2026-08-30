// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include "CsrReferenceInputIndependentEncodingInternal.hpp"

namespace ista::csr::test::wire {
namespace {

void appendTableSignature(Writer& writer, const TimingTableSignature& signature)
{
  writer.appendBool(signature.present);
  writer.appendEnum(signature.variable1);
  writer.appendEnum(signature.variable2);
}

void appendEligibility(Writer& writer, const ArcEligibility& eligibility)
{
  writer.appendBool(eligibility.levelization);
  writer.appendBool(eligibility.data_arrival);
  writer.appendBool(eligibility.data_slew);
  writer.appendBool(eligibility.clock);
}

void appendLoad(Writer& writer, const ArcOutputLoadMatrix& load)
{
  for (const auto& analysis : load)
    for (double transition : analysis)
      writer.appendFiniteDouble(transition);
}

}  // namespace

void appendSnapshotIdentity(Writer& writer, const CsrSnapshotIdentity& identity)
{
  writer.appendString(identity.fingerprint_schema_version);
  writer.appendU64(identity.session_token);
  writer.appendU64(identity.graph_generation);
  writer.appendString(identity.topology_fingerprint);
  writer.appendString(identity.clock_constraint_fingerprint);
  writer.appendString(identity.library_fingerprint);
  writer.appendString(identity.corner_fingerprint);
  writer.appendString(identity.parasitic_fingerprint);
  writer.appendString(identity.timing_input_fingerprint);
}

void appendNodeKey(Writer& writer, const NodeKey& key)
{
  writer.appendEnum(key.kind);
  writer.appendString(key.full_name);
  writer.appendString(key.instance_name);
  writer.appendString(key.pin_name);
}

void appendLibraryArcSignature(Writer& writer, const LibraryArcSignature& signature)
{
  writer.appendString(signature.library_name);
  writer.appendI32(signature.arc_index);
  writer.appendEnum(signature.sense);
  writer.appendEnum(signature.trigger_transition);
  writer.appendEnum(signature.check_transition);
  writer.appendString(signature.sdf_condition);
  for (const auto& table : signature.delay_tables)
    appendTableSignature(writer, table);
  for (const auto& table : signature.slew_tables)
    appendTableSignature(writer, table);
}

void appendArcKey(Writer& writer, const ArcKey& key)
{
  appendNodeKey(writer, key.base.source);
  appendNodeKey(writer, key.base.sink);
  writer.appendEnum(key.base.kind);
  writer.appendString(key.base.owner_name);
  writer.appendString(key.base.library_source_port);
  writer.appendString(key.base.library_sink_port);
  writer.appendVector(key.base.library_arcs, appendLibraryArcSignature);
  writer.appendU64(key.parallel_ordinal);
}

void appendNode(Writer& writer, const NodeRecord& node)
{
  writer.appendU64(node.id.value());
  appendNodeKey(writer, node.key);
  writer.appendEnum(node.direction);
  writer.appendString(node.instance_cell);
  writer.appendString(node.net_name);
  writer.appendBool(node.is_port);
  writer.appendBool(node.is_startpoint);
  writer.appendBool(node.is_endpoint);
  writer.appendBool(node.is_sequential);
  writer.appendBool(node.is_clock_pin);
  writer.appendBool(node.is_data_pin);
  writer.appendBool(node.is_output_pin);
}

void appendNodeLookup(Writer& writer, const NodeIdLookupEntry& entry)
{
  appendNodeKey(writer, entry.key);
  writer.appendU64(entry.id.value());
}

void appendInstance(Writer& writer, const InstanceRecord& instance)
{
  writer.appendString(instance.instance_name);
  writer.appendString(instance.cell_name);
  writer.appendVector(instance.ordered_pins, appendNodeId);
  writer.appendBool(instance.is_sequential);
  writer.appendBool(instance.has_timing_checks);
  writer.appendBool(instance.is_clock_gating);
  writer.appendBool(instance.is_macro);
}

void appendArc(Writer& writer, const ArcRecord& arc)
{
  writer.appendU64(arc.id.value());
  appendArcKey(writer, arc.key);
  writer.appendU64(arc.source.value());
  writer.appendU64(arc.sink.value());
  writer.appendEnum(arc.kind);
  writer.appendU64(arc.native_traversal_rank);
  writer.appendBool(arc.is_clock_arc);
  writer.appendBool(arc.is_disabled);
  writer.appendBool(arc.is_loop_disabled);
  appendEligibility(writer, arc.eligibility);
  appendTransfer(writer, arc.transfer);
  writer.appendOptional(arc.output_load, appendLoad);
}

void appendArcLookup(Writer& writer, const ArcIdLookupEntry& entry)
{
  appendArcKey(writer, entry.key);
  writer.appendU64(entry.id.value());
}

}  // namespace ista::csr::test::wire
