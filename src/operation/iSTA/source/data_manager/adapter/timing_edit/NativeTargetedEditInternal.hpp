// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#pragma once
#include <algorithm>
#include <stdexcept>
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "NativeTargetedEditPreparer.hpp"
namespace ista::timing_edit::detail {
template <class T>
void reserveTargetedAppend(std::vector<T>& values, std::size_t required)
{
  if (required <= values.capacity())
    return;
  if (required > values.max_size())
    throw std::length_error("targeted append exceeds vector capacity");
  const auto doubled = values.capacity() <= values.max_size() / 2 ? values.capacity() * 2 : values.max_size();
  values.reserve(std::max(required, doubled));
}
struct PreparedNativeTargetedMetadata
{
  PreparedNativeTargetedMetadata(NativeTargetedEditState& state, const AppendOnlySlotPlan& slots,
                                const std::vector<TokenMaterialization>& materializations);
  [[nodiscard]] TimingEditResolverView resolver() const { return _resolver_patch.view(); }
  [[nodiscard]] const std::string& nodeName(std::size_t id) const
  { return id < _node_count ? _state.node_names.at(id) : _names.at(id - _node_count); }
  void apply() noexcept;
  void undo() noexcept;
  void commit() noexcept { _resolver_patch.commit(); }

 private:
  NativeTargetedEditState& _state;
  PreparedTimingEditResolverPatch _resolver_patch;
  TransactionGeneration _generation;
  std::size_t _node_count, _arc_count, _native_count, _out_count, _in_count;
  std::vector<std::string> _names;
  std::vector<std::pair<std::size_t, StableArcRef>> _new_arc_refs;
  std::map<std::string, csr::NodeId> _node_ids;
  std::vector<std::map<std::string, csr::NodeId>::iterator> _ids;
  std::vector<std::pair<std::size_t, csr::ArcId>> _old_native_ids;
  std::vector<std::pair<std::size_t, std::optional<std::size_t>>> _old_ranks;
  bool _applied{false};
};
csr::NodeRecord targetedNode(const Database& database, const NativeTargetedEditState& state, const std::string& name);
csr::ArcSemanticPayload targetedArc(const Database& database, const NativeTargetedEditState& state, std::size_t rank,
                                    std::map<std::string, csr::ArcOutputLoadMatrix>& loads);
std::vector<csr::ArcId> targetedRow(const Database& database, const NativeTargetedEditState& state, csr::NodeId node, bool incoming);
}  // namespace ista::timing_edit::detail
