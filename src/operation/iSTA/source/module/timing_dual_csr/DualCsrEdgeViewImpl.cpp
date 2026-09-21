// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrEdgeViewImpl.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace ista::timing_dual_csr {
namespace {

csr::EdgeVisitResult normalizedResult(csr::EdgeVisitResult result)
{
  if (result.control == csr::EdgeVisitControl::kError && !result.error)
    return csr::EdgeVisitResult::fail(csr::CsrViewError::kCallbackError);
  if (result.control != csr::EdgeVisitControl::kError && result.error)
    return csr::EdgeVisitResult::fail(csr::CsrViewError::kCallbackError);
  return result;
}

bool validOffsets(const std::vector<std::uint64_t>& offsets,
                  std::size_t rows, std::size_t entries) noexcept
{
  return rows < std::numeric_limits<std::size_t>::max()
         && offsets.size() == rows + 1 && !offsets.empty()
         && offsets.front() == 0
         && offsets.back() == entries;
}

}  // namespace

DualCsrEdgeViewImpl::DualCsrEdgeViewImpl(const DualCsrState& state)
    : _state(&state)
{
  const std::size_t nodes = _state->nodes.size();
  const auto active = _state->telemetry.logical_edges_after;
  _malformed = active > std::numeric_limits<std::size_t>::max()
               || !validOffsets(_state->base_out.row_offsets, nodes,
                                _state->base_out.physical_slots.size())
               || !validOffsets(_state->base_in.row_offsets, nodes,
                                _state->base_in.physical_slots.size())
               || _state->aux_out.source_to_aux_row.size() != nodes
               || _state->aux_out.row_arcs.size() != _state->aux_out.row_sources.size();
  if (!_malformed)
    _active_slot_count = static_cast<std::size_t>(active);
}

std::size_t DualCsrEdgeViewImpl::nodeCount() const noexcept
{
  return _state->nodes.size();
}

std::size_t DualCsrEdgeViewImpl::arcSlotCount() const noexcept
{
  return _state->arc_slots.size();
}

std::size_t DualCsrEdgeViewImpl::activeArcCount() const noexcept
{
  return _active_slot_count;
}

bool DualCsrEdgeViewImpl::isActive(csr::ArcId arc_id) const noexcept
{
  return arc_id.value() < _state->arc_slots.size()
         && _state->arc_slots[arc_id.value()].active;
}

csr::ExpectedRef<const csr::ArcSemanticPayload, csr::CsrViewError>
DualCsrEdgeViewImpl::arc(csr::ArcId arc_id) const noexcept
{
  if (arc_id.value() >= _state->arc_slots.size())
    return csr::ExpectedRef<const csr::ArcSemanticPayload,
                            csr::CsrViewError>::failure(
        csr::CsrViewError::kArcOutOfRange);
  if (!_state->arc_slots[arc_id.value()].active)
    return csr::ExpectedRef<const csr::ArcSemanticPayload,
                            csr::CsrViewError>::failure(
        csr::CsrViewError::kInactiveArc);
  return csr::ExpectedRef<const csr::ArcSemanticPayload,
                          csr::CsrViewError>::success(
      _state->arc_slots[arc_id.value()].payload);
}

csr::EdgeVisitResult DualCsrEdgeViewImpl::visitArc(
    csr::ArcId arc_id, csr::NodeId row, bool outgoing,
    csr::EdgeStorageKind storage,
    const csr::EdgeVisitor& visitor) const
{
  if (!isActive(arc_id))
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  const auto& payload = _state->arc_slots[arc_id.value()].payload;
  if ((outgoing && payload.source != row)
      || (!outgoing && payload.sink != row))
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  return normalizedResult(visitor({arc_id, storage}));
}

csr::EdgeVisitResult DualCsrEdgeViewImpl::visitBaseRow(
    const BaseCsrState& base, csr::NodeId row, bool outgoing,
    csr::EdgeStorageKind storage,
    const csr::EdgeVisitor& visitor) const
{
  const auto index = static_cast<std::size_t>(row.value());
  if (index + 1 >= base.row_offsets.size())
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  const std::uint64_t begin = base.row_offsets[index];
  const std::uint64_t end = base.row_offsets[index + 1];
  if (begin > end || end > base.physical_slots.size())
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  for (std::uint64_t slot = begin; slot < end; ++slot) {
    if (!base.physical_slots[slot])
      continue;
    const auto result = visitArc(*base.physical_slots[slot], row,
                                 outgoing, storage, visitor);
    if (result.control != csr::EdgeVisitControl::kContinue)
      return result;
  }
  return csr::EdgeVisitResult::continueVisit();
}

csr::EdgeVisitResult DualCsrEdgeViewImpl::visitAuxRow(
    std::size_t row, const csr::EdgeVisitor& visitor) const
{
  const auto& aux = _state->aux_out;
  if (row >= aux.row_sources.size() || row >= aux.row_arcs.size())
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  const csr::NodeId source = aux.row_sources[row];
  if (source.value() >= nodeCount()
      || aux.source_to_aux_row[source.value()]
             != static_cast<std::int64_t>(row))
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  for (const auto arc : aux.row_arcs[row]) {
    const auto result = visitArc(arc, source, true, csr::EdgeStorageKind::kAuxOut, visitor);
    if (result.control != csr::EdgeVisitControl::kContinue)
      return result;
  }
  return csr::EdgeVisitResult::continueVisit();
}

csr::EdgeVisitResult DualCsrEdgeViewImpl::forEachActiveArc(
    const csr::EdgeVisitor& visitor) const
{
  if (!visitor)
    return csr::EdgeVisitResult::fail(csr::CsrViewError::kCallbackError);
  if (_malformed)
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  std::size_t visited = 0;
  const csr::EdgeVisitor counted = [&](const csr::EdgeEntry& entry) {
    if (visited == std::numeric_limits<std::size_t>::max())
      return csr::EdgeVisitResult::fail(
          csr::CsrViewError::kMalformedAdjacency);
    ++visited;
    return visitor(entry);
  };
  for (std::size_t row = 0; row < nodeCount(); ++row) {
    const auto result = visitBaseRow(
        _state->base_out, csr::NodeId{row}, true,
        csr::EdgeStorageKind::kBaseOut, counted);
    if (result.control != csr::EdgeVisitControl::kContinue)
      return result;
  }
  for (std::size_t row = 0; row < _state->aux_out.row_sources.size(); ++row) {
    const auto result = visitAuxRow(row, counted);
    if (result.control != csr::EdgeVisitControl::kContinue)
      return result;
  }
  return visited == _active_slot_count
             ? csr::EdgeVisitResult::continueVisit()
             : csr::EdgeVisitResult::fail(
                   csr::CsrViewError::kMalformedAdjacency);
}

csr::EdgeVisitResult DualCsrEdgeViewImpl::forEachOutgoing(
    csr::NodeId node, const csr::EdgeVisitor& visitor) const
{
  if (node.value() >= nodeCount())
    return csr::EdgeVisitResult::fail(csr::CsrViewError::kNodeOutOfRange);
  if (!visitor)
    return csr::EdgeVisitResult::fail(csr::CsrViewError::kCallbackError);
  if (_malformed)
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  auto result = visitBaseRow(_state->base_out, node, true,
                             csr::EdgeStorageKind::kBaseOut, visitor);
  if (result.control != csr::EdgeVisitControl::kContinue)
    return result;
  const std::int64_t mapped
      = _state->aux_out.source_to_aux_row[node.value()];
  if (mapped == -1)
    return result;
  if (mapped < 0
      || static_cast<std::uint64_t>(mapped)
             >= _state->aux_out.row_sources.size()
      || _state->aux_out.row_sources[static_cast<std::size_t>(mapped)]
             != node)
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  return visitAuxRow(static_cast<std::size_t>(mapped), visitor);
}

csr::EdgeVisitResult DualCsrEdgeViewImpl::forEachIncoming(
    csr::NodeId node, const csr::EdgeVisitor& visitor) const
{
  if (node.value() >= nodeCount())
    return csr::EdgeVisitResult::fail(csr::CsrViewError::kNodeOutOfRange);
  if (!visitor)
    return csr::EdgeVisitResult::fail(csr::CsrViewError::kCallbackError);
  if (_malformed)
    return csr::EdgeVisitResult::fail(
        csr::CsrViewError::kMalformedAdjacency);
  return visitBaseRow(_state->base_in, node, false,
                      csr::EdgeStorageKind::kBaseIn, visitor);
}

}  // namespace ista::timing_dual_csr
