// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ista {
class Database;
}

namespace ista::csr::test {

enum class RawDirection : std::uint8_t
{
  kNone = 0,
  kInput = 1,
  kOutput = 2,
  kInout = 3
};

enum class RawArcKind : std::uint8_t
{
  kCell = 0,
  kNet = 1
};

enum class RawTransition : std::uint8_t
{
  kRise = 0,
  kFall = 1,
  kNone = 2
};

enum class RawTimingSense : std::uint8_t
{
  kNonePositiveFallback = 0,
  kPositiveUnate = 1,
  kNegativeUnate = 2,
  kNonUnate = 3
};

enum class RawTableVariable : std::uint8_t
{
  kNone = 0,
  kInputTransition = 1,
  kOutputCapacitance = 2,
  kConstrainedTransition = 3
};

enum class RawTimingCheckKind : std::uint8_t
{
  kNone = 0,
  kSetup = 1,
  kHold = 2,
  kRecovery = 3,
  kRemoval = 4,
  kWidth = 5,
  kPeriod = 6
};

struct RawNode
{
  std::string map_key;
  std::string full_name;
  bool is_port{false};
  RawDirection direction{RawDirection::kNone};
  std::string instance_name;

  friend bool operator==(const RawNode&, const RawNode&) = default;
};

struct RawSelectedClockArc
{
  std::string source;
  std::string sink;
  std::vector<RawTransition> ordered_child_triggers;

  friend bool operator==(const RawSelectedClockArc&, const RawSelectedClockArc&) = default;
};

struct RawInstance
{
  std::string map_key;
  std::string cell_name;
  bool sequential{false};
  bool gating{false};
  bool macro{false};
  std::string clock_pin_name;
  std::string data_pin_name;
  std::string output_pin_name;
  std::vector<std::string> ordered_pin_names;
  std::optional<RawSelectedClockArc> selected_clock_arc;

  friend bool operator==(const RawInstance&, const RawInstance&) = default;
};

struct RawArc
{
  std::uint64_t native_rank{0};
  std::string source;
  std::string sink;
  RawArcKind kind{RawArcKind::kNet};
  std::string owner;
  std::string library_source_port;
  std::string library_sink_port;
  bool is_clock_arc{false};
  bool disabled{false};
  bool loop_disabled{false};

  friend bool operator==(const RawArc&, const RawArc&) = default;
};

struct RawNet
{
  std::string map_key;
  std::string embedded_name;
  std::vector<std::string> ordered_drivers;
  std::vector<std::string> ordered_loads;

  friend bool operator==(const RawNet&, const RawNet&) = default;
};

struct RawClockConstraint
{
  std::string map_key;
  std::string embedded_name;
  std::vector<std::string> ordered_sources;
  double period{0.0};
  double rise_edge{0.0};
  double fall_edge{0.0};
  bool propagated{false};

  friend bool operator==(const RawClockConstraint&, const RawClockConstraint&) = default;
};

struct RawPortConstraint
{
  std::string map_key;
  std::string embedded_port_name;
  std::string clock_name;
  std::optional<double> input_delay_max;
  std::optional<double> input_delay_min;
  std::optional<double> input_transition;
  std::optional<double> output_delay_max;
  std::optional<double> output_delay_min;
  std::optional<double> output_load;

  friend bool operator==(const RawPortConstraint&, const RawPortConstraint&) = default;
};

struct RawCaseConstraint
{
  std::string map_key;
  std::string raw_name;
  bool value{false};

  friend bool operator==(const RawCaseConstraint&, const RawCaseConstraint&) = default;
};

struct RawTimingCheck
{
  std::string instance_name;
  std::string cell_name;
  std::uint64_t ordinal{0};
  RawTimingCheckKind kind{RawTimingCheckKind::kNone};
  std::string raw_clock_name;
  std::string full_clock_name;
  std::string raw_data_name;
  std::string full_data_name;

  friend bool operator==(const RawTimingCheck&, const RawTimingCheck&) = default;
};

struct RawTimingTable
{
  bool present{false};
  std::optional<RawTableVariable> variable1;
  std::optional<RawTableVariable> variable2;
  std::vector<double> axis1;
  std::vector<double> axis2;
  std::vector<double> values;

  friend bool operator==(const RawTimingTable&, const RawTimingTable&) = default;
};

struct RawLibraryPort
{
  std::string name;
  RawDirection direction{RawDirection::kNone};
  bool clock{false};
  bool output{false};
  double default_capacitance{0.0};
  std::array<std::array<std::optional<double>, 2>, 2> transition_capacitance;

  friend bool operator==(const RawLibraryPort&, const RawLibraryPort&) = default;
};

struct RawLibraryArcChild
{
  std::string library_name;
  std::int32_t index{0};
  RawTimingSense sense{RawTimingSense::kNonePositiveFallback};
  RawTransition trigger_transition{RawTransition::kNone};
  RawTransition check_transition{RawTransition::kNone};
  std::string sdf_condition;
  double time_scale{0.0};
  double capacitance_scale{0.0};
  double slew_derate{0.0};
  std::array<double, 2> slew_lower_thresholds{};
  std::array<double, 2> slew_upper_thresholds{};
  std::array<double, 2> input_thresholds{};
  std::array<double, 2> output_thresholds{};
  std::array<RawTimingTable, 2> delay_tables;
  std::array<RawTimingTable, 2> slew_tables;
  std::array<RawTimingTable, 2> check_tables;

  friend bool operator==(const RawLibraryArcChild&, const RawLibraryArcChild&) = default;
};

struct RawLibraryCellArc
{
  std::string source_port;
  std::string sink_port;
  double constant_delay{0.0};
  double delay_min{0.0};
  double delay_max{0.0};
  bool is_clock_arc{false};
  bool disabled{false};
  bool is_clear_or_preset{false};
  std::vector<RawLibraryArcChild> ordered_children;

  friend bool operator==(const RawLibraryCellArc&, const RawLibraryCellArc&) = default;
};

struct RawLibraryCheckArc
{
  std::string raw_clock_port;
  std::string raw_data_port;
  RawTimingCheckKind kind{RawTimingCheckKind::kNone};
  double check_time{0.0};
  RawTransition clock_transition{RawTransition::kNone};
  std::vector<RawLibraryArcChild> ordered_children;

  friend bool operator==(const RawLibraryCheckArc&, const RawLibraryCheckArc&) = default;
};

struct RawLibraryCellPayload
{
  bool sequential{false};
  bool gating{false};
  bool macro{false};
  std::string clock_pin_name;
  std::string data_pin_name;
  std::string output_pin_name;
  std::string capacitance_unit;
  std::string time_unit;
  double slew_derate{0.0};
  std::array<double, 2> slew_lower_thresholds{};
  std::array<double, 2> slew_upper_thresholds{};
  std::array<double, 2> input_thresholds{};
  std::array<double, 2> output_thresholds{};
  std::vector<RawLibraryPort> ports;
  std::vector<RawLibraryCellArc> cell_arcs;
  std::vector<RawLibraryCheckArc> check_arcs;

  friend bool operator==(const RawLibraryCellPayload&, const RawLibraryCellPayload&) = default;
};

struct RawReferencedLibraryCell
{
  std::string library_name;
  std::string cell_name;
  std::optional<RawLibraryCellPayload> payload;

  friend bool operator==(const RawReferencedLibraryCell&, const RawReferencedLibraryCell&) = default;
};

struct RawParasiticNode
{
  std::string map_key;
  std::string embedded_name;
  double capacitance{0.0};
  double x{0.0};
  double y{0.0};

  friend bool operator==(const RawParasiticNode&, const RawParasiticNode&) = default;
};

struct RawParasiticResistor
{
  std::string source;
  std::string sink;
  double resistance{0.0};

  friend bool operator==(const RawParasiticResistor&, const RawParasiticResistor&) = default;
};

struct RawParasiticNet
{
  std::string map_key;
  std::string embedded_name;
  double total_capacitance{0.0};
  std::vector<RawParasiticNode> nodes;
  std::vector<RawParasiticResistor> resistors;

  friend bool operator==(const RawParasiticNet&, const RawParasiticNet&) = default;
};

struct RawParasitics
{
  std::string source_path;
  std::string capacitance_unit;
  std::string resistance_unit;
  std::vector<RawParasiticNet> nets;

  friend bool operator==(const RawParasitics&, const RawParasitics&) = default;
};

struct CsrReferenceInputRawProjection
{
  std::vector<RawNode> nodes;
  std::vector<RawInstance> instances;
  std::vector<RawArc> arcs;
  std::vector<RawNet> nets;
  std::vector<RawClockConstraint> clock_constraints;
  std::vector<RawPortConstraint> port_constraints;
  std::vector<RawCaseConstraint> case_constraints;
  std::vector<RawTimingCheck> timing_checks;
  std::vector<RawReferencedLibraryCell> referenced_library_cells;
  std::optional<RawParasitics> parasitics;

  friend bool operator==(const CsrReferenceInputRawProjection&, const CsrReferenceInputRawProjection&) = default;
};

// This DTO is the sole raw allow-list. The split helpers are implemented by
// the four frozen projector translation units; production code must not call them.
[[nodiscard]] CsrReferenceInputRawProjection projectCsrReferenceInputRaw(const ista::Database& database);
void projectCsrReferenceInputRawConstraints(const ista::Database& database, CsrReferenceInputRawProjection& projection);
void projectCsrReferenceInputRawLibrary(const ista::Database& database, CsrReferenceInputRawProjection& projection);
void projectCsrReferenceInputRawParasitics(const ista::Database& database, CsrReferenceInputRawProjection& projection);

[[nodiscard]] std::vector<std::uint8_t> canonicalIndependentReferenceInputRawBytes(const CsrReferenceInputRawProjection& projection);

}  // namespace ista::csr::test
