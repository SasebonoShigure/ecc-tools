// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>

#include "DualCsrState.hpp"

namespace ista::timing_dual_csr {

class DualCsrEdgeViewImpl final : public csr::ActiveCsrEdgeView
{
 public:
  explicit DualCsrEdgeViewImpl(const DualCsrState& state);

  [[nodiscard]] std::size_t nodeCount() const noexcept override;
  [[nodiscard]] std::size_t arcSlotCount() const noexcept override;
  [[nodiscard]] std::size_t activeArcCount() const noexcept override;
  [[nodiscard]] bool isActive(csr::ArcId arc_id) const noexcept override;
  [[nodiscard]] csr::ExpectedRef<const csr::ArcSemanticPayload, csr::CsrViewError>
  arc(csr::ArcId arc_id) const noexcept override;
  [[nodiscard]] csr::EdgeVisitResult forEachActiveArc(
      const csr::EdgeVisitor& visitor) const override;
  [[nodiscard]] csr::EdgeVisitResult forEachOutgoing(
      csr::NodeId node, const csr::EdgeVisitor& visitor) const override;
  [[nodiscard]] csr::EdgeVisitResult forEachIncoming(
      csr::NodeId node, const csr::EdgeVisitor& visitor) const override;

 private:
  [[nodiscard]] csr::EdgeVisitResult visitArc(
      csr::ArcId arc_id, csr::NodeId row, bool outgoing,
      csr::EdgeStorageKind storage,
      const csr::EdgeVisitor& visitor) const;
  [[nodiscard]] csr::EdgeVisitResult visitBaseRow(
      const BaseCsrState& base, csr::NodeId row, bool outgoing,
      csr::EdgeStorageKind storage,
      const csr::EdgeVisitor& visitor) const;
  [[nodiscard]] csr::EdgeVisitResult visitAuxRow(
      std::size_t row, const csr::EdgeVisitor& visitor) const;

  const DualCsrState* _state;
  std::size_t _active_slot_count{0};
  bool _malformed{false};
};

}  // namespace ista::timing_dual_csr
