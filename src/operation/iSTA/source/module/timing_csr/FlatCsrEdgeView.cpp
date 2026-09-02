#include "FlatCsrEdgeView.hpp"

namespace ista::csr {

namespace {

ArcSemanticPayload semanticPayload(const ArcRecord& arc)
{
  return {arc.key, arc.source, arc.sink, arc.kind, arc.is_clock_arc, arc.is_disabled, arc.is_loop_disabled, arc.eligibility, arc.transfer, arc.output_load};
}

EdgeVisitResult normalizedResult(EdgeVisitResult result)
{
  if (result.control == EdgeVisitControl::kError && !result.error)
    return EdgeVisitResult::fail(CsrViewError::kCallbackError);
  if (result.control != EdgeVisitControl::kError && result.error)
    return EdgeVisitResult::fail(CsrViewError::kCallbackError);
  return result;
}

}  // namespace

FlatCsrEdgeView::FlatCsrEdgeView(const CsrReferenceInput& input) : _input(&input)
{
  _payloads.reserve(input.arcs.size());
  for (const auto& arc_record : input.arcs)
    _payloads.push_back(semanticPayload(arc_record));
}

std::size_t FlatCsrEdgeView::nodeCount() const noexcept
{
  return _input->nodes.size();
}

std::size_t FlatCsrEdgeView::arcSlotCount() const noexcept
{
  return _payloads.size();
}

std::size_t FlatCsrEdgeView::activeArcCount() const noexcept
{
  return _payloads.size();
}

bool FlatCsrEdgeView::isActive(ArcId arc_id) const noexcept
{
  return arc_id.value() < _payloads.size();
}

ExpectedRef<const ArcSemanticPayload, CsrViewError> FlatCsrEdgeView::arc(ArcId arc_id) const noexcept
{
  if (arc_id.value() >= _payloads.size())
    return ExpectedRef<const ArcSemanticPayload, CsrViewError>::failure(CsrViewError::kArcOutOfRange);
  return ExpectedRef<const ArcSemanticPayload, CsrViewError>::success(_payloads[arc_id.value()]);
}

EdgeVisitResult FlatCsrEdgeView::forEachActiveArc(const EdgeVisitor& visitor) const
{
  if (!visitor)
    return EdgeVisitResult::fail(CsrViewError::kCallbackError);
  for (std::size_t index = 0; index < _payloads.size(); ++index) {
    const EdgeVisitResult result = normalizedResult(
        visitor(EdgeEntry{ArcId{index}, EdgeStorageKind::kBaseOut}));
    if (result.control != EdgeVisitControl::kContinue)
      return result;
  }
  return EdgeVisitResult::continueVisit();
}

EdgeVisitResult FlatCsrEdgeView::visitRow(const std::vector<std::uint64_t>& offsets, const std::vector<ArcId>& arc_ids, NodeId node,
                                          EdgeStorageKind kind, const EdgeVisitor& visitor) const
{
  if (!visitor)
    return EdgeVisitResult::fail(CsrViewError::kCallbackError);
  if (node.value() >= nodeCount())
    return EdgeVisitResult::fail(CsrViewError::kNodeOutOfRange);
  if (offsets.size() != nodeCount() + 1)
    return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
  const std::uint64_t begin = offsets[node.value()];
  const std::uint64_t end = offsets[node.value() + 1];
  if (begin > end || end > arc_ids.size())
    return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
  for (std::uint64_t offset = begin; offset < end; ++offset) {
    const ArcId arc_id = arc_ids[offset];
    if (!isActive(arc_id))
      return EdgeVisitResult::fail(CsrViewError::kArcOutOfRange);
    const ArcSemanticPayload& payload = _payloads[arc_id.value()];
    if ((kind == EdgeStorageKind::kBaseOut
         && payload.source != node)
        || (kind == EdgeStorageKind::kBaseIn
            && payload.sink != node)
        || kind == EdgeStorageKind::kAuxOut)
      return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
    const EdgeVisitResult result
        = normalizedResult(visitor(EdgeEntry{arc_id, kind}));
    if (result.control != EdgeVisitControl::kContinue)
      return result;
  }
  return EdgeVisitResult::continueVisit();
}

EdgeVisitResult FlatCsrEdgeView::forEachOutgoing(NodeId node, const EdgeVisitor& visitor) const
{
  return visitRow(_input->out_row_offsets, _input->out_arc_ids, node, EdgeStorageKind::kBaseOut, visitor);
}

EdgeVisitResult FlatCsrEdgeView::forEachIncoming(NodeId node, const EdgeVisitor& visitor) const
{
  return visitRow(_input->in_row_offsets, _input->in_arc_ids, node, EdgeStorageKind::kBaseIn, visitor);
}

}  // namespace ista::csr
