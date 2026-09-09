// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <map>
#include <string>

#include "TimingEditTypes.hpp"

namespace ista {
class Database;
}

namespace ista::timing_edit {

struct NativeSlotBinding
{
  AppendOnlySlot slot;
  std::string native_name;
  bool tombstone{false};

  friend bool operator==(const NativeSlotBinding&, const NativeSlotBinding&) = default;
};

// Session-local resolver state. Baseline ArcKey ordinals are bound once by
// bindBaseline() and are never reinterpreted against a later native vector.
class TimingEditResolverState final
{
 public:
  TimingEditResolverState() = default;

  [[nodiscard]] static TimingEditResolverState bindBaseline(const Database& database);
  [[nodiscard]] TimingEditResolverState stage(const PreparedBatch& batch) const;

  [[nodiscard]] const std::string& resolveNode(const StableNodeRef& ref) const;
  [[nodiscard]] const std::string& resolveNet(const StableNetRef& ref) const;
  [[nodiscard]] AppendOnlySlot resolveArcSlot(const StableArcRef& ref) const;
  [[nodiscard]] const TokenMaterialization& materialization(const CreationToken& token) const;
  [[nodiscard]] StableNodeRef stableNodeForNativeName(const std::string& native_name) const;
  [[nodiscard]] StableNetRef stableNetForNativeName(const std::string& native_name) const;

  [[nodiscard]] const AppendOnlyHighWater& highWater() const noexcept { return _high_water; }
  [[nodiscard]] const std::map<StableNodeRef, NativeSlotBinding>& nodeBindings() const noexcept { return _nodes; }
  [[nodiscard]] const std::map<StableNetRef, NativeSlotBinding>& netBindings() const noexcept { return _nets; }
  [[nodiscard]] const std::map<StableArcRef, NativeSlotBinding>& arcBindings() const noexcept { return _arcs; }
  [[nodiscard]] const std::map<CreationToken, TokenMaterialization>& materializations() const noexcept { return _materializations; }

  friend bool operator==(const TimingEditResolverState&, const TimingEditResolverState&) = default;

 private:
  template <typename CanonicalRecords>
  [[nodiscard]] static TimingEditResolverState bindRecords(const Database& database, const CanonicalRecords& input);
  [[nodiscard]] static TimingEditResolverState bindReferenceBaseline(const Database& database, const csr::CsrReferenceInput& input);
  [[nodiscard]] TimingEditResolverState stageAssignments(const AppendOnlySlotPlan& plan, const std::vector<TokenMaterialization>& values) const;
  friend class NativeTargetedEditPreparer;
  [[nodiscard]] std::string materializedNodeName(const StableNodeRef& ref) const;
  [[nodiscard]] std::string materializedNetName(const StableNetRef& ref) const;

  AppendOnlyHighWater _high_water;
  std::map<StableNodeRef, NativeSlotBinding> _nodes;
  std::map<StableNetRef, NativeSlotBinding> _nets;
  std::map<StableArcRef, NativeSlotBinding> _arcs;
  std::map<CreationToken, TokenMaterialization> _materializations;
};

}  // namespace ista::timing_edit
