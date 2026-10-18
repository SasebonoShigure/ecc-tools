// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <map>
#include <memory>
#include <string>

#include "TimingEditTypes.hpp"

namespace ista {
class Database;
class CudaCsrEditState;
}

namespace ista::timing_edit {
namespace detail { struct PreparedNativeTargetedMetadata; }
class TimingEditResolverDelta;
class TimingEditResolverView;
class PreparedTimingEditResolverPatch;

struct NativeSlotBinding
{
  AppendOnlySlot slot;
  std::string native_name;
  bool tombstone{false};

  friend bool operator==(const NativeSlotBinding&, const NativeSlotBinding&) = default;
};

using NodeBindingEntry = std::map<StableNodeRef, NativeSlotBinding>::value_type;
using NetBindingEntry = std::map<StableNetRef, NativeSlotBinding>::value_type;

// Session-local resolver state. Baseline ArcKey ordinals are bound once by
// bindBaseline() and are never reinterpreted against a later native vector.
class TimingEditResolverState final
{
 public:
  TimingEditResolverState() = default;

  [[nodiscard]] static TimingEditResolverState bindBaseline(const Database& database);
  [[nodiscard]] TimingEditResolverState stage(const PreparedBatch& batch) const;
  [[nodiscard]] TimingEditResolverDelta prepare(const PreparedBatch& batch) const;

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
  friend class ista::CudaCsrEditState;
  friend class NativeFullEditPreparer;
  friend struct detail::PreparedNativeTargetedMetadata;
  friend class TimingEditResolverView;
  friend class PreparedTimingEditResolverPatch;
  [[nodiscard]] TimingEditResolverDelta prepareAssignmentsDelta(const AppendOnlySlotPlan& plan, const std::vector<TokenMaterialization>& values) const;

  AppendOnlyHighWater _high_water;
  std::map<StableNodeRef, NativeSlotBinding> _nodes;
  std::map<StableNetRef, NativeSlotBinding> _nets;
  std::map<StableArcRef, NativeSlotBinding> _arcs;
  std::map<CreationToken, TokenMaterialization> _materializations;
};

// Only touched bindings and new map nodes; no committed history prefix.
class TimingEditResolverDelta final
{
 public:
  TimingEditResolverDelta(TimingEditResolverDelta&&) noexcept = default;
  TimingEditResolverDelta& operator=(TimingEditResolverDelta&&) noexcept = default;
  TimingEditResolverDelta(const TimingEditResolverDelta&) = delete;
  TimingEditResolverDelta& operator=(const TimingEditResolverDelta&) = delete;
  [[nodiscard]] const AppendOnlyHighWater& highWater() const noexcept { return _changes.highWater(); }

 private:
  TimingEditResolverDelta() = default;
  TimingEditResolverState _expected;
  TimingEditResolverState _changes;
  friend class TimingEditResolverState;
  friend class TimingEditResolverView;
  friend class PreparedTimingEditResolverPatch;
};

// Short-lived borrowed lookup during capture/preparation; never stored across apply.
class TimingEditResolverView final
{
 public:
  TimingEditResolverView(const TimingEditResolverState& base) noexcept : _base(base) {}
  TimingEditResolverView(const TimingEditResolverState& base, const TimingEditResolverDelta& delta) noexcept
      : _base(base), _delta(&delta) {}
  [[nodiscard]] const NativeSlotBinding* findNodeBinding(const StableNodeRef& ref) const;
  [[nodiscard]] const NodeBindingEntry* findNodeEntryForKey(const csr::NodeKey& key) const;
  [[nodiscard]] const NetBindingEntry* findNetEntryForName(const std::string& name) const;
  [[nodiscard]] const NativeSlotBinding* findNetBinding(const StableNetRef& ref) const;
  [[nodiscard]] const NativeSlotBinding* findArcBinding(const StableArcRef& ref) const;
  [[nodiscard]] const std::string& resolveNode(const StableNodeRef& ref) const;
  [[nodiscard]] const std::string& resolveNet(const StableNetRef& ref) const;
  [[nodiscard]] AppendOnlySlot resolveArcSlot(const StableArcRef& ref) const;
  [[nodiscard]] const TokenMaterialization* findMaterialization(const CreationToken& token) const;
  [[nodiscard]] const TokenMaterialization& materialization(const CreationToken& token) const;
  [[nodiscard]] const AppendOnlyHighWater& highWater() const noexcept
  { return _delta ? _delta->highWater() : _base.highWater(); }

 private:
  [[nodiscard]] const NodeBindingEntry* findNodeEntry(const StableNodeRef& ref) const;
  [[nodiscard]] const NetBindingEntry* findNetEntry(const StableNetRef& ref) const;
  const TimingEditResolverState& _base;
  const TimingEditResolverDelta* _delta{};
};

class PreparedTimingEditResolverPatch final
{
 public:
  PreparedTimingEditResolverPatch(TimingEditResolverState& state, TimingEditResolverDelta delta);
  PreparedTimingEditResolverPatch(PreparedTimingEditResolverPatch&&) noexcept;
  PreparedTimingEditResolverPatch(const PreparedTimingEditResolverPatch&) = delete;
  PreparedTimingEditResolverPatch& operator=(const PreparedTimingEditResolverPatch&) = delete;
  ~PreparedTimingEditResolverPatch();
  [[nodiscard]] TimingEditResolverView view() const;
  void apply() noexcept;
  void rollback() noexcept;
  void commit() noexcept;

 private:
  struct Data;
  std::unique_ptr<Data> _data;
};

}  // namespace ista::timing_edit
