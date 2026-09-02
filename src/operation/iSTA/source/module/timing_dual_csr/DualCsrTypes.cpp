// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTypes.hpp"

namespace ista::timing_dual_csr {

bool DualCsrReferencePayloadDelta::empty() const noexcept
{
  return !schema_version && !source_identity && !analysis_context
         && !analysis_order && !path_source_order && !transition_order
         && !instances && !ordered_root_ids && !ordered_start_ids
         && !ordered_end_ids && !clock_tags && !clocks
         && !clock_owner_by_node && !port_constraints
         && !case_analysis_constraints && !timing_check_endpoints
         && !referenced_library_cells && !parasitics && !start_seeds
         && !endpoint_required && !cell_pin_capacitances
         && !unsupported_reasons;
}

DualCsrReferencePayload makeDualCsrReferencePayload(
    const csr::CsrReferenceInput& input)
{
  DualCsrReferencePayload result;
  result.schema_version = input.schema_version;
  result.source_identity = input.source_identity;
  result.analysis_context = input.analysis_context;
  result.analysis_order = input.analysis_order;
  result.path_source_order = input.path_source_order;
  result.transition_order = input.transition_order;
  result.instances = input.instances;
  result.ordered_root_ids = input.ordered_root_ids;
  result.ordered_start_ids = input.ordered_start_ids;
  result.ordered_end_ids = input.ordered_end_ids;
  result.clock_tags = input.clock_tags;
  result.clocks = input.clocks;
  result.clock_owner_by_node = input.clock_owner_by_node;
  result.port_constraints = input.port_constraints;
  result.case_analysis_constraints = input.case_analysis_constraints;
  result.timing_check_endpoints = input.timing_check_endpoints;
  result.referenced_library_cells = input.referenced_library_cells;
  result.parasitics = input.parasitics;
  result.start_seeds = input.start_seeds;
  result.endpoint_required = input.endpoint_required;
  result.cell_pin_capacitances = input.cell_pin_capacitances;
  result.unsupported_reasons = input.unsupported_reasons;
  return result;
}

}  // namespace ista::timing_dual_csr
