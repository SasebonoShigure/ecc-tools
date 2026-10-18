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
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrReportValues.hpp"
#include "TimingPoint.hpp"
#include "TimingResultScatter.hpp"

namespace ista::timing_dual_csr { struct DualCsrState; }
namespace ista::timing_edit { class TimingEditResolverState; }

namespace ista::csr::detail {

using NativeLaneDoubleMap = std::map<ista::AnalysisType, std::map<ista::TransType, double>>;
using NativeLaneStringMap = std::map<ista::AnalysisType, std::map<ista::TransType, std::string>>;
using NativeLaneTransitionMap = std::map<ista::AnalysisType, std::map<ista::TransType, ista::TransType>>;
using NativeArcIoDelayMap = std::map<ista::AnalysisType, std::map<ista::TransType, std::map<ista::TransType, double>>>;
using NativeArcTransitionMap = std::map<ista::TransType, ista::TransType>;
using NativePathStateMap
    = std::map<ista::AnalysisType, std::map<ista::PathSourceType, std::map<ista::TransType, std::map<std::string, ista::TimingPathState>>>>;

struct ResolvedNodeTarget
{
  ista::TimingPoint* point{nullptr};
  const std::string* full_name{nullptr};
};

struct ResolvedArcTarget
{
  // Null means an inactive stable ArcId, never a valid predecessor.
  ista::Arc* arc{nullptr};
  std::size_t native_rank{0};
};

struct StableScatterTargets
{
  std::vector<ResolvedNodeTarget> nodes;
  std::vector<ResolvedArcTarget> arcs;
  std::vector<std::string>* timing_order{nullptr};
};

struct TimingScatterLayout
{
  const std::array<AnalysisMode, kAnalysisCount>& analysis_order;
  const std::array<PathSource, kPathSourceCount>& path_source_order;
  const std::array<Transition, kTransitionCount>& transition_order;
  const std::vector<std::string>& clock_tags;
};

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

// Query-scoped report reader. Borrowed mappings/results stay under one operation guard.
// Only consumed nodes acquire native path maps; the authoritative Database is untouched.
class CsrReportNodeReader final
{
 public:
  CsrReportNodeReader(Database& database, const timing_dual_csr::DualCsrState& graph,
      const timing_edit::TimingEditResolverState& resolver, const CsrReportValues& values,
      std::span<const std::string> clock_names, PrepareCsrReport prepare);
  CsrReportNodeReader(const CsrReportNodeReader&) = delete;
  CsrReportNodeReader& operator=(const CsrReportNodeReader&) = delete;
  [[nodiscard]] bool contains(const std::string& name) const;
  [[nodiscard]] NodeId nodeId(const std::string& name) const;
  [[nodiscard]] ClockTagId clockTag(std::string_view name) const;
  void prepare(const CsrReportRequest& request);
  void preparePathTrace();
  [[nodiscard]] ista::TimingPoint& point(const std::string& name);

 private:
  Database& _database;
  const timing_dual_csr::DualCsrState& _graph;
  const timing_edit::TimingEditResolverState& _resolver;
  const CsrReportValues& _values;
  std::span<const std::string> _clock_names;
  PrepareCsrReport _prepare;
  std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> _path_slots;
  std::map<std::size_t, ista::TimingPoint> _points;
  std::map<std::uint64_t, std::size_t> _arc_ranks;
  bool _path_trace{false};
  [[nodiscard]] std::optional<NodeId> findNode(const std::string& name) const;
  [[nodiscard]] const std::string& nodeName(std::uint64_t id) const;
  [[nodiscard]] std::size_t arcRank(std::uint64_t id);
  void appendPath(PreparedTimingPointState& prepared, std::uint64_t row, std::uint64_t tag,
                  const value_detail::PathState& value);
  void decodeComplete(std::size_t id, PreparedTimingPointState& prepared);
  void decodeSparse(std::size_t id, PreparedTimingPointState& prepared);
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

// Internal values-only boundary. Caller owns stable mappings and holds the
// operation guard; all candidate container growth must precede target lookup.
[[nodiscard]] TimingResultPreparationResult prepareStableTimingResult(
    Database& database, const std::vector<std::string>& node_names,
    const std::vector<std::optional<std::size_t>>& arc_native_ranks,
    const TimingScatterLayout& layout, const CsrTimingValues& values);

// Resolve current native targets only within the caller's guarded query.
[[nodiscard]] StableScatterTargets resolveDualCsrTimingTargets(
    Database& database, const timing_dual_csr::DualCsrState& graph);

[[nodiscard]] PreparedTimingResult prepareDualCsrTimingResult(
    Database& database, const timing_dual_csr::DualCsrState& graph, const CsrTimingValues& values);

[[nodiscard]] TimingResultPreparationResult prepareResolvedTimingResult(const StableScatterTargets& targets, const TimingScatterLayout& layout,
                                                                        const CsrTimingValues& values);

[[nodiscard]] TimingResultPreparationResult prepareTimingResultScatter(Database& database, const CsrReferenceInput& committed_input,
                                                                       const CsrReferenceInput& current_input, const CsrTimingResult& result);
[[nodiscard]] PrepublishScatterResult prepareScatterAttempt(Database& database, const CsrReferenceInput& committed_input, const CsrTimingResult& result,
                                                            const StaAnalysisContext& current_context, std::uint64_t active_session_token,
                                                            std::uint64_t active_graph_generation);

// Internal computed-result path: the caller must hold one operation guard
// across export, checked computation and publication, with unchanged Database.
// External or test-injected results must use scatterCsrTimingResult instead.
[[nodiscard]] CsrScatterCode publishComputedTimingResult(Database& database, const CsrReferenceInput& input, const CsrTimingResult& result,
                                                         std::uint64_t session_token);

void publishPreparedTimingPoint(PreparedTimingPointState& prepared) noexcept;
void publishPreparedTimingResult(PreparedTimingResult& prepared) noexcept;

}  // namespace ista::csr::detail
