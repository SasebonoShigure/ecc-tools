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
#include <utility>

#include "CsrReferenceInputAdapterInternal.hpp"

namespace ista::csr::detail {
namespace {

const ReferencedLibraryCellModel& requireReferencedCell(const ReferenceBuildState& state, const std::string& cell_name)
{
  const auto found = state.referenced_cell_indices_by_name.find(cell_name);
  if (found == state.referenced_cell_indices_by_name.end() || found->second >= state.input.referenced_library_cells.size())
    malformedReferenceInput();
  return state.input.referenced_library_cells[found->second];
}

void validateResolvedEndpoint(const ReferenceBuildState& state, const std::string& instance_name, const std::optional<NodeId>& resolved)
{
  if (!resolved)
    return;
  const NodeRecord& node = requireNode(state, *resolved);
  if (node.is_port || node.key.instance_name != instance_name)
    malformedReferenceInput();
}

void validateNativeCheck(const ista::TimingCheckArc& native_check, const LibraryTimingCheckArcModel& closure, const std::string& clock_full_name,
                         const std::string& data_full_name)
{
  const auto& native_children = native_check.get_timing_arc_list();
  if (native_check.get_clock_port() != clock_full_name || native_check.get_data_port() != data_full_name
      || convertReferenceTimingCheckKind(native_check.get_check_type()) != closure.kind || native_check.get_check_time() != closure.check_time
      || convertReferenceTransition(native_check.get_clock_trans_type()) != closure.clock_transition || native_children.size() != closure.children.size())
    malformedReferenceInput();

  for (std::size_t child = 0; child < closure.children.size(); ++child)
    if (!(exportReferenceChildTimingArc(native_children.at(child)) == closure.children.at(child)))
      malformedReferenceInput();
}

}  // namespace

void exportReferenceTimingChecks(ReferenceBuildState& state)
{
  for (auto& instance : state.input.instances) {
    const ista::Instance& native_instance = requireNativeInstance(state, instance.instance_name);
    const auto& closure = requireReferencedCell(state, instance.cell_name);
    const auto& native_checks = native_instance.get_check_arc_list();
    instance.has_timing_checks = !native_checks.empty();

    const std::size_t closure_count = closure.present ? closure.timing_checks.size() : 0;
    if (native_checks.size() != closure_count)
      malformedReferenceInput();

    for (std::size_t ordinal = 0; ordinal < closure_count; ++ordinal) {
      const auto& check = closure.timing_checks.at(ordinal);
      TimingCheckEndpointModel endpoint;
      endpoint.instance_name = instance.instance_name;
      endpoint.referenced_check_ordinal = ordinal;
      endpoint.check_kind = check.kind;
      endpoint.clock_pin_raw_name = check.clock_port;
      endpoint.data_pin_raw_name = check.data_port;
      endpoint.clock_pin_full_name = instance.instance_name + ":" + check.clock_port;
      endpoint.data_pin_full_name = instance.instance_name + ":" + check.data_port;
      endpoint.resolved_clock_node = resolveNode(state, endpoint.clock_pin_full_name);
      endpoint.resolved_data_node = resolveNode(state, endpoint.data_pin_full_name);
      validateResolvedEndpoint(state, instance.instance_name, endpoint.resolved_clock_node);
      validateResolvedEndpoint(state, instance.instance_name, endpoint.resolved_data_node);
      validateNativeCheck(native_checks.at(ordinal), check, endpoint.clock_pin_full_name, endpoint.data_pin_full_name);
      if (endpoint.resolved_data_node)
        state.timing_check_data_nodes[endpoint.resolved_data_node->value()] = 1;
      state.input.timing_check_endpoints.push_back(std::move(endpoint));
    }
  }
}

}  // namespace ista::csr::detail
