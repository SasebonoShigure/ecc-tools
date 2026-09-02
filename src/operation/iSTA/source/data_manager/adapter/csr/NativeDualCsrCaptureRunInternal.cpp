// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDualCsrCaptureRunInternal.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace ista::timing_dual_csr::detail {

[[noreturn]] void abortCapture(NarrowCaptureError error)
{
  throw CaptureAbort{error};
}

CaptureRun::CaptureRun(const AffectedCapturePlan& plan)
    : _before(csr::detail::currentNativeFullExportCounters())
{
  _records.audit.requested_nodes = plan.nodes.size();
  _records.audit.requested_instances
      = plan.token_materializations.size();
  _records.audit.requested_arcs = std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity != 0;
      });
  _records.audit.requested_deactivations = std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity == 0;
      });
  _records.audit.requested_load_nets = plan.load_nets.size();
  _records.audit.requested_node_roles = plan.node_roles.size();
  _records.audit.requested_cell_pin_capacitances
      = plan.cell_pin_capacitances.size();
}

NativeCaptureResult CaptureRun::fail(NarrowCaptureError error)
{
  observeExports();
  ++_records.audit.rejected_reads;
  return {std::nullopt, error, _records.audit};
}

NativeCaptureResult CaptureRun::succeed()
{
  observeExports();
  if (_records.audit.full_export_observed) {
    ++_records.audit.rejected_reads;
    return {std::nullopt, NarrowCaptureError::kFullExportObserved,
            _records.audit};
  }
  _records.audit.full_export_guard_active = true;
  const NarrowCaptureAudit audit = _records.audit;
  return {std::move(_records), std::nullopt, audit};
}

void CaptureRun::observeExports()
{
  const auto after = csr::detail::currentNativeFullExportCounters();
  _records.audit.full_export_guard_active = true;
  if (after.graph_exports < _before.graph_exports
      || after.reference_exports < _before.reference_exports) {
    _records.audit.full_export_observed = true;
    return;
  }
  _records.audit.native_full_graph_exports
      = after.graph_exports - _before.graph_exports;
  _records.audit.native_full_reference_exports
      = after.reference_exports - _before.reference_exports;
  _records.audit.full_export_observed
      = _records.audit.native_full_graph_exports != 0
        || _records.audit.native_full_reference_exports != 0;
}

}  // namespace ista::timing_dual_csr::detail
