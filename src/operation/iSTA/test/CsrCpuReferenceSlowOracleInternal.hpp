// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "CsrCpuReferenceSlowOracle.hpp"

namespace ista::csr::test::detail {

enum class SlowOracleStatus : std::uint8_t
{
  kSuccess,
  kCycle,
  kInvariant,
  kNumeric
};

struct SlowOracleTableValue
{
  bool present{false};
  SlowOracleStatus status{SlowOracleStatus::kSuccess};
  double value{0.0};
};

struct SlowOracleLevelization
{
  bool complete{false};
  std::vector<std::uint64_t> level_offsets{0};
  std::vector<NodeId> level_nodes;
  std::vector<std::uint64_t> node_to_level;
};

struct SlowOraclePathEntry
{
  ClockTagId clock_tag;
  double arrival{0.0};
  double slew{0.0};
  double launch{0.0};
  NodeId start_node;
  std::optional<NodeId> predecessor_node;
  std::optional<ArcId> predecessor_arc;
  std::optional<double> predecessor_arc_delay;
  std::optional<Transition> predecessor_transition;
  std::optional<NodeId> crpr_node;
  std::optional<Transition> crpr_transition;
};

struct SlowOracleState
{
  CsrTimingResult result;
  std::vector<std::vector<SlowOraclePathEntry>> path_rows;
  std::vector<std::uint8_t> dynamic_cache_seen;
};

[[nodiscard]] std::size_t slowNodeLane(std::size_t node, std::size_t analysis, std::size_t transition);
[[nodiscard]] std::size_t slowPathRow(std::size_t node, std::size_t analysis, std::size_t source, std::size_t transition);
[[nodiscard]] std::size_t slowArcTransitionLane(std::size_t arc, std::size_t analysis, std::size_t input_transition);
[[nodiscard]] std::size_t slowArcIoLane(std::size_t arc, std::size_t analysis, std::size_t input_transition, std::size_t output_transition);
[[nodiscard]] std::size_t slowChildLane(std::size_t child, std::size_t analysis, std::size_t input_transition, std::size_t output_transition);

[[nodiscard]] SlowOracleTableValue slowOracleLookupTable(const TimingTableModel& table, double input_slew, double raw_output_load, double time_unit_scale,
                                                         double capacitance_unit_scale);

[[nodiscard]] SlowOracleStatus slowOracleInitialize(const CsrReferenceInput& input, const SlowOracleLevelization& levelization, SlowOracleState& state);
[[nodiscard]] SlowOracleStatus slowOracleInitializeArcs(const CsrReferenceInput& input, SlowOracleState& state);
[[nodiscard]] SlowOracleLevelization slowOracleLevelize(const CsrReferenceInput& input);
[[nodiscard]] SlowOracleStatus slowOraclePropagate(const CsrReferenceInput& input, SlowOracleState& state);

}  // namespace ista::csr::test::detail
