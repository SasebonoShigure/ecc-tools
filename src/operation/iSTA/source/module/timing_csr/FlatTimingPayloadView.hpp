#pragma once

#include "TimingPayloadView.hpp"

namespace ista::csr {

class FlatTimingPayloadView final : public TimingPayloadView
{
 public:
  explicit FlatTimingPayloadView(const CsrReferenceInput& input) noexcept : _input(&input) {}

  [[nodiscard]] const std::string& schemaVersion() const noexcept override { return _input->schema_version; }
  [[nodiscard]] const CsrSnapshotIdentity& sourceIdentity() const noexcept override { return _input->source_identity; }
  [[nodiscard]] std::size_t nodeCount() const noexcept override { return _input->nodes.size(); }
  [[nodiscard]] const std::vector<NodeRecord>& nodeRecords() const noexcept override { return _input->nodes; }
  [[nodiscard]] const std::vector<ReferencedLibraryCellModel>& referencedLibraryCells() const noexcept override { return _input->referenced_library_cells; }
  [[nodiscard]] const std::vector<NodeId>& orderedRootIds() const noexcept override { return _input->ordered_root_ids; }
  [[nodiscard]] const std::vector<NodeId>& orderedStartIds() const noexcept override { return _input->ordered_start_ids; }
  [[nodiscard]] const std::vector<NodeId>& orderedEndIds() const noexcept override { return _input->ordered_end_ids; }
  [[nodiscard]] const std::vector<std::string>& clockTags() const noexcept override { return _input->clock_tags; }
  [[nodiscard]] const std::vector<ClockConstraintModel>& clocks() const noexcept override { return _input->clocks; }
  [[nodiscard]] const std::vector<std::optional<std::string>>& clockOwners() const noexcept override { return _input->clock_owner_by_node; }
  [[nodiscard]] const std::vector<StartSeedModel>& startSeeds() const noexcept override { return _input->start_seeds; }
  [[nodiscard]] const std::vector<EndpointRequiredModel>& endpointRequired() const noexcept override { return _input->endpoint_required; }
  [[nodiscard]] const std::vector<TimingCheckEndpointModel>& timingCheckEndpoints() const noexcept override { return _input->timing_check_endpoints; }
  [[nodiscard]] const std::vector<CsrReferenceUnsupportedReason>& unsupportedReasons() const noexcept override { return _input->unsupported_reasons; }

 private:
  const CsrReferenceInput* _input;
};

}  // namespace ista::csr
