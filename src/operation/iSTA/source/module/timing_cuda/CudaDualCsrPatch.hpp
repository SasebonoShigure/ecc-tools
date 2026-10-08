#pragma once

#include <optional>
#include <functional>
#include <map>
#include <vector>

#include "DualCsrDescriptors.hpp"

namespace ista::csr {
struct CudaAppendedBaseInRow
{
  NodeId sink;
  std::vector<ArcId> arc_ids;
};

struct CudaRcBindingChange
{
  NodeId node;
  CsrParasiticBinding expected, replacement;
};

// Only changed data crosses this boundary; no full CPU graph or timing shadow.
struct CudaDualCsrPatch
{
  timing_edit::TimingEditSessionToken session_token;
  timing_edit::TransactionGeneration expected_generation, target_generation;
  timing_edit::AppendOnlyHighWater before, after;
  std::vector<timing_dual_csr::NodeAppendRecord> node_appends;
  std::vector<timing_dual_csr::ArcAppendRecord> arc_appends;
  std::vector<timing_dual_csr::BaseOutRemoveDescriptor> removes;
  std::vector<timing_dual_csr::BaseOutChangeDescriptor> changes;
  std::vector<timing_dual_csr::BaseInChangeDescriptor> reverse_changes;
  std::vector<timing_dual_csr::ArcPayloadChangeDescriptor> payload_changes;
  std::vector<timing_dual_csr::LogicalArcDeactivate> logical_deactivations;
  std::vector<timing_dual_csr::AuxOutRowDescriptor> aux_rows;
  std::vector<timing_dual_csr::AppendedBaseOutRowDescriptor> appended_base_out_rows;
  std::vector<CudaAppendedBaseInRow> appended_base_in_rows;
  // Production buffer insertion leaves these absent. Algorithm tests may append
  // source/end nodes, using existing clock tags and explicit initial seeds.
  std::optional<timing_dual_csr::ValueReplacement<std::vector<NodeId>>> roots, ends;
  // Includes driver and every affected terminal, even deferred Q without arcs.
  std::vector<CudaRcBindingChange> rc_bindings;
  // Borrow immutable session models until stage() finishes its stream work.
  // Only newly introduced RC load-cell types are needed.
  std::map<std::string, std::reference_wrapper<const ReferencedLibraryCellModel>> rc_load_cells;
  std::vector<StartSeedModel> appended_start_seeds;
  std::vector<EndpointRequiredModel> appended_endpoints;
};

}  // namespace ista::csr
