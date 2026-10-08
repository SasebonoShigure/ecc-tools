// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrState.hpp"
#include "TimingPayloadView.hpp"

namespace ista::timing_dual_csr {

class DualCsrTimingPayloadViewImpl final : public csr::TimingPayloadView
{
 public:
  explicit DualCsrTimingPayloadViewImpl(const DualCsrState& state) noexcept
      : _state(&state)
  {
  }

  [[nodiscard]] const std::string& schemaVersion() const noexcept override;
  [[nodiscard]] const csr::CsrSnapshotIdentity& sourceIdentity() const noexcept override;
  [[nodiscard]] std::size_t nodeCount() const noexcept override;
  [[nodiscard]] const std::vector<csr::NodeRecord>& nodeRecords() const noexcept override;
  [[nodiscard]] const std::vector<csr::ReferencedLibraryCellModel>& referencedLibraryCells() const noexcept override;
  [[nodiscard]] const csr::InputPortRcThresholds& inputPortRcThresholds() const noexcept override;
  [[nodiscard]] const std::vector<csr::NodeId>& orderedRootIds() const noexcept override;
  [[nodiscard]] const std::vector<csr::NodeId>& orderedStartIds() const noexcept override;
  [[nodiscard]] const std::vector<csr::NodeId>& orderedEndIds() const noexcept override;
  [[nodiscard]] const std::vector<std::string>& clockTags() const noexcept override;
  [[nodiscard]] const std::vector<csr::ClockConstraintModel>& clocks() const noexcept override;
  [[nodiscard]] const std::vector<std::optional<std::string>>& clockOwners() const noexcept override;
  [[nodiscard]] const std::vector<csr::StartSeedModel>& startSeeds() const noexcept override;
  [[nodiscard]] const std::vector<csr::EndpointRequiredModel>& endpointRequired() const noexcept override;
  [[nodiscard]] const std::vector<csr::TimingCheckEndpointModel>& timingCheckEndpoints() const noexcept override;
  [[nodiscard]] const std::vector<csr::CsrReferenceUnsupportedReason>& unsupportedReasons() const noexcept override;

 private:
  const DualCsrState* _state;
};

}  // namespace ista::timing_dual_csr
