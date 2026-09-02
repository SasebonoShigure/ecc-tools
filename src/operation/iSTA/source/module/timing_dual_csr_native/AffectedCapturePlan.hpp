// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "DualCsrState.hpp"

namespace ista::timing_dual_csr {

// Requests contain semantic identities only. Native slots, pointers and
// traversal ranks are deliberately absent: the candidate is resolved again
// after GraphBuilder has finished.
struct AffectedNodeCapture
{
  timing_edit::StableNodeRef ref;
  csr::NodeKey expected_key;

  friend bool operator==(const AffectedNodeCapture&,
                         const AffectedNodeCapture&) = default;
};

struct AffectedArcCapture
{
  timing_edit::StableArcRef ref;
  std::optional<csr::ArcKey> expected_current_key;
  csr::ArcKeyBase expected_post_base;
  std::uint64_t expected_post_multiplicity{0};

  friend bool operator==(const AffectedArcCapture&,
                         const AffectedArcCapture&) = default;
};

struct AffectedNetCapture
{
  timing_edit::StableNetRef ref;
  std::string expected_name;

  friend bool operator==(const AffectedNetCapture&,
                         const AffectedNetCapture&) = default;
};

struct AffectedNodeModelCapture
{
  timing_edit::StableNodeRef ref;
  csr::NodeKey expected_key;
  std::string expected_cell;
  std::string expected_port;

  friend bool operator==(const AffectedNodeModelCapture&,
                         const AffectedNodeModelCapture&) = default;
};

// This is a typed, exact batch binding. It intentionally contains no nonce or
// hash: the reader compares every field with the PreparedBatch it consumes.
struct AffectedCapturePlan
{
  timing_edit::TimingEditSessionToken session_token;
  timing_edit::TransactionGeneration expected_generation;
  timing_edit::TransactionGeneration target_generation;
  timing_edit::AppendOnlyHighWater before;
  timing_edit::AppendOnlyHighWater after;
  std::vector<timing_edit::BufferInsertionMutation> normalized_edits;
  timing_edit::TimingGraphDelta normalized_forward;
  timing_edit::TimingGraphDelta normalized_reverse;
  timing_edit::AppendOnlySlotPlan slot_plan;
  std::vector<timing_edit::TokenMaterialization> token_materializations;
  timing_edit::InvalidationClosure invalidation_closure;
  timing_edit::PolicyReceipt policy_receipt;
  std::vector<AffectedNodeCapture> nodes;
  std::vector<AffectedNetCapture> load_nets;
  // Frozen before any native role/capacitance projector is called.
  std::vector<AffectedNodeModelCapture> node_roles;
  std::vector<AffectedNodeModelCapture>
      cell_pin_capacitances;
  std::vector<AffectedArcCapture> arcs;

  friend bool operator==(const AffectedCapturePlan&,
                         const AffectedCapturePlan&) = default;
};

[[nodiscard]] AffectedCapturePlan makeAffectedCapturePlan(
    const timing_edit::PreparedBatch& batch,
    std::vector<AffectedNodeCapture> nodes,
    std::vector<AffectedNetCapture> load_nets,
    std::vector<AffectedNodeModelCapture> node_roles,
    std::vector<AffectedNodeModelCapture>
        cell_pin_capacitances,
    std::vector<AffectedArcCapture> arcs);

enum class NarrowCaptureError : std::uint8_t
{
  kBatchBindingMismatch,
  kInvalidGeneration,
  kInvalidHighWater,
  kUnsupportedPolicy,
  kParasiticModeMismatch,
  kDuplicateRequest,
  kUnplannedDependency,
  kNativeNodeNotFound,
  kNativeInstanceNotFound,
  kNativeNetNotFound,
  kArcMultiplicityMismatch,
  kArcOrdinalMismatch,
  kArcKeyMismatch,
  kMalformedNativeRecord,
  kFullExportObserved
};

struct NarrowCaptureAudit
{
  std::uint64_t requested_nodes{0};
  std::uint64_t node_reads{0};
  std::uint64_t accepted_nodes{0};
  std::uint64_t requested_instances{0};
  std::uint64_t instance_reads{0};
  std::uint64_t accepted_instances{0};
  std::uint64_t requested_arcs{0};
  std::uint64_t native_arc_candidate_reads{0};
  std::uint64_t accepted_arcs{0};
  std::uint64_t requested_deactivations{0};
  std::uint64_t deactivated_arc_before_reads{0};
  std::uint64_t accepted_deactivations{0};
  std::uint64_t captured_arc_records{0};
  std::uint64_t requested_load_nets{0};
  std::uint64_t net_reads{0};
  std::uint64_t accepted_load_nets{0};
  std::uint64_t requested_library_cells{0};
  std::uint64_t library_cell_reads{0};
  std::uint64_t accepted_library_cells{0};
  std::uint64_t requested_cell_pin_capacitances{0};
  std::uint64_t cell_pin_capacitance_reads{0};
  std::uint64_t accepted_cell_pin_capacitances{0};
  std::uint64_t requested_node_roles{0};
  std::uint64_t node_role_reads{0};
  std::uint64_t accepted_node_roles{0};
  std::uint64_t index_group_lookups{0};
  std::uint64_t native_arc_candidate_upper_bound{0};
  std::uint64_t arc_identity_tie_breaks{0};
  std::uint64_t rejected_reads{0};
  bool full_export_guard_active{false};
  bool full_export_observed{false};
  std::uint64_t native_full_graph_exports{0};
  std::uint64_t native_full_reference_exports{0};

  friend bool operator==(const NarrowCaptureAudit&,
                         const NarrowCaptureAudit&) = default;
};

}  // namespace ista::timing_dual_csr
