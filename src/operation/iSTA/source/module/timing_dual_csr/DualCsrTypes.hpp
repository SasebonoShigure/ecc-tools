// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "CsrReferenceTypes.hpp"
#include "TimingEditTypes.hpp"

namespace ista::timing_dual_csr {

inline constexpr auto kDualCsrDescriptorSchema = "ista-dual-csr-update/1.2.0";

enum class DualCsrViolationCode : std::uint8_t
{
  kSchema,
  kSession,
  kGeneration,
  kHighWater,
  kShape,
  kId,
  kKey,
  kConflict,
  kExpectedValue,
  kAdjacency,
  kTranspose,
  kCapacity,
  kTimingResult
};

struct DualCsrViolation
{
  DualCsrViolationCode code{DualCsrViolationCode::kShape};
  std::string field;
  std::string message;

  friend bool operator==(const DualCsrViolation&, const DualCsrViolation&) = default;
};

using DualCsrViolations = std::vector<DualCsrViolation>;

template <typename T>
struct ValueReplacement
{
  T expected;
  T replacement;

  friend bool operator==(const ValueReplacement&, const ValueReplacement&) = default;
};

struct DualCsrReferencePayload
{
  std::string schema_version{csr::kCsrSchemaVersion};
  csr::CsrSnapshotIdentity source_identity;
  csr::StaAnalysisContext analysis_context;
  std::array<csr::AnalysisMode, csr::kAnalysisCount> analysis_order{
      csr::AnalysisMode::kMax, csr::AnalysisMode::kMin};
  std::array<csr::PathSource, csr::kPathSourceCount> path_source_order{
      csr::PathSource::kInput, csr::PathSource::kRegister};
  std::array<csr::Transition, csr::kTransitionCount> transition_order{
      csr::Transition::kRise, csr::Transition::kFall};
  std::vector<csr::InstanceRecord> instances;
  std::vector<csr::NodeId> ordered_root_ids;
  std::vector<csr::NodeId> ordered_start_ids;
  std::vector<csr::NodeId> ordered_end_ids;
  std::vector<std::string> clock_tags;
  std::vector<csr::ClockConstraintModel> clocks;
  std::vector<std::optional<std::string>> clock_owner_by_node;
  std::vector<csr::PortConstraintModel> port_constraints;
  std::vector<csr::CaseAnalysisConstraintModel> case_analysis_constraints;
  std::vector<csr::TimingCheckEndpointModel> timing_check_endpoints;
  std::vector<csr::ReferencedLibraryCellModel> referenced_library_cells;
  csr::ParasiticModel parasitics;
  std::vector<csr::StartSeedModel> start_seeds;
  std::vector<csr::EndpointRequiredModel> endpoint_required;
  std::vector<csr::CellPinCapacitanceModel> cell_pin_capacitances;
  std::vector<csr::CsrReferenceUnsupportedReason> unsupported_reasons;

  friend bool operator==(const DualCsrReferencePayload&,
                         const DualCsrReferencePayload&) = default;
};

struct DualCsrReferencePayloadDelta
{
  std::optional<ValueReplacement<std::string>> schema_version;
  std::optional<ValueReplacement<csr::CsrSnapshotIdentity>> source_identity;
  std::optional<ValueReplacement<csr::StaAnalysisContext>> analysis_context;
  std::optional<ValueReplacement<decltype(DualCsrReferencePayload::analysis_order)>>
      analysis_order;
  std::optional<ValueReplacement<decltype(DualCsrReferencePayload::path_source_order)>>
      path_source_order;
  std::optional<ValueReplacement<decltype(DualCsrReferencePayload::transition_order)>>
      transition_order;
  std::optional<ValueReplacement<std::vector<csr::InstanceRecord>>> instances;
  std::optional<ValueReplacement<std::vector<csr::NodeId>>> ordered_root_ids;
  std::optional<ValueReplacement<std::vector<csr::NodeId>>> ordered_start_ids;
  std::optional<ValueReplacement<std::vector<csr::NodeId>>> ordered_end_ids;
  std::optional<ValueReplacement<std::vector<std::string>>> clock_tags;
  std::optional<ValueReplacement<std::vector<csr::ClockConstraintModel>>> clocks;
  std::optional<ValueReplacement<std::vector<std::optional<std::string>>>>
      clock_owner_by_node;
  std::optional<ValueReplacement<std::vector<csr::PortConstraintModel>>>
      port_constraints;
  std::optional<ValueReplacement<std::vector<csr::CaseAnalysisConstraintModel>>>
      case_analysis_constraints;
  std::optional<ValueReplacement<std::vector<csr::TimingCheckEndpointModel>>>
      timing_check_endpoints;
  std::optional<ValueReplacement<std::vector<csr::ReferencedLibraryCellModel>>>
      referenced_library_cells;
  std::optional<ValueReplacement<csr::ParasiticModel>> parasitics;
  std::optional<ValueReplacement<std::vector<csr::StartSeedModel>>> start_seeds;
  std::optional<ValueReplacement<std::vector<csr::EndpointRequiredModel>>>
      endpoint_required;
  std::optional<ValueReplacement<std::vector<csr::CellPinCapacitanceModel>>>
      cell_pin_capacitances;
  std::optional<ValueReplacement<std::vector<csr::CsrReferenceUnsupportedReason>>>
      unsupported_reasons;

  [[nodiscard]] bool empty() const noexcept;

  friend bool operator==(const DualCsrReferencePayloadDelta&,
                         const DualCsrReferencePayloadDelta&) = default;
};

[[nodiscard]] DualCsrReferencePayload makeDualCsrReferencePayload(
    const csr::CsrReferenceInput& input);

}  // namespace ista::timing_dual_csr
