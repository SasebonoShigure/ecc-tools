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
#include <memory>
#include <tuple>
#include <string>
#include <vector>

#include "CsrReferencePrimitives.hpp"
#include "CsrParasiticModel.hpp"

namespace ista::csr {

struct TimingTableModel
{
  bool present{false};
  TableVariable variable1{TableVariable::kNone};
  TableVariable variable2{TableVariable::kNone};
  std::vector<std::vector<double>> axes;
  std::vector<double> values;

  friend bool operator==(const TimingTableModel&, const TimingTableModel&) = default;
};

struct ChildTimingArcModel
{
  LibraryArcSignature signature;
  double time_unit_scale{1.0};
  double capacitance_unit_scale{1.0};
  double slew_derate{1.0};
  std::array<double, kTransitionCount> slew_lower_threshold_pct{0.3, 0.3};
  std::array<double, kTransitionCount> slew_upper_threshold_pct{0.7, 0.7};
  std::array<double, kTransitionCount> input_threshold_pct{0.5, 0.5};
  std::array<double, kTransitionCount> output_threshold_pct{0.5, 0.5};
  std::array<TimingTableModel, kTransitionCount> delay_tables;
  std::array<TimingTableModel, kTransitionCount> slew_tables;
  std::array<TimingTableModel, kTransitionCount> check_tables;

  friend bool operator==(const ChildTimingArcModel&, const ChildTimingArcModel&) = default;
};

struct CellTransferModel
{
  double delay{1.0};
  double delay_min{1.0};
  double delay_max{1.0};
  bool is_clock_arc{false};
  bool is_disabled{false};
  bool is_clear_or_preset{false};
  [[nodiscard]] const std::vector<ChildTimingArcModel>& children() const noexcept
  {
    static const std::vector<ChildTimingArcModel> empty;
    return _children ? *_children : empty;
  }
  [[nodiscard]] const std::shared_ptr<const std::vector<ChildTimingArcModel>>& childrenOwner() const noexcept { return _children; }
  void setChildren(const std::vector<ChildTimingArcModel>& values)
  {
    _children = values.empty() ? nullptr : std::make_shared<const std::vector<ChildTimingArcModel>>(values);
  }
  friend bool operator==(const CellTransferModel& lhs, const CellTransferModel& rhs)
  {
    return std::tie(lhs.delay, lhs.delay_min, lhs.delay_max, lhs.is_clock_arc, lhs.is_disabled, lhs.is_clear_or_preset) == std::tie(rhs.delay, rhs.delay_min, rhs.delay_max, rhs.is_clock_arc, rhs.is_disabled, rhs.is_clear_or_preset)
        && (lhs._children == rhs._children || lhs.children() == rhs.children());
  }

 private:
  std::shared_ptr<const std::vector<ChildTimingArcModel>> _children;
};

struct ArcEligibility
{
  bool levelization{false};
  bool data_arrival{false};
  bool data_slew{false};
  bool clock{false};

  friend bool operator==(const ArcEligibility&, const ArcEligibility&) = default;
};

struct NodeRecord
{
  NodeId id;
  NodeKey key;
  PinDirection direction{PinDirection::kNone};
  std::string instance_cell;
  std::string net_name;
  bool is_port{false};
  bool is_startpoint{false};
  bool is_endpoint{false};
  bool is_sequential{false};
  bool is_clock_pin{false};
  bool is_data_pin{false};
  bool is_output_pin{false};

  friend bool operator==(const NodeRecord&, const NodeRecord&) = default;
};

struct InstanceRecord
{
  std::string instance_name;
  std::string cell_name;
  std::vector<NodeId> ordered_pins;
  bool is_sequential{false};
  bool has_timing_checks{false};
  bool is_clock_gating{false};
  bool is_macro{false};

  friend bool operator==(const InstanceRecord&, const InstanceRecord&) = default;
};

struct ArcRecord
{
  ArcId id;
  ArcKey key;
  NodeId source;
  NodeId sink;
  ArcKind kind{ArcKind::kNet};
  std::uint64_t native_traversal_rank{0};
  bool is_clock_arc{false};
  bool is_disabled{false};
  bool is_loop_disabled{false};
  ArcEligibility eligibility;
  CellTransferModel transfer;
  std::optional<ArcOutputLoadMatrix> output_load;
  // Index in the owning Liberty cell, distinct from child/table interning.
  std::optional<std::uint64_t> native_cell_arc_ordinal{std::nullopt};
  CsrParasiticBinding parasitic{};

  friend bool operator==(const ArcRecord&, const ArcRecord&) = default;
};

struct RawNodeReference
{
  std::string full_name;
  std::optional<NodeId> resolved_node;

  friend bool operator==(const RawNodeReference&, const RawNodeReference&) = default;
};

struct ClockConstraintModel
{
  std::string map_key;
  std::string embedded_clock_name;
  std::vector<RawNodeReference> ordered_sources;
  double period{0.0};
  std::array<double, kTransitionCount> edges{};
  bool propagated{false};

  friend bool operator==(const ClockConstraintModel&, const ClockConstraintModel&) = default;
};

struct PortConstraintModel
{
  std::string map_key;
  std::string embedded_port_name;
  std::optional<NodeId> resolved_node;
  std::array<std::optional<double>, kAnalysisCount> input_delay;
  std::optional<double> input_transition;
  std::array<std::optional<double>, kAnalysisCount> output_delay;
  std::optional<double> output_load;
  std::string clock_name;

  friend bool operator==(const PortConstraintModel&, const PortConstraintModel&) = default;
};

struct CaseAnalysisConstraintModel
{
  std::string raw_name;
  std::optional<NodeId> resolved_node;
  bool value{false};

  friend bool operator==(const CaseAnalysisConstraintModel&, const CaseAnalysisConstraintModel&) = default;
};

struct StartSeedModel
{
  NodeId seed_node;
  NodeId path_start_node;
  StartSeedAvailability availability{StartSeedAvailability::kMaterialized};
  std::optional<PathSource> path_source;
  std::string clock_tag;
  OptionalLaneMatrix arrival;
  OptionalLaneMatrix slew;
  std::optional<NodeId> crpr_node;
  std::optional<Transition> crpr_transition;
  // Q can have no graph arc; its deferred seed still owns the net model.
  CsrParasiticBinding parasitic{};
  std::optional<ArcOutputLoadMatrix> output_load;

  friend bool operator==(const StartSeedModel&, const StartSeedModel&) = default;
};

struct EndpointRequiredModel
{
  NodeId node;
  std::string clock_name;
  double clock_period{0.0};
  std::array<std::optional<double>, kAnalysisCount> output_delay;

  friend bool operator==(const EndpointRequiredModel&, const EndpointRequiredModel&) = default;
};

struct CellPinCapacitanceModel
{
  NodeId node;
  double default_capacitance{0.0};
  OptionalLaneMatrix transition_capacitance;

  friend bool operator==(const CellPinCapacitanceModel&, const CellPinCapacitanceModel&) = default;
};

struct TimingCheckEndpointModel
{
  std::string instance_name;
  std::uint64_t referenced_check_ordinal{0};
  TimingCheckKind check_kind{TimingCheckKind::kNone};
  std::string clock_pin_raw_name;
  std::string data_pin_raw_name;
  std::string clock_pin_full_name;
  std::string data_pin_full_name;
  std::optional<NodeId> resolved_clock_node;
  std::optional<NodeId> resolved_data_node;

  friend bool operator==(const TimingCheckEndpointModel&, const TimingCheckEndpointModel&) = default;
};

struct LibraryPortModel
{
  std::string name;
  PinDirection direction{PinDirection::kNone};
  bool is_clock{false};
  bool is_output{false};
  double default_capacitance{0.0};
  OptionalLaneMatrix transition_capacitance;

  friend bool operator==(const LibraryPortModel&, const LibraryPortModel&) = default;
};

struct LibraryCellArcModel
{
  std::string source_port;
  std::string sink_port;
  CellTransferModel transfer;

  friend bool operator==(const LibraryCellArcModel&, const LibraryCellArcModel&) = default;
};

struct LibraryTimingCheckArcModel
{
  std::string clock_port;
  std::string data_port;
  TimingCheckKind kind{TimingCheckKind::kNone};
  double check_time{0.0};
  Transition clock_transition{Transition::kRise};
  [[nodiscard]] const std::vector<ChildTimingArcModel>& children() const noexcept
  {
    static const std::vector<ChildTimingArcModel> empty;
    return _children ? *_children : empty;
  }
  [[nodiscard]] const std::shared_ptr<const std::vector<ChildTimingArcModel>>& childrenOwner() const noexcept { return _children; }
  void setChildren(const std::vector<ChildTimingArcModel>& values)
  {
    _children = values.empty() ? nullptr : std::make_shared<const std::vector<ChildTimingArcModel>>(values);
  }
  friend bool operator==(const LibraryTimingCheckArcModel& lhs, const LibraryTimingCheckArcModel& rhs)
  {
    return std::tie(lhs.clock_port, lhs.data_port, lhs.kind, lhs.check_time, lhs.clock_transition) == std::tie(rhs.clock_port, rhs.data_port, rhs.kind, rhs.check_time, rhs.clock_transition)
        && (lhs._children == rhs._children || lhs.children() == rhs.children());
  }

 private:
  std::shared_ptr<const std::vector<ChildTimingArcModel>> _children;
};

struct ReferencedLibraryCellModel
{
  std::string cell_name;
  bool present{false};
  std::string library_name;
  bool is_sequential{false};
  bool is_clock_gating{false};
  bool is_macro{false};
  std::string clock_pin_name;
  std::string data_pin_name;
  std::string output_pin_name;
  std::vector<LibraryPortModel> ports;
  std::vector<LibraryCellArcModel> cell_arcs;
  std::vector<LibraryTimingCheckArcModel> timing_checks;
  // Load-cell library thresholds, independent of its outgoing arc children.
  double slew_derate{1.0};
  std::array<double, kTransitionCount> slew_lower_threshold_pct{0.3, 0.3};
  std::array<double, kTransitionCount> slew_upper_threshold_pct{0.7, 0.7};
  std::array<double, kTransitionCount> input_threshold_pct{0.5, 0.5};

  friend bool operator==(const ReferencedLibraryCellModel&, const ReferencedLibraryCellModel&) = default;
};

struct ParasiticNodeModel
{
  std::string map_key;
  std::string embedded_name;
  double capacitance{0.0};
  double x{0.0};
  double y{0.0};

  friend bool operator==(const ParasiticNodeModel&, const ParasiticNodeModel&) = default;
};

struct ParasiticResistorModel
{
  std::string source;
  std::string sink;
  double resistance{0.0};

  friend bool operator==(const ParasiticResistorModel&, const ParasiticResistorModel&) = default;
};

struct ParasiticNetModel
{
  std::string map_key;
  std::string embedded_name;
  double lumped_capacitance{0.0};
  std::vector<ParasiticNodeModel> nodes;
  std::vector<ParasiticResistorModel> resistors;

  friend bool operator==(const ParasiticNetModel&, const ParasiticNetModel&) = default;
};

struct InputPortRcThresholds
{
  double slew_derate{1.0};
  std::array<double, kTransitionCount> lower{0.3, 0.3};
  std::array<double, kTransitionCount> upper{0.7, 0.7};
  friend bool operator==(const InputPortRcThresholds&, const InputPortRcThresholds&) = default;
};

struct ParasiticModel
{
  std::string capacitance_unit;
  std::string resistance_unit;
  std::vector<ParasiticNetModel> nets;
  InputPortRcThresholds input_port_thresholds{};

  friend bool operator==(const ParasiticModel&, const ParasiticModel&) = default;
};

}  // namespace ista::csr
