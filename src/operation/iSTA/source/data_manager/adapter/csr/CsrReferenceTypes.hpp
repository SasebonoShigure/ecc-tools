// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "CsrReferenceNativeModels.hpp"

namespace ista::csr {

// Extents count valid elements and allocated element slots, never bytes.
struct CsrBufferExtent
{
  std::uint64_t logical_size{0};
  std::uint64_t capacity{0};

  friend bool operator==(const CsrBufferExtent&, const CsrBufferExtent&) = default;
};

struct CsrCurrentFingerprints
{
  std::optional<std::string> topology_fingerprint;
  std::optional<std::string> clock_constraint_fingerprint;
  std::optional<std::string> library_fingerprint;
  std::optional<std::string> corner_fingerprint;
  std::optional<std::string> parasitic_fingerprint;
  std::optional<std::string> timing_input_fingerprint;

  friend bool operator==(const CsrCurrentFingerprints&, const CsrCurrentFingerprints&) = default;
};

struct CsrReferenceInput
{
  std::string schema_version{kCsrSchemaVersion};
  CsrSnapshotIdentity source_identity;
  StaAnalysisContext analysis_context;
  std::array<AnalysisMode, kAnalysisCount> analysis_order{AnalysisMode::kMax, AnalysisMode::kMin};
  std::array<PathSource, kPathSourceCount> path_source_order{PathSource::kInput, PathSource::kRegister};
  std::array<Transition, kTransitionCount> transition_order{Transition::kRise, Transition::kFall};
  std::vector<NodeRecord> nodes;
  std::vector<NodeIdLookupEntry> node_ids_by_key;
  std::vector<InstanceRecord> instances;
  std::vector<ArcRecord> arcs;
  std::vector<ArcIdLookupEntry> arc_ids_by_key;
  std::vector<std::uint64_t> out_row_offsets;
  std::vector<ArcId> out_arc_ids;
  std::vector<std::uint64_t> in_row_offsets;
  std::vector<ArcId> in_arc_ids;
  std::vector<NodeId> ordered_root_ids;
  std::vector<NodeId> ordered_start_ids;
  std::vector<NodeId> ordered_end_ids;
  std::vector<std::string> clock_tags;
  std::vector<ClockConstraintModel> clocks;
  std::vector<std::optional<std::string>> clock_owner_by_node;
  std::vector<PortConstraintModel> port_constraints;
  std::vector<CaseAnalysisConstraintModel> case_analysis_constraints;
  std::vector<TimingCheckEndpointModel> timing_check_endpoints;
  std::vector<ReferencedLibraryCellModel> referenced_library_cells;
  ParasiticModel parasitics;
  std::vector<StartSeedModel> start_seeds;
  std::vector<EndpointRequiredModel> endpoint_required;
  std::vector<CellPinCapacitanceModel> cell_pin_capacitances;
  std::vector<CsrReferenceUnsupportedReason> unsupported_reasons;
};

// Capture/computation use an empty error on success. Unsupported reasons are
// sorted and unique. Scatter success is the only code allowed nonzero writes.
struct CsrCaptureResult
{
  std::optional<CsrCaptureCode> error;

  friend bool operator==(const CsrCaptureResult&, const CsrCaptureResult&) = default;
};

struct CsrReferenceSupportResult
{
  std::vector<CsrReferenceUnsupportedReason> unsupported_reasons;

  friend bool operator==(const CsrReferenceSupportResult&, const CsrReferenceSupportResult&) = default;
};

// Every OptionalSoa has presence.size() == values.size(). Values at absent
// positions are canonical zero/default values, so missing and real zero differ.
template <typename T>
struct OptionalSoa
{
  std::vector<std::uint8_t> presence;
  std::vector<T> values;

  friend bool operator==(const OptionalSoa&, const OptionalSoa&) = default;
};

// Clock/data lanes are NodeId x Analysis(max,min) x Transition(rise,fall).
struct ClockResultSoa
{
  OptionalSoa<double> arrivals;
  OptionalSoa<double> slews;
  OptionalSoa<NodeId> predecessor_nodes;
  OptionalSoa<double> predecessor_arc_delays;
  OptionalSoa<Transition> predecessor_transitions;

  friend bool operator==(const ClockResultSoa&, const ClockResultSoa&) = default;
};

struct NodeResultSoa
{
  OptionalSoa<std::uint8_t> is_clock_point;
  OptionalSoa<double> arrivals;
  OptionalSoa<double> requireds;
  OptionalSoa<double> slacks;
  OptionalSoa<double> launches;
  OptionalSoa<std::string> clock_names;
  OptionalSoa<NodeId> predecessor_nodes;
  OptionalSoa<ArcId> predecessor_arcs;

  friend bool operator==(const NodeResultSoa&, const NodeResultSoa&) = default;
};

// Path lanes are exactly NodeId x Analysis(max,min) x
// PathSource(input,register) x Transition(rise,fall). The current transition is
// derived only from the lane and is deliberately not repeated per entry.
struct PathResultSoa
{
  std::vector<std::uint64_t> row_offsets;
  OptionalSoa<ClockTagId> clock_tags;
  OptionalSoa<double> arrivals;
  OptionalSoa<double> slews;
  OptionalSoa<double> launches;
  OptionalSoa<NodeId> start_nodes;
  OptionalSoa<NodeId> predecessor_nodes;
  OptionalSoa<ArcId> predecessor_arcs;
  OptionalSoa<double> predecessor_arc_delays;
  OptionalSoa<Transition> predecessor_transitions;
  OptionalSoa<NodeId> crpr_nodes;
  OptionalSoa<Transition> crpr_transitions;

  // clock_tags resolve the clock name through CsrReferenceInput::clock_tags;
  // the current transition is the lane transition. These fields through start_nodes are required (presence == 1) per entry;
  // predecessor and CRPR fields may be absent.
  friend bool operator==(const PathResultSoa&, const PathResultSoa&) = default;
};

struct ArcResultSoa
{
  // Per ArcId scalar state. All three scalar fields require presence == 1.
  OptionalSoa<double> delays;
  OptionalSoa<double> min_delays;
  OptionalSoa<double> max_delays;
  // _trans_delay_map: ArcId x Analysis x input Transition.
  OptionalSoa<double> transition_delays;
  // _input_output_delay_map and _graph_delay_map:
  // ArcId x Analysis x input Transition x output Transition.
  OptionalSoa<double> input_output_delays;
  OptionalSoa<double> graph_delays;
  // _trans_type_map: ArcId x input Transition -> output Transition.
  OptionalSoa<Transition> transition_types;
  // Per-Arc rows list unique signed native TimingArc::arc_idx keys.
  // Delay/fallback lanes are timing-arc entry x Analysis x input x output.
  std::vector<std::uint64_t> timing_arc_row_offsets;
  std::vector<std::int32_t> timing_arc_indices;
  OptionalSoa<double> timing_arc_delays;
  OptionalSoa<std::uint8_t> timing_arc_fallbacks;

  friend bool operator==(const ArcResultSoa&, const ArcResultSoa&) = default;
};

// Computed values without a snapshot identity. Internal device sessions bind
// these to their own session/generation before native publication.
struct CsrTimingValues
{
  std::vector<std::uint64_t> level_offsets;
  std::vector<NodeId> level_nodes;
  std::vector<std::uint64_t> node_to_level;
  NodeResultSoa nodes;
  ClockResultSoa effective_clock;
  ClockResultSoa physical_clock;
  OptionalSoa<double> data_slews;
  PathResultSoa paths;
  ArcResultSoa arcs;

  friend bool operator==(const CsrTimingValues&, const CsrTimingValues&) = default;
};

struct CsrTimingResult : CsrTimingValues
{
  std::string schema_version{kCsrSchemaVersion};
  CsrSnapshotIdentity source_identity;

  friend bool operator==(const CsrTimingResult&, const CsrTimingResult&) = default;
};

struct CsrComputationResult
{
  std::variant<CsrTimingResult, CsrReferenceSupportResult, CsrComputationCode> outcome{CsrComputationCode::kInvariantViolation};

  friend bool operator==(const CsrComputationResult&, const CsrComputationResult&) = default;
};

struct CsrScatterResult
{
  CsrScatterCode code{CsrScatterCode::kInvalidState};
  std::uint64_t writes_applied{0};
  std::optional<CsrInvalidationReason> invalidation_reason;

  friend bool operator==(const CsrScatterResult&, const CsrScatterResult&) = default;
};

}  // namespace ista::csr
