// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeBufferCellValidation.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "Database.hpp"

namespace ista::timing_edit {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("native buffer cell: " + std::move(message));
}

void requirePortDirection(const TimingCellPort& port, bool input)
{
  if (input ? (port.get_is_input() && !port.get_is_output() && !port.get_is_clock())
            : (port.get_is_output() && !port.get_is_input() && !port.get_is_clock())) {
    return;
  }
  invalid(input ? "selected input port has an incompatible direction" : "selected output port has an incompatible direction");
}

bool isIdentityFunction(const TimingCellPort& output, const std::string& input_port)
{
  // The legacy native getter is not const. Inspecting a value copy keeps this
  // validation read-only with respect to the active Database.
  TimingCellPort copy = output;
  auto& terms = copy.get_function_expression().get_term_list();
  return terms.size() == 1 && terms.front().get_operation_type() == LogicOperationType::kPort
         && terms.front().get_port_name() == input_port;
}

std::vector<csr::LibraryArcSignature> exportSignatures(const TimingCellArc& arc)
{
  std::vector<csr::LibraryArcSignature> signatures;
  signatures.reserve(arc.get_timing_arc_list().size());
  for (const auto& child : arc.get_timing_arc_list()) {
    signatures.push_back(csr::detail::exportReferenceLibraryArcSignature(child));
  }
  return signatures;
}

}  // namespace

const TimingCell& requireLegalNativeBufferCell(const Database& database, const LibraryCellRef& reference)
{
  const auto cell = database.get_timing_library().get_cell_map().find(reference.cell_name);
  if (cell == database.get_timing_library().get_cell_map().end()) {
    invalid("library cell does not exist: " + reference.cell_name);
  }
  const TimingCell& value = cell->second;
  if (value.get_library_name() != reference.library_name) {
    invalid("library cell resolves in a different library");
  }
  if (value.get_is_sequential() || value.get_is_clock_gating() || value.get_is_macro()) {
    invalid("cell must be combinational, non-gating, and non-macro");
  }
  if (reference.input_port.empty() || reference.output_port.empty() || reference.input_port == reference.output_port) {
    invalid("distinct explicit input and output ports are required");
  }
  const auto input = value.get_port_map().find(reference.input_port);
  const auto output = value.get_port_map().find(reference.output_port);
  if (input == value.get_port_map().end() || output == value.get_port_map().end()) {
    invalid("selected library port does not exist");
  }
  requirePortDirection(input->second, true);
  requirePortDirection(output->second, false);
  if (value.get_port_map().size() != 2 || !isIdentityFunction(output->second, reference.input_port)) {
    invalid("cell is not a two-port non-inverting buffer");
  }

  std::size_t enabled_graph_arc_count = 0;
  std::size_t legal_arc_count = 0;
  for (const auto& arc : value.get_cell_arc_list()) {
    if (!arc.get_is_timing_graph_arc() || arc.get_is_clock_arc() || arc.get_is_clear_preset_arc() || arc.get_is_disable_arc()) {
      continue;
    }
    ++enabled_graph_arc_count;
    if (arc.get_source_port() != reference.input_port || arc.get_sink_port() != reference.output_port) {
      continue;
    }
    const auto signatures = exportSignatures(arc);
    if (signatures != reference.ordered_arc_signatures) {
      continue;
    }
    if (!std::ranges::all_of(signatures,
                             [](const csr::LibraryArcSignature& signature) { return signature.sense == csr::TimingSense::kPositiveUnate; })) {
      invalid("matched timing arcs are not all explicitly positive-unate");
    }
    ++legal_arc_count;
  }
  if (enabled_graph_arc_count != 1 || legal_arc_count != 1) {
    invalid("port pair does not resolve to exactly one matching enabled graph arc signature set");
  }
  return value;
}

}  // namespace ista::timing_edit
