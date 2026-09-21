// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "DualCsrDescriptors.hpp"

namespace ista::timing_dual_csr {

struct DualCsrArcGroupIndexBinding
{
  csr::ArcId id;
  timing_edit::StableArcRef ref;

  friend bool operator==(const DualCsrArcGroupIndexBinding&,
                         const DualCsrArcGroupIndexBinding&) = default;
};

struct DualCsrArcGroupIndexSlot
{
  timing_edit::StableArcRef ref;
  std::optional<csr::ArcKey> current_active_key;

  friend bool operator==(const DualCsrArcGroupIndexSlot&,
                         const DualCsrArcGroupIndexSlot&) = default;
};

struct DualCsrArcGroup
{
  csr::ArcKeyBase base;
  std::vector<csr::ArcId> active_arc_ids;

  friend bool operator==(const DualCsrArcGroup&,
                         const DualCsrArcGroup&) = default;
};

struct DualCsrArcSinkNetGroup
{
  std::string sink_net_name;
  std::vector<csr::ArcId> active_arc_ids;

  friend bool operator==(const DualCsrArcSinkNetGroup&,
                         const DualCsrArcSinkNetGroup&) = default;
};

struct DualCsrNetSinkKey
{
  std::string owner_name;
  csr::NodeKey sink;

  friend bool operator==(const DualCsrNetSinkKey&,
                         const DualCsrNetSinkKey&) = default;
};

struct DualCsrNetSinkGroup
{
  DualCsrNetSinkKey key;
  std::vector<csr::ArcId> active_arc_ids;

  friend bool operator==(const DualCsrNetSinkGroup&,
                         const DualCsrNetSinkGroup&) = default;
};

struct DualCsrArcGroupIndex
{
  timing_edit::TimingEditSessionToken session_token;
  timing_edit::TransactionGeneration generation;
  std::uint64_t arc_high_water{0};
  std::vector<DualCsrArcGroupIndexSlot> slots;
  std::vector<DualCsrArcGroup> active_groups;
  std::vector<DualCsrArcSinkNetGroup> active_sink_net_groups;
  std::vector<DualCsrNetSinkGroup> active_net_sink_groups;
  std::vector<csr::NodeId> clock_source_node_ids;

  [[nodiscard]] const DualCsrArcGroupIndexSlot* slot(
      csr::ArcId id) const noexcept;
  [[nodiscard]] const DualCsrArcGroup* activeGroup(
      const csr::ArcKeyBase& base) const noexcept;
  [[nodiscard]] const DualCsrArcSinkNetGroup* activeSinkNetGroup(
      std::string_view sink_net_name) const noexcept;
  [[nodiscard]] const DualCsrNetSinkGroup* activeNetSinkGroup(
      std::string_view owner_name,
      const csr::NodeKey& sink) const noexcept;
  [[nodiscard]] bool isClockSourceNode(
      csr::NodeId id) const noexcept;

  void swap(DualCsrArcGroupIndex& other) noexcept;

  friend bool operator==(const DualCsrArcGroupIndex&,
                         const DualCsrArcGroupIndex&) = default;
};

void swap(DualCsrArcGroupIndex& lhs,
          DualCsrArcGroupIndex& rhs) noexcept;

struct DualCsrArcGroupIndexAttempt
{
  std::optional<DualCsrArcGroupIndex> index;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return index.has_value() && violations.empty();
  }
};

// Baseline/recovery supplies one typed StableArcRef binding per logical
// ArcId. Normal targeted batches must use the descriptor transition below.
[[nodiscard]] DualCsrArcGroupIndexAttempt buildDualCsrArcGroupIndex(
    const DualCsrState& graph,
    std::span<const DualCsrArcGroupIndexBinding> bindings);

// Applies only the descriptor's typed append/deactivate/rekey delta. It does
// not inspect a resolver or native graph.


}  // namespace ista::timing_dual_csr
