// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

class TimingPayloadView
{
 public:
  virtual ~TimingPayloadView() = default;

  [[nodiscard]] virtual const std::string& schemaVersion() const noexcept = 0;
  [[nodiscard]] virtual const CsrSnapshotIdentity& sourceIdentity() const noexcept = 0;
  [[nodiscard]] virtual std::size_t nodeCount() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<NodeRecord>& nodeRecords() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<ReferencedLibraryCellModel>& referencedLibraryCells() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<NodeId>& orderedRootIds() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<NodeId>& orderedStartIds() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<NodeId>& orderedEndIds() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<std::string>& clockTags() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<ClockConstraintModel>& clocks() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<std::optional<std::string>>& clockOwners() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<StartSeedModel>& startSeeds() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<EndpointRequiredModel>& endpointRequired() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<TimingCheckEndpointModel>& timingCheckEndpoints() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<CsrReferenceUnsupportedReason>& unsupportedReasons() const noexcept = 0;
};

}  // namespace ista::csr
