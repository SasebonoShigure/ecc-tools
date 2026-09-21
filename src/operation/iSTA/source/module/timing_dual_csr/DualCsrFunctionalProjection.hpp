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

#include "DualCsrTimingResultValidator.hpp"

namespace ista::timing_dual_csr {

struct DualCsrProjectedNode
{
  csr::NodeKey key;
  csr::PinDirection direction{csr::PinDirection::kNone};
  std::string instance_cell;
  std::string net_name;
  bool is_port{false};
  bool is_startpoint{false};
  bool is_endpoint{false};
  bool is_sequential{false};
  bool is_clock_pin{false};
  bool is_data_pin{false};
  bool is_output_pin{false};

  friend bool operator==(const DualCsrProjectedNode&,
                         const DualCsrProjectedNode&) = default;
};

struct DualCsrProjectedArc
{
  csr::ArcKey key;
  csr::NodeKey source;
  csr::NodeKey sink;
  csr::ArcKind kind{csr::ArcKind::kNet};
  bool is_clock_arc{false};
  bool is_disabled{false};
  bool is_loop_disabled{false};
  csr::ArcEligibility eligibility;
  csr::CellTransferModel transfer;
  std::optional<csr::ArcOutputLoadMatrix> output_load;

  friend bool operator==(const DualCsrProjectedArc&,
                         const DualCsrProjectedArc&) = default;
};

struct DualCsrProjectedInstance
{
  std::string instance_name;
  std::string cell_name;
  std::vector<csr::NodeKey> ordered_pins;
  bool is_sequential{false};
  bool has_timing_checks{false};
  bool is_clock_gating{false};
  bool is_macro{false};

  friend bool operator==(const DualCsrProjectedInstance&,
                         const DualCsrProjectedInstance&) = default;
};

struct DualCsrProjectedRawNodeReference
{
  std::string full_name;
  std::optional<csr::NodeKey> resolved_node;

  friend bool operator==(const DualCsrProjectedRawNodeReference&,
                         const DualCsrProjectedRawNodeReference&) = default;
};

struct DualCsrProjectedClockConstraint
{
  std::string map_key;
  std::string embedded_clock_name;
  std::vector<DualCsrProjectedRawNodeReference> ordered_sources;
  double period{0.0};
  std::array<double, csr::kTransitionCount> edges{};
  bool propagated{false};

  friend bool operator==(const DualCsrProjectedClockConstraint&,
                         const DualCsrProjectedClockConstraint&) = default;
};

struct DualCsrProjectedPortConstraint
{
  std::string map_key;
  std::string embedded_port_name;
  std::optional<csr::NodeKey> resolved_node;
  std::array<std::optional<double>, csr::kAnalysisCount> input_delay;
  std::optional<double> input_transition;
  std::array<std::optional<double>, csr::kAnalysisCount> output_delay;
  std::optional<double> output_load;
  std::string clock_name;

  friend bool operator==(const DualCsrProjectedPortConstraint&,
                         const DualCsrProjectedPortConstraint&) = default;
};

struct DualCsrProjectedCaseAnalysis
{
  std::string raw_name;
  std::optional<csr::NodeKey> resolved_node;
  bool value{false};

  friend bool operator==(const DualCsrProjectedCaseAnalysis&,
                         const DualCsrProjectedCaseAnalysis&) = default;
};

struct DualCsrProjectedTimingCheckEndpoint
{
  std::string instance_name;
  std::uint64_t referenced_check_ordinal{0};
  csr::TimingCheckKind check_kind{csr::TimingCheckKind::kNone};
  std::string clock_pin_raw_name;
  std::string data_pin_raw_name;
  std::string clock_pin_full_name;
  std::string data_pin_full_name;
  std::optional<csr::NodeKey> resolved_clock_node;
  std::optional<csr::NodeKey> resolved_data_node;

  friend bool operator==(const DualCsrProjectedTimingCheckEndpoint&,
                         const DualCsrProjectedTimingCheckEndpoint&) = default;
};

struct DualCsrProjectedStartSeed
{
  csr::NodeKey seed_node;
  csr::NodeKey path_start_node;
  csr::StartSeedAvailability availability{
      csr::StartSeedAvailability::kMaterialized};
  std::optional<csr::PathSource> path_source;
  std::string clock_tag;
  csr::OptionalLaneMatrix arrival;
  csr::OptionalLaneMatrix slew;
  std::optional<csr::NodeKey> crpr_node;
  std::optional<csr::Transition> crpr_transition;

  friend bool operator==(const DualCsrProjectedStartSeed&,
                         const DualCsrProjectedStartSeed&) = default;
};

struct DualCsrProjectedEndpointRequired
{
  csr::NodeKey node;
  std::string clock_name;
  double clock_period{0.0};
  std::array<std::optional<double>, csr::kAnalysisCount> output_delay;

  friend bool operator==(const DualCsrProjectedEndpointRequired&,
                         const DualCsrProjectedEndpointRequired&) = default;
};

struct DualCsrProjectedCellPinCapacitance
{
  csr::NodeKey node;
  double default_capacitance{0.0};
  csr::OptionalLaneMatrix transition_capacitance;

  friend bool operator==(const DualCsrProjectedCellPinCapacitance&,
                         const DualCsrProjectedCellPinCapacitance&) = default;
};

struct DualCsrProjectedClockOwner
{
  csr::NodeKey node;
  std::optional<std::string> clock_name;

  friend bool operator==(const DualCsrProjectedClockOwner&,
                         const DualCsrProjectedClockOwner&) = default;
};

struct DualCsrProjectedReferencePayload
{
  std::string schema_version{csr::kCsrSchemaVersion};
  csr::StaAnalysisContext analysis_context;
  std::array<csr::AnalysisMode, csr::kAnalysisCount> analysis_order{
      csr::AnalysisMode::kMax, csr::AnalysisMode::kMin};
  std::array<csr::PathSource, csr::kPathSourceCount> path_source_order{
      csr::PathSource::kInput, csr::PathSource::kRegister};
  std::array<csr::Transition, csr::kTransitionCount> transition_order{
      csr::Transition::kRise, csr::Transition::kFall};
  std::vector<DualCsrProjectedInstance> instances;
  std::vector<csr::NodeKey> ordered_roots;
  std::vector<csr::NodeKey> ordered_starts;
  std::vector<csr::NodeKey> ordered_ends;
  std::vector<std::string> clock_tags;
  std::vector<DualCsrProjectedClockConstraint> clocks;
  std::vector<DualCsrProjectedClockOwner> clock_owners;
  std::vector<DualCsrProjectedPortConstraint> port_constraints;
  std::vector<DualCsrProjectedCaseAnalysis> case_analysis_constraints;
  std::vector<DualCsrProjectedTimingCheckEndpoint> timing_check_endpoints;
  std::vector<csr::ReferencedLibraryCellModel> referenced_library_cells;
  csr::ParasiticModel parasitics;
  std::vector<DualCsrProjectedStartSeed> start_seeds;
  std::vector<DualCsrProjectedEndpointRequired> endpoint_required;
  std::vector<DualCsrProjectedCellPinCapacitance> cell_pin_capacitances;
  std::vector<csr::CsrReferenceUnsupportedReason> unsupported_reasons;

  friend bool operator==(const DualCsrProjectedReferencePayload&,
                         const DualCsrProjectedReferencePayload&) = default;
};

struct DualCsrFunctionalProjection
{
  std::vector<DualCsrProjectedNode> nodes;
  std::vector<DualCsrProjectedArc> active_arcs;
  DualCsrProjectedReferencePayload reference_payload;

  friend bool operator==(const DualCsrFunctionalProjection&,
                         const DualCsrFunctionalProjection&) = default;
};

struct DualCsrProjectionAttempt
{
  std::optional<DualCsrFunctionalProjection> projection;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return projection.has_value() && violations.empty();
  }
};

[[nodiscard]] DualCsrProjectionAttempt projectDualCsrState(
    const DualCsrState& state);

struct DualCsrProjectedNodeTiming
{
  csr::OptionalSoa<std::uint8_t> is_clock_point;
  csr::OptionalSoa<double> arrivals;
  csr::OptionalSoa<double> requireds;
  csr::OptionalSoa<double> slacks;
  csr::OptionalSoa<double> launches;
  csr::OptionalSoa<std::string> clock_names;
  csr::OptionalSoa<csr::NodeKey> predecessor_nodes;
  csr::OptionalSoa<csr::ArcKey> predecessor_arcs;

  friend bool operator==(const DualCsrProjectedNodeTiming&,
                         const DualCsrProjectedNodeTiming&) = default;
};

struct DualCsrProjectedClockTiming
{
  csr::OptionalSoa<double> arrivals;
  csr::OptionalSoa<double> slews;
  csr::OptionalSoa<csr::NodeKey> predecessor_nodes;
  csr::OptionalSoa<double> predecessor_arc_delays;
  csr::OptionalSoa<csr::Transition> predecessor_transitions;

  friend bool operator==(const DualCsrProjectedClockTiming&,
                         const DualCsrProjectedClockTiming&) = default;
};

struct DualCsrProjectedPathTiming
{
  std::vector<std::uint64_t> row_offsets;
  csr::OptionalSoa<std::string> clock_tags;
  csr::OptionalSoa<double> arrivals;
  csr::OptionalSoa<double> slews;
  csr::OptionalSoa<double> launches;
  csr::OptionalSoa<csr::NodeKey> start_nodes;
  csr::OptionalSoa<csr::NodeKey> predecessor_nodes;
  csr::OptionalSoa<csr::ArcKey> predecessor_arcs;
  csr::OptionalSoa<double> predecessor_arc_delays;
  csr::OptionalSoa<csr::Transition> predecessor_transitions;
  csr::OptionalSoa<csr::NodeKey> crpr_nodes;
  csr::OptionalSoa<csr::Transition> crpr_transitions;

  friend bool operator==(const DualCsrProjectedPathTiming&,
                         const DualCsrProjectedPathTiming&) = default;
};

struct DualCsrProjectedArcTiming
{
  csr::OptionalSoa<double> delays;
  csr::OptionalSoa<double> min_delays;
  csr::OptionalSoa<double> max_delays;
  csr::OptionalSoa<double> transition_delays;
  csr::OptionalSoa<double> input_output_delays;
  csr::OptionalSoa<double> graph_delays;
  csr::OptionalSoa<csr::Transition> transition_types;
  std::vector<std::uint64_t> timing_arc_row_offsets;
  std::vector<std::int32_t> timing_arc_indices;
  csr::OptionalSoa<double> timing_arc_delays;
  csr::OptionalSoa<std::uint8_t> timing_arc_fallbacks;

  friend bool operator==(const DualCsrProjectedArcTiming&,
                         const DualCsrProjectedArcTiming&) = default;
};

struct DualCsrTimingSemanticProjection
{
  std::string schema_version{csr::kCsrSchemaVersion};
  std::vector<std::uint64_t> level_offsets;
  std::vector<csr::NodeKey> level_nodes;
  std::vector<std::uint64_t> node_to_level;
  DualCsrProjectedNodeTiming nodes;
  DualCsrProjectedClockTiming effective_clock;
  DualCsrProjectedClockTiming physical_clock;
  csr::OptionalSoa<double> data_slews;
  DualCsrProjectedPathTiming paths;
  DualCsrProjectedArcTiming arcs;

  friend bool operator==(const DualCsrTimingSemanticProjection&,
                         const DualCsrTimingSemanticProjection&) = default;
};

struct FunctionalSemanticReceipt
{
  DualCsrFunctionalProjection graph;
  DualCsrTimingSemanticProjection timing;

  friend bool operator==(const FunctionalSemanticReceipt&,
                         const FunctionalSemanticReceipt&) = default;
};

struct FunctionalSemanticReceiptAttempt
{
  std::optional<FunctionalSemanticReceipt> receipt;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return receipt.has_value() && violations.empty();
  }
};

[[nodiscard]] FunctionalSemanticReceiptAttempt
projectDualCsrFunctionalSemanticReceipt(
    const DualCsrState& state, const DualCsrTimingResult& timing);

}  // namespace ista::timing_dual_csr
