// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTimingPayloadViewImpl.hpp"

namespace ista::timing_dual_csr {

const std::string& DualCsrTimingPayloadViewImpl::schemaVersion() const noexcept
{
  return _state->reference_payload.schema_version;
}

const csr::CsrSnapshotIdentity&
DualCsrTimingPayloadViewImpl::sourceIdentity() const noexcept
{
  return _state->reference_payload.source_identity;
}

std::size_t DualCsrTimingPayloadViewImpl::nodeCount() const noexcept
{
  return _state->nodes.size();
}

const std::vector<csr::NodeRecord>& DualCsrTimingPayloadViewImpl::nodeRecords() const noexcept
{
  return _state->nodes;
}

const std::vector<csr::ReferencedLibraryCellModel>& DualCsrTimingPayloadViewImpl::referencedLibraryCells() const noexcept
{
  return _state->reference_payload.referenced_library_cells;
}

const csr::InputPortRcThresholds& DualCsrTimingPayloadViewImpl::inputPortRcThresholds() const noexcept
{
  return _state->reference_payload.parasitics.input_port_thresholds;
}

const std::vector<csr::NodeId>&
DualCsrTimingPayloadViewImpl::orderedRootIds() const noexcept
{
  return _state->reference_payload.ordered_root_ids;
}

const std::vector<csr::NodeId>&
DualCsrTimingPayloadViewImpl::orderedStartIds() const noexcept
{
  return _state->reference_payload.ordered_start_ids;
}

const std::vector<csr::NodeId>&
DualCsrTimingPayloadViewImpl::orderedEndIds() const noexcept
{
  return _state->reference_payload.ordered_end_ids;
}

const std::vector<std::string>&
DualCsrTimingPayloadViewImpl::clockTags() const noexcept
{
  return _state->reference_payload.clock_tags;
}

const std::vector<csr::ClockConstraintModel>&
DualCsrTimingPayloadViewImpl::clocks() const noexcept
{
  return _state->reference_payload.clocks;
}

const std::vector<std::optional<std::string>>&
DualCsrTimingPayloadViewImpl::clockOwners() const noexcept
{
  return _state->reference_payload.clock_owner_by_node;
}

const std::vector<csr::StartSeedModel>&
DualCsrTimingPayloadViewImpl::startSeeds() const noexcept
{
  return _state->reference_payload.start_seeds;
}

const std::vector<csr::EndpointRequiredModel>&
DualCsrTimingPayloadViewImpl::endpointRequired() const noexcept
{
  return _state->reference_payload.endpoint_required;
}

const std::vector<csr::TimingCheckEndpointModel>& DualCsrTimingPayloadViewImpl::timingCheckEndpoints() const noexcept
{
  return _state->reference_payload.timing_check_endpoints;
}

const std::vector<csr::CsrReferenceUnsupportedReason>&
DualCsrTimingPayloadViewImpl::unsupportedReasons() const noexcept
{
  return _state->reference_payload.unsupported_reasons;
}

}  // namespace ista::timing_dual_csr
