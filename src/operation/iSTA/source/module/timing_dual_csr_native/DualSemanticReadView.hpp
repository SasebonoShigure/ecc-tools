// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>

#include "DualCsrState.hpp"

namespace ista::timing_dual_csr {

enum class DualSemanticReadError : std::uint8_t
{
  kNodeNotFound,
  kArcNotFound,
  kInactiveArc,
  kMalformedState
};

// A non-owning semantic view of one committed Dual-CSR generation. The caller
// must keep the state alive and must not mutate it for the lifetime of the view.
class DualSemanticReadView final
{
 public:
  explicit DualSemanticReadView(const DualCsrState& state) noexcept;

  [[nodiscard]] csr::ExpectedRef<const csr::NodeRecord,
                                 DualSemanticReadError>
  nodeByKey(const csr::NodeKey& key) const noexcept;
  [[nodiscard]] std::optional<csr::NodeId> nodeIdByKey(
      const csr::NodeKey& key) const noexcept;
  [[nodiscard]] csr::ExpectedRef<const csr::NodeRecord,
                                 DualSemanticReadError>
  nodeById(csr::NodeId id) const noexcept;

  [[nodiscard]] csr::ExpectedRef<const csr::ArcSemanticPayload,
                                 DualSemanticReadError>
  arcByKey(const csr::ArcKey& key) const noexcept;
  [[nodiscard]] csr::ExpectedRef<const csr::ArcSemanticPayload,
                                 DualSemanticReadError>
  arcById(csr::ArcId id) const noexcept;
  [[nodiscard]] std::optional<csr::ArcId> arcIdByKey(
      const csr::ArcKey& key) const noexcept;

  [[nodiscard]] const timing_edit::AppendOnlyHighWater& highWater()
      const noexcept;
  [[nodiscard]] std::uint64_t nodeHighWater() const noexcept;
  [[nodiscard]] std::uint64_t netHighWater() const noexcept;
  [[nodiscard]] std::uint64_t arcHighWater() const noexcept;
  [[nodiscard]] timing_edit::TimingEditSessionToken sessionToken()
      const noexcept;
  [[nodiscard]] timing_edit::TransactionGeneration generation()
      const noexcept;
  [[nodiscard]] std::optional<std::size_t> parasiticNetSlot(const std::string& name) const noexcept;
  [[nodiscard]] const DualCsrReferencePayload& referencePayload()
      const noexcept;

 private:
  const DualCsrState* _state;
};

}  // namespace ista::timing_dual_csr
