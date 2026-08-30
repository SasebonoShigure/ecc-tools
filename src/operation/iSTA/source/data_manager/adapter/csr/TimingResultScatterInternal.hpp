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

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "TimingPoint.hpp"
#include "TimingResultScatter.hpp"

namespace ista::csr::detail {

using NativeLaneDoubleMap = std::map<ista::AnalysisType, std::map<ista::TransType, double>>;
using NativeLaneStringMap = std::map<ista::AnalysisType, std::map<ista::TransType, std::string>>;
using NativeLaneTransitionMap = std::map<ista::AnalysisType, std::map<ista::TransType, ista::TransType>>;
using NativeArcIoDelayMap = std::map<ista::AnalysisType, std::map<ista::TransType, std::map<ista::TransType, double>>>;
using NativeArcTransitionMap = std::map<ista::TransType, ista::TransType>;
using NativePathStateMap
    = std::map<ista::AnalysisType, std::map<ista::PathSourceType, std::map<ista::TransType, std::map<std::string, ista::TimingPathState>>>>;

struct PreparedClockState
{
  NativeLaneDoubleMap arrivals;
  NativeLaneDoubleMap slews;
  NativeLaneStringMap predecessors;
  NativeLaneDoubleMap predecessor_arc_delays;
  NativeLaneTransitionMap predecessor_transitions;
};

struct PreparedTimingPointState
{
  ista::TimingPoint* target{nullptr};
  double arrival{0.0};
  double required{0.0};
  double slack{0.0};
  double launch{0.0};
  std::int32_t level{0};
  bool is_clock_point{false};
  std::string predecessor;
  std::string clock_name;
  std::size_t predecessor_arc_index{0};
  PreparedClockState effective_clock;
  PreparedClockState physical_clock;
  NativeLaneDoubleMap data_slews;
  NativePathStateMap path_states;
};

struct PreparedArcState
{
  ista::Arc* target{nullptr};
  double delay{0.0};
  double minimum_delay{0.0};
  double maximum_delay{0.0};
  NativeLaneDoubleMap transition_delays;
  NativeArcIoDelayMap input_output_delays;
  NativeArcIoDelayMap graph_delays;
  ista::TimingArcDelayState timing_arc_delays;
  NativeArcTransitionMap transition_types;
};

struct PreparedTimingResult
{
  std::vector<PreparedTimingPointState> nodes;
  std::vector<PreparedArcState> arcs;
  std::vector<std::string>* timing_order_target{nullptr};
  std::vector<std::string> timing_order;
};

struct TimingResultPreparationFailure
{
  CsrScatterCode code{CsrScatterCode::kIdMappingMismatch};
};

using TimingResultPreparationResult = std::variant<PreparedTimingResult, TimingResultPreparationFailure>;

struct PreparedScatterEnvelope
{
  PreparedTimingResult prepared;
  CsrCurrentFingerprints current_fingerprints;
};

using PrepublishScatterResult = std::variant<PreparedScatterEnvelope, CsrTimingResultScatterAttempt>;

[[nodiscard]] bool validTimingScatterGuard(const CsrReferenceInput& committed_input, std::uint64_t active_session_token, std::uint64_t active_graph_generation);
[[nodiscard]] CsrCurrentFingerprints currentFingerprints(const CsrSnapshotIdentity& identity);
[[nodiscard]] std::optional<CsrScatterCode> firstTimingResultScatterFailure(const CsrReferenceInput& committed_input, const CsrTimingResult& result);
[[nodiscard]] std::optional<std::uint64_t> timingScatterWriteCount(std::size_t node_count, std::size_t arc_count) noexcept;

[[nodiscard]] TimingResultPreparationResult prepareTimingResultScatter(Database& database, const CsrReferenceInput& committed_input,
                                                                       const CsrReferenceInput& current_input, const CsrTimingResult& result);
[[nodiscard]] PrepublishScatterResult prepareScatterAttempt(Database& database, const CsrReferenceInput& committed_input, const CsrTimingResult& result,
                                                            const StaAnalysisContext& current_context, std::uint64_t active_session_token,
                                                            std::uint64_t active_graph_generation);

void publishPreparedTimingResult(PreparedTimingResult& prepared) noexcept;

}  // namespace ista::csr::detail
