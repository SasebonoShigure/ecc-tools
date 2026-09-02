#pragma once

#include <vector>

#include "ActiveCsrEdgeView.hpp"
#include "CsrReferenceTypes.hpp"

namespace ista::csr {

class FlatCsrEdgeView final : public ActiveCsrEdgeView
{
 public:
  explicit FlatCsrEdgeView(const CsrReferenceInput& input);

  [[nodiscard]] std::size_t nodeCount() const noexcept override;
  [[nodiscard]] std::size_t arcSlotCount() const noexcept override;
  [[nodiscard]] std::size_t activeArcCount() const noexcept override;
  [[nodiscard]] bool isActive(ArcId arc_id) const noexcept override;
  [[nodiscard]] ExpectedRef<const ArcSemanticPayload, CsrViewError> arc(ArcId arc_id) const noexcept override;
  [[nodiscard]] EdgeVisitResult forEachActiveArc(const EdgeVisitor& visitor) const override;
  [[nodiscard]] EdgeVisitResult forEachOutgoing(NodeId node, const EdgeVisitor& visitor) const override;
  [[nodiscard]] EdgeVisitResult forEachIncoming(NodeId node, const EdgeVisitor& visitor) const override;

 private:
  [[nodiscard]] EdgeVisitResult visitRow(const std::vector<std::uint64_t>& offsets, const std::vector<ArcId>& arc_ids, NodeId node,
                                         EdgeStorageKind kind, const EdgeVisitor& visitor) const;

  const CsrReferenceInput* _input;
  std::vector<ArcSemanticPayload> _payloads;
};

}  // namespace ista::csr
