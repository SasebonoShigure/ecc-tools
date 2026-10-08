// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualSemanticReadView.hpp"

#include <cstddef>

namespace ista::timing_dual_csr {
namespace {

struct NodeLookup
{
  const csr::NodeRecord* record{nullptr};
  std::optional<DualSemanticReadError> error;
};

struct ArcLookup
{
  const DualCsrArcSlot* slot{nullptr};
  csr::ArcId id;
  std::optional<DualSemanticReadError> error;
};

NodeLookup findNode(const DualCsrState& state,
                    const csr::NodeKey& key) noexcept
{
  if (state.high_water.node_slots != state.nodes.size())
    return {nullptr, DualSemanticReadError::kMalformedState};

  const csr::NodeRecord* match = nullptr;
  for (std::size_t index = 0; index < state.nodes.size(); ++index) {
    const auto& node = state.nodes[index];
    if (node.id.value() != index)
      return {nullptr, DualSemanticReadError::kMalformedState};
    if (node.key != key)
      continue;
    if (match != nullptr)
      return {nullptr, DualSemanticReadError::kMalformedState};
    match = &node;
  }
  if (match == nullptr)
    return {nullptr, DualSemanticReadError::kNodeNotFound};
  return {match, std::nullopt};
}

bool validArcPayload(const DualCsrState& state,
                     const csr::ArcSemanticPayload& payload) noexcept
{
  const std::uint64_t source = payload.source.value();
  const std::uint64_t sink = payload.sink.value();
  return source < state.nodes.size() && sink < state.nodes.size()
         && payload.kind == payload.key.base.kind
         && payload.key.base.source == state.nodes[source].key
         && payload.key.base.sink == state.nodes[sink].key;
}

ArcLookup findArc(const DualCsrState& state,
                  const csr::ArcKey& key) noexcept
{
  if (state.high_water.node_slots != state.nodes.size()
      || state.high_water.arc_slots != state.arc_slots.size())
    return {nullptr, csr::ArcId{}, DualSemanticReadError::kMalformedState};

  const DualCsrArcSlot* match = nullptr;
  csr::ArcId match_id;
  for (std::size_t index = 0; index < state.arc_slots.size(); ++index) {
    const auto& slot = state.arc_slots[index];
    if (!validArcPayload(state, slot.payload))
      return {nullptr, csr::ArcId{}, DualSemanticReadError::kMalformedState};
    if (!slot.active)
      continue;
    if (slot.payload.key != key)
      continue;
    if (match != nullptr)
      return {nullptr, csr::ArcId{}, DualSemanticReadError::kMalformedState};
    match = &slot;
    match_id = csr::ArcId{static_cast<std::uint64_t>(index)};
  }
  if (match == nullptr)
    return {nullptr, csr::ArcId{}, DualSemanticReadError::kArcNotFound};
  return {match, match_id, std::nullopt};
}

}  // namespace

DualSemanticReadView::DualSemanticReadView(const DualCsrState& state) noexcept
    : _state(&state)
{
}

csr::ExpectedRef<const csr::NodeRecord, DualSemanticReadError>
DualSemanticReadView::nodeByKey(const csr::NodeKey& key) const noexcept
{
  const NodeLookup lookup = findNode(*_state, key);
  if (lookup.error)
    return csr::ExpectedRef<const csr::NodeRecord,
                            DualSemanticReadError>::failure(*lookup.error);
  return csr::ExpectedRef<const csr::NodeRecord,
                          DualSemanticReadError>::success(*lookup.record);
}

std::optional<csr::NodeId> DualSemanticReadView::nodeIdByKey(
    const csr::NodeKey& key) const noexcept
{
  const NodeLookup lookup = findNode(*_state, key);
  if (lookup.error)
    return std::nullopt;
  return lookup.record->id;
}

csr::ExpectedRef<const csr::NodeRecord, DualSemanticReadError>
DualSemanticReadView::nodeById(csr::NodeId id) const noexcept
{
  if (_state->high_water.node_slots != _state->nodes.size()
      || id.value() >= _state->nodes.size())
    return csr::ExpectedRef<const csr::NodeRecord,
                            DualSemanticReadError>::failure(
        id.value() >= _state->nodes.size()
            ? DualSemanticReadError::kNodeNotFound
            : DualSemanticReadError::kMalformedState);
  const auto& node = _state->nodes[id.value()];
  if (node.id != id)
    return csr::ExpectedRef<const csr::NodeRecord,
                            DualSemanticReadError>::failure(
        DualSemanticReadError::kMalformedState);
  return csr::ExpectedRef<const csr::NodeRecord,
                          DualSemanticReadError>::success(node);
}

csr::ExpectedRef<const csr::ArcSemanticPayload, DualSemanticReadError>
DualSemanticReadView::arcByKey(const csr::ArcKey& key) const noexcept
{
  const ArcLookup lookup = findArc(*_state, key);
  if (lookup.error)
    return csr::ExpectedRef<const csr::ArcSemanticPayload,
                            DualSemanticReadError>::failure(*lookup.error);
  return csr::ExpectedRef<const csr::ArcSemanticPayload,
                          DualSemanticReadError>::success(
      lookup.slot->payload);
}

csr::ExpectedRef<const csr::ArcSemanticPayload, DualSemanticReadError>
DualSemanticReadView::arcById(csr::ArcId id) const noexcept
{
  if (_state->high_water.node_slots != _state->nodes.size()
      || _state->high_water.arc_slots != _state->arc_slots.size())
    return csr::ExpectedRef<const csr::ArcSemanticPayload,
                            DualSemanticReadError>::failure(
        DualSemanticReadError::kMalformedState);
  if (id.value() >= _state->arc_slots.size())
    return csr::ExpectedRef<const csr::ArcSemanticPayload,
                            DualSemanticReadError>::failure(
        DualSemanticReadError::kArcNotFound);
  const auto& slot = _state->arc_slots[id.value()];
  if (!validArcPayload(*_state, slot.payload))
    return csr::ExpectedRef<const csr::ArcSemanticPayload,
                            DualSemanticReadError>::failure(
        DualSemanticReadError::kMalformedState);
  if (!slot.active)
    return csr::ExpectedRef<const csr::ArcSemanticPayload,
                            DualSemanticReadError>::failure(
        DualSemanticReadError::kInactiveArc);
  return csr::ExpectedRef<const csr::ArcSemanticPayload,
                          DualSemanticReadError>::success(slot.payload);
}

std::optional<csr::ArcId> DualSemanticReadView::arcIdByKey(
    const csr::ArcKey& key) const noexcept
{
  const ArcLookup lookup = findArc(*_state, key);
  if (lookup.error)
    return std::nullopt;
  return lookup.id;
}

const timing_edit::AppendOnlyHighWater& DualSemanticReadView::highWater()
    const noexcept
{
  return _state->high_water;
}

std::uint64_t DualSemanticReadView::nodeHighWater() const noexcept
{
  return _state->high_water.node_slots;
}

std::uint64_t DualSemanticReadView::netHighWater() const noexcept
{
  return _state->high_water.net_slots;
}

std::uint64_t DualSemanticReadView::arcHighWater() const noexcept
{
  return _state->high_water.arc_slots;
}

timing_edit::TimingEditSessionToken DualSemanticReadView::sessionToken()
    const noexcept
{
  return _state->session_token;
}

timing_edit::TransactionGeneration DualSemanticReadView::generation()
    const noexcept
{
  return _state->generation;
}

const DualCsrReferencePayload& DualSemanticReadView::referencePayload()
    const noexcept
{
  return _state->reference_payload;
}

std::optional<std::size_t> DualSemanticReadView::parasiticNetSlot(const std::string& name) const noexcept
{
  const auto found = _state->parasitic_net_slots.find(name);
  return found == _state->parasitic_net_slots.end() ? std::nullopt : std::optional<std::size_t>{found->second};
}

}  // namespace ista::timing_dual_csr
