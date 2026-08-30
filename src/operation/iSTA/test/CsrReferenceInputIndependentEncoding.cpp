// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <cstdint>

#include "CsrReferenceInputIndependentEncodingInternal.hpp"
#include "CsrReferenceInputTestSupport.hpp"

namespace ista::csr::test {
namespace {

template <typename Id>
void appendIds(wire::Writer& writer, const std::vector<Id>& values)
{
  writer.appendU64(static_cast<std::uint64_t>(values.size()));
  for (Id value : values)
    writer.appendU64(value.value());
}

void appendU64s(wire::Writer& writer, const std::vector<std::uint64_t>& values)
{
  writer.appendVector(values, [](wire::Writer& local, std::uint64_t value) { local.appendU64(value); });
}

}  // namespace

std::vector<std::uint8_t> canonicalIndependentReferenceInputBytes(const CsrReferenceInput& input)
{
  wire::Writer writer;
  writer.appendString("ista-csr-reference-input-test-wire/1.0.0");
  writer.appendString(input.schema_version);
  wire::appendSnapshotIdentity(writer, input.source_identity);
  writer.appendString(input.analysis_context.liberty_corner);
  writer.appendString(input.analysis_context.temperature);
  writer.appendString(input.analysis_context.rc_corner);
  for (AnalysisMode value : input.analysis_order)
    writer.appendEnum(value);
  for (PathSource value : input.path_source_order)
    writer.appendEnum(value);
  for (Transition value : input.transition_order)
    writer.appendEnum(value);

  writer.appendVector(input.nodes, wire::appendNode);
  writer.appendVector(input.node_ids_by_key, wire::appendNodeLookup);
  writer.appendVector(input.instances, wire::appendInstance);
  writer.appendVector(input.arcs, wire::appendArc);
  writer.appendVector(input.arc_ids_by_key, wire::appendArcLookup);
  appendU64s(writer, input.out_row_offsets);
  appendIds(writer, input.out_arc_ids);
  appendU64s(writer, input.in_row_offsets);
  appendIds(writer, input.in_arc_ids);
  appendIds(writer, input.ordered_root_ids);
  appendIds(writer, input.ordered_start_ids);
  appendIds(writer, input.ordered_end_ids);
  writer.appendVector(input.clock_tags, [](wire::Writer& local, const std::string& value) { local.appendString(value); });
  writer.appendVector(input.clocks, wire::appendClock);
  writer.appendVector(input.clock_owner_by_node, [](wire::Writer& local, const std::optional<std::string>& owner) {
    local.appendOptional(owner, [](wire::Writer& nested, const std::string& value) { nested.appendString(value); });
  });
  writer.appendVector(input.port_constraints, wire::appendPortConstraint);
  writer.appendVector(input.case_analysis_constraints, wire::appendCaseConstraint);
  writer.appendVector(input.timing_check_endpoints, wire::appendTimingCheckEndpoint);
  writer.appendVector(input.referenced_library_cells, wire::appendLibraryCell);
  wire::appendParasitics(writer, input.parasitics);
  writer.appendVector(input.start_seeds, wire::appendStartSeed);
  writer.appendVector(input.endpoint_required, wire::appendEndpointRequired);
  writer.appendVector(input.cell_pin_capacitances, wire::appendPinCapacitance);
  writer.appendVector(input.unsupported_reasons, [](wire::Writer& local, CsrReferenceUnsupportedReason reason) { local.appendEnum(reason); });
  return writer.bytes();
}

}  // namespace ista::csr::test
