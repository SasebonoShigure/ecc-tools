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

struct NodeAppendRecord
{
  csr::NodeId id;
  csr::NodeKey key;
  csr::NodeRecord payload;

  friend bool operator==(const NodeAppendRecord&, const NodeAppendRecord&) = default;
};

struct ArcAppendRecord
{
  csr::ArcId id;
  csr::ArcKey key;
  csr::NodeId source;
  csr::NodeId sink;
  csr::ArcSemanticPayload payload;

  friend bool operator==(const ArcAppendRecord&, const ArcAppendRecord&) = default;
};

struct NodePayloadChange
{
  csr::NodeId id;
  csr::NodeKey expected_key;
  std::string expected_net_name;
  std::string replacement_net_name;

  friend bool operator==(const NodePayloadChange&, const NodePayloadChange&) = default;
};

struct BaseOutRemoveDescriptor
{
  csr::NodeId source;
  std::uint64_t physical_slot{0};
  csr::ArcId expected_arc;

  friend bool operator==(const BaseOutRemoveDescriptor&,
                         const BaseOutRemoveDescriptor&) = default;
};

struct BaseOutChangeDescriptor
{
  csr::NodeId source;
  std::uint64_t physical_slot{0};
  std::optional<csr::ArcId> expected_arc;
  csr::ArcId replacement_arc;

  friend bool operator==(const BaseOutChangeDescriptor&,
                         const BaseOutChangeDescriptor&) = default;
};

struct BaseInChangeDescriptor
{
  csr::NodeId sink;
  std::uint64_t physical_slot{0};
  csr::ArcId expected_arc;
  csr::ArcId replacement_arc;

  friend bool operator==(const BaseInChangeDescriptor&,
                         const BaseInChangeDescriptor&) = default;
};

struct ArcPayloadChangeDescriptor
{
  csr::ArcId id;
  csr::ArcSemanticPayload expected;
  csr::ArcSemanticPayload replacement;

  friend bool operator==(const ArcPayloadChangeDescriptor&,
                         const ArcPayloadChangeDescriptor&) = default;
};

enum class LogicalArcDeactivateReason : std::uint8_t
{
  kRemoved,
  kReplaced,
  kAuxCleared
};

struct LogicalArcDeactivate
{
  csr::ArcId id;
  csr::ArcKey key;
  csr::NodeId source;
  csr::NodeId sink;
  LogicalArcDeactivateReason reason{LogicalArcDeactivateReason::kRemoved};
  bool expected_active{true};

  friend bool operator==(const LogicalArcDeactivate&,
                         const LogicalArcDeactivate&) = default;
};

struct AuxOutRowDescriptor
{
  csr::NodeId source;
  std::vector<csr::ArcId> expected_arc_ids;
  std::vector<csr::ArcId> replacement_arc_ids;

  friend bool operator==(const AuxOutRowDescriptor&,
                         const AuxOutRowDescriptor&) = default;
};

struct AppendedBaseOutRowDescriptor
{
  csr::NodeId source;
  std::vector<csr::ArcId> arc_ids;

  friend bool operator==(const AppendedBaseOutRowDescriptor&,
                         const AppendedBaseOutRowDescriptor&) = default;
};

struct AffectedBaseInRowDescriptor
{
  csr::NodeId sink;
  std::vector<csr::ArcId> expected_arc_ids;
  std::vector<csr::ArcId> replacement_arc_ids;

  friend bool operator==(const AffectedBaseInRowDescriptor&,
                         const AffectedBaseInRowDescriptor&) = default;
};

struct ArcGroupIndexDelta
{
  csr::ArcId id;
  timing_edit::StableArcRef ref;
  std::optional<csr::ArcKey> expected_active_key;
  std::optional<csr::ArcKey> replacement_active_key;

  friend bool operator==(const ArcGroupIndexDelta&,
                         const ArcGroupIndexDelta&) = default;
};

struct DualCsrBatchDescriptor
{
  std::string schema{kDualCsrDescriptorSchema};
  timing_edit::TimingEditSessionToken session_token;
  timing_edit::TransactionGeneration expected_generation;
  timing_edit::TransactionGeneration target_generation;
  timing_edit::AppendOnlyHighWater before;
  timing_edit::AppendOnlyHighWater after;
  std::vector<NodeAppendRecord> node_appends;
  std::vector<ArcAppendRecord> arc_appends;
  std::vector<NodePayloadChange> node_payload_changes;
  std::vector<BaseOutRemoveDescriptor> removes;
  std::vector<BaseOutChangeDescriptor> changes;
  std::vector<BaseInChangeDescriptor> reverse_changes;
  std::vector<ArcPayloadChangeDescriptor> payload_changes;
  std::vector<LogicalArcDeactivate> logical_deactivations;
  std::vector<AuxOutRowDescriptor> aux_rows;
  std::vector<AppendedBaseOutRowDescriptor>
      appended_base_out_rows;
  std::vector<AffectedBaseInRowDescriptor>
      affected_base_in_rows;
  DualCsrReferencePayloadDelta reference_payload;
  std::vector<ArcGroupIndexDelta> arc_group_index_delta;

  friend bool operator==(const DualCsrBatchDescriptor&,
                         const DualCsrBatchDescriptor&) = default;
};

}  // namespace ista::timing_dual_csr
