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
#include "CsrRealDifferentialScenario.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>

#include "ClockPropagator.hpp"
#include "DataManager.hpp"
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "IncrementalStaSession.hpp"
#include "STAInterface.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingPropagator.hpp"
#include "iSTA/source/module/timing_csr_snapshot/CsrSnapshotObservation.hpp"
namespace ista {
class StaCsrSessionProbeAccess
{
 public:
  static void use(std::uint64_t token, StaSessionState state, const std::function<void(Database&)>& callback)
  {
    StaStatus failure;
    auto guard = STAInterface::getInst().acquireUse(StaOwner::session(token), nullptr, false, state, &failure);
    if (!guard) {
      throw std::runtime_error("failed to acquire session operation guard: " + failure.message);
    }
    callback(STADM.getDatabase());
  }
};
}  // namespace ista
namespace ista::csr::real_diff {
namespace {
struct PreFullState
{
  TimingCellArc* member{nullptr};
  std::size_t library_size{0};
  Json receipt;
};
using canary_detail::active;
using canary_detail::fail;
bool arcMatches(const Database& database, const Arc& arc, const Json& selector)
{
  const std::string instance = selector.at("instance").get<std::string>();
  const auto found = database.get_instance_map().find(instance);
  return found != database.get_instance_map().end() && found->second.get_cell_name() == selector.at("cell") && arc.get_type() == ArcType::kCell
         && arc.get_source_pin() == instance + ":" + selector.at("source_pin").get<std::string>()
         && arc.get_sink_pin() == instance + ":" + selector.at("sink_pin").get<std::string>() && arc.get_owner_name() == selector.at("owner_name")
         && arc.get_library_source_port() == selector.at("library_source_port") && arc.get_library_sink_port() == selector.at("library_sink_port");
}
std::size_t uniqueArc(const Database& database, const Json& selector)
{
  std::size_t result = 0;
  std::size_t matches = 0;
  for (std::size_t rank = 0; rank < database.get_arc_list().size(); ++rank) {
    if (arcMatches(database, database.get_arc_list()[rank], selector)) {
      result = rank;
      ++matches;
    }
  }
  if (matches != 1) {
    fail("graph Arc selector is not unique");
  }
  return result;
}
PreFullState disableBeforeFull(Database& database, const ProbeOptions& options, std::uint64_t token)
{
  PreFullState state;
  Json descriptor = nullptr;
  std::size_t matches = 0;
  bool before = false;
  bool after = false;
  if (options.case_name == "supported_branches") {
    descriptor = options.descriptor.at("disabled_anchor");
    const std::string cell_name = descriptor.at("cell").get<std::string>();
    auto cell = database.get_timing_library().get_cell_map().find(cell_name);
    if (cell == database.get_timing_library().get_cell_map().end()) {
      fail("disabled library cell is absent");
    }
    auto& members = cell->second.get_cell_arc_list();
    state.library_size = members.size();
    for (TimingCellArc& member : members) {
      if (member.get_source_port() == descriptor.at("source_pin") && member.get_sink_port() == descriptor.at("sink_pin")) {
        state.member = &member;
        ++matches;
      }
    }
    if (matches != 1 || state.member == nullptr) {
      fail("disabled library member is not unique");
    }
    before = state.member->get_is_disable_arc();
    if (before) {
      fail("disabled library member was already disabled");
    }
    state.member->set_is_disable_arc(true);
    after = state.member->get_is_disable_arc();
    if (!after || members.size() != state.library_size) {
      fail("pre-full disable mutation contract differs");
    }
    for (const Arc& arc : database.get_arc_list()) {
      if (arcMatches(database, arc, descriptor)) {
        fail("bound graph Arc existed before production full");
      }
    }
  }
  state.receipt = {{"phase", "pre_full"},
                   {"state", "kInitialized"},
                   {"guard_owner", "session"},
                   {"token_nonzero", token != 0},
                   {"descriptor", descriptor},
                   {"library_match_count", matches},
                   {"library_size_before", state.library_size},
                   {"library_size_after", state.library_size},
                   {"member_address_stable", true},
                   {"disabled_before", options.case_name == "supported_branches" ? Json(before) : Json(nullptr)},
                   {"disabled_after", options.case_name == "supported_branches" ? Json(after) : Json(nullptr)},
                   {"bound_graph_arc_count", 0}};
  return state;
}
Json replayLevels(const Database& database)
{
  std::map<std::string, std::size_t> indegree;
  std::map<std::string, std::int32_t> levels;
  for (const auto& [name, point] : database.get_timing_point_map()) {
    levels[name] = 0;
    std::size_t count = 0;
    const auto incoming = database.get_incoming_arc_list_map().find(name);
    if (incoming != database.get_incoming_arc_list_map().end()) {
      for (std::size_t rank : incoming->second) {
        count += active(database.get_arc_list().at(rank));
      }
    }
    indegree[name] = count;
  }
  std::queue<std::string> queue;
  for (const auto& [name, count] : indegree) {
    if (count == 0) {
      levels[name] = 1;
      queue.push(name);
    }
  }
  std::vector<std::string> order;
  while (!queue.empty()) {
    const std::string source = queue.front();
    queue.pop();
    order.push_back(source);
    const auto outgoing = database.get_outgoing_arc_list_map().find(source);
    if (outgoing == database.get_outgoing_arc_list_map().end()) {
      continue;
    }
    for (std::size_t rank : outgoing->second) {
      const Arc& arc = database.get_arc_list().at(rank);
      if (!active(arc)) {
        continue;
      }
      levels[arc.get_sink_pin()] = std::max(levels[arc.get_sink_pin()], levels[source] + 1);
      if (--indegree.at(arc.get_sink_pin()) == 0) {
        queue.push(arc.get_sink_pin());
      }
    }
  }
  if (order.size() != database.get_timing_point_map().size()) {
    fail("independent active graph replay found a cycle");
  }
  Json level_rows = Json::array();
  for (const auto& [name, level] : levels) {
    level_rows.push_back({{"node", name}, {"level", level}});
  }
  return {{"order", order}, {"levels", std::move(level_rows)}};
}
Json nativeLevels(const Database& database)
{
  Json levels = Json::array();
  for (const auto& [name, point] : database.get_timing_point_map()) {
    levels.push_back({{"node", name}, {"level", point.get_level()}});
  }
  return {{"order", database.get_timing_order_list()}, {"levels", std::move(levels)}};
}
Json levelReceipt(const Json& native, const Json& replay)
{
  if (native != replay) {
    fail("independent FIFO levels/order differ from production full");
  }
  const std::string native_bytes = native.dump();
  const std::string replay_bytes = replay.dump();
  return {{"native_bytes_hex", bytesToHex(std::vector<std::uint8_t>(native_bytes.begin(), native_bytes.end()))},
          {"replay_bytes_hex", bytesToHex(std::vector<std::uint8_t>(replay_bytes.begin(), replay_bytes.end()))},
          {"equal", true}};
}
const TimingPathState* pathState(const TimingPoint& point, std::string_view tag)
{
  const auto& paths = point.get_path_state_map();
  if (!paths.contains(AnalysisType::kMax) || !paths.at(AnalysisType::kMax).contains(PathSourceType::kInput)
      || !paths.at(AnalysisType::kMax).at(PathSourceType::kInput).contains(TransType::kRise)) {
    return nullptr;
  }
  const auto& lane = paths.at(AnalysisType::kMax).at(PathSourceType::kInput).at(TransType::kRise);
  const auto state = lane.find(std::string(tag));
  return state == lane.end() ? nullptr : &state->second;
}
double nearCandidate(const Database& database, std::size_t rank, const TimingPathState& source)
{
  const auto& lanes = database.get_arc_list().at(rank).get_graph_delay_map();
  if (!lanes.contains(AnalysisType::kMax) || !lanes.at(AnalysisType::kMax).contains(TransType::kRise)
      || !lanes.at(AnalysisType::kMax).at(TransType::kRise).contains(TransType::kRise)) {
    fail("near-tie rise/rise max graph-delay lane is absent");
  }
  const double delay = lanes.at(AnalysisType::kMax).at(TransType::kRise).at(TransType::kRise);
  if (!std::isfinite(source.get_arrival()) || !std::isfinite(delay)) {
    fail("near-tie finite rise candidate inputs are absent");
  }
  return source.get_arrival() + delay;
}
bool activeReachable(const Database& database, std::string_view root, std::string_view target)
{
  std::map<std::string, bool> reached{{std::string(root), true}};
  std::queue<std::string> pending;
  pending.push(std::string(root));
  while (!pending.empty()) {
    const std::string source = pending.front();
    pending.pop();
    const auto row = database.get_outgoing_arc_list_map().find(source);
    if (row == database.get_outgoing_arc_list_map().end()) {
      continue;
    }
    for (std::size_t rank : row->second) {
      const Arc& arc = database.get_arc_list().at(rank);
      if (active(arc) && !reached[arc.get_sink_pin()]) {
        reached[arc.get_sink_pin()] = true;
        pending.push(arc.get_sink_pin());
      }
    }
  }
  return reached.contains(std::string(target));
}
Json verifyNearTie(const Database& database, const Json& descriptor)
{
  const Json& near = descriptor.at("near_tie");
  if (near.at("analysis") != "max" || near.at("path_source") != "input" || near.at("transition") != "rise") {
    fail("near-tie native lane selector differs");
  }
  const std::string join_name = near.at("join_node").at("full_name");
  const std::size_t first_rank = uniqueArc(database, near.at("row_first_arc"));
  const std::size_t second_rank = uniqueArc(database, near.at("row_second_arc"));
  const auto& row = database.get_incoming_arc_list_map().at(join_name);
  if (row.size() != 2 || row[0] != first_rank || row[1] != second_rank) {
    fail("near-tie actual incoming row differs");
  }
  const std::string tag = near.at("clock_tag");
  const std::string primary_root = descriptor.at("data_roots").at(0).at("full_name");
  const Arc& first = database.get_arc_list().at(first_rank);
  const Arc& second = database.get_arc_list().at(second_rank);
  const TimingPathState* first_source = pathState(database.get_timing_point_map().at(first.get_source_pin()), tag);
  const TimingPathState* second_source = pathState(database.get_timing_point_map().at(second.get_source_pin()), tag);
  if (!first_source || !second_source || first_source->get_start_point() != primary_root || second_source->get_start_point() != primary_root
      || first_source->get_clock_name() != tag || second_source->get_clock_name() != tag) {
    fail("near-tie candidates do not share the primary root/tag");
  }
  const double first_candidate = nearCandidate(database, first_rank, *first_source);
  const double second_candidate = nearCandidate(database, second_rank, *second_source);
  const double delta = std::abs(first_candidate - second_candidate);
  const double lower = near.at("lower_exclusive");
  const double upper = near.at("upper_exclusive");
  if (!(delta > lower && delta < upper)) {
    fail("near-tie actual candidate distance is outside frozen bounds");
  }
  const std::size_t strict_rank = second_candidate > first_candidate ? second_rank : first_rank;
  const std::size_t epsilon_rank = second_candidate > first_candidate + upper ? second_rank : first_rank;
  if (strict_rank == epsilon_rank) {
    fail("near-tie strict and epsilon winners do not differ");
  }
  const TimingPoint& join = database.get_timing_point_map().at(join_name);
  const TimingPathState* joined_path = pathState(join, tag);
  const Arc& strict_arc = database.get_arc_list().at(strict_rank);
  const Arc& epsilon_arc = database.get_arc_list().at(epsilon_rank);
  if (join.get_predecessor_arc_idx() != strict_rank || join.get_predecessor() != strict_arc.get_source_pin() || !joined_path
      || joined_path->get_predecessor_arc_idx() != epsilon_rank || joined_path->get_predecessor() != epsilon_arc.get_source_pin()) {
    fail("near-tie native scalar/path predecessor winners differ");
  }
  const std::string secondary_root = near.at("secondary_root").at("full_name");
  const std::string secondary_tag = near.at("secondary_clock_tag");
  if (!activeReachable(database, primary_root, join_name) || activeReachable(database, secondary_root, join_name) || pathState(join, secondary_tag)) {
    fail("near-tie secondary-root reachability exclusion differs");
  }
  Json tags = Json::array();
  for (std::size_t index = 0; index < descriptor.at("clock_tags").size(); ++index) {
    const std::string root = descriptor.at("data_roots").at(index).at("full_name");
    const std::string clock_tag = descriptor.at("clock_tags").at(index);
    const TimingPathState* root_state = pathState(database.get_timing_point_map().at(root), clock_tag);
    if (!root_state || root_state->get_start_point() != root || root_state->get_clock_name() != clock_tag) {
      fail("multi-root clock-tag path witness differs");
    }
    tags.push_back(clock_tag);
  }
  return tags;
}
Json coverageWitness(const Database& database, const ProbeOptions& options)
{
  if (options.case_name != "supported_branches") {
    return {{"chain", options.case_name == "chain"}, {"diamond", options.case_name == "diamond"}, {"extended", false}, {"complete", true}};
  }
  const Json& descriptor = options.descriptor;
  const Arc& non_unate = database.get_arc_list().at(uniqueArc(database, descriptor.at("non_unate_anchor")));
  bool non_unate_sense = false;
  for (const TimingArc& arc : non_unate.get_timing_cell_arc()->get_timing_arc_list()) {
    non_unate_sense |= arc.get_sense() == TimingArcSense::kNonUnate;
  }
  const auto max_lanes = non_unate.get_graph_delay_map().find(AnalysisType::kMax);
  if (!non_unate_sense || max_lanes == non_unate.get_graph_delay_map().end() || !max_lanes->second.contains(TransType::kRise)
      || !max_lanes->second.contains(TransType::kFall) || max_lanes->second.at(TransType::kRise).empty() || max_lanes->second.at(TransType::kFall).empty()) {
    fail("non-unate actual sense/transition lanes differ");
  }
  std::size_t loop_count = 0;
  const Json& loop = descriptor.at("loop_disabled_witness");
  for (const Arc& arc : database.get_arc_list()) {
    loop_count += arc.get_is_loop_disable() && arc.get_source_pin() == loop.at("source") && arc.get_sink_pin() == loop.at("sink")
                  && arc.get_owner_name() == loop.at("owner_name");
  }
  if (loop_count < loop.at("minimum_count").get<std::size_t>()) {
    fail("loop-disabled actual witness count differs");
  }
  return {{"chain", false},
          {"diamond", false},
          {"extended", true},
          {"non_unate", true},
          {"loop_disabled_count", loop_count},
          {"loop_disabled_minimum", loop.at("minimum_count")},
          {"multi_root", true},
          {"clock_tags", verifyNearTie(database, descriptor)},
          {"near_tie_descriptor", descriptor.at("near_tie")},
          {"complete", true}};
}
Json transformAfterFull(Database& database, const ProbeOptions& options, const PreFullState& pre)
{
  const Json saved_native = nativeLevels(database);
  Json disabled = {{"applicable", false}};
  Json parallel = {{"applicable", false}};
  if (options.case_name == "supported_branches") {
    const Json& disabled_selector = options.descriptor.at("disabled_anchor");
    const std::size_t disabled_rank = uniqueArc(database, disabled_selector);
    const Arc& disabled_arc = database.get_arc_list().at(disabled_rank);
    auto& members = database.get_timing_library().get_cell_map().at(disabled_selector.at("cell")).get_cell_arc_list();
    const TimingPoint& source = database.get_timing_point_map().at(disabled_arc.get_source_pin());
    const std::size_t uses = std::count_if(database.get_arc_list().begin(), database.get_arc_list().end(),
                                           [&](const Arc& arc) { return arc.get_timing_cell_arc() == pre.member; });
    const bool source_ok = !std::isfinite(source.get_arrival()) && source.get_data_slew_map().empty() && source.get_path_state_map().empty()
                           && std::isinf(source.get_required()) && source.get_required() > 0.0;
    const Json delay_receipt = scenario_detail::disabledDelayReceipt(disabled_arc);
    if (disabled_arc.get_timing_cell_arc() != pre.member || !pre.member->get_is_disable_arc() || !disabled_arc.get_is_disable_arc()
        || members.size() != pre.library_size || uses != 1 || !source_ok) {
      fail("post-full disabled inheritance witness differs");
    }
    disabled = {{"applicable", true},
                {"native_rank", disabled_rank},
                {"library_match_count", 1},
                {"graph_match_count", 1},
                {"member_use_count", uses},
                {"same_member", true},
                {"library_size_stable", true},
                {"member_disabled", true},
                {"graph_disabled", true},
                {"source_seed_and_candidate_absent", source_ok},
                {"complete", true}};
    disabled.update(delay_receipt);
    const Json& selector = options.descriptor.at("parallel_anchor");
    const std::size_t original_rank = uniqueArc(database, selector);
    const Arc& original = database.get_arc_list().at(original_rank);
    const std::string source_name = original.get_source_pin();
    const std::string sink_name = original.get_sink_pin();
    const TimingCellArc* parallel_member = original.get_timing_cell_arc();
    auto& outgoing = database.get_outgoing_arc_list_map().at(source_name);
    auto& incoming = database.get_incoming_arc_list_map().at(sink_name);
    const std::size_t incoming_before = incoming.size();
    if (!active(original) || parallel_member == nullptr || parallel_member == pre.member || outgoing.size() != 1 || outgoing.back() != original_rank) {
      fail("parallel pre-clone row-tail witness differs");
    }
    Arc clone_value = original;
    database.get_arc_list().push_back(std::move(clone_value));
    const std::size_t clone_rank = database.get_arc_list().size() - 1;
    outgoing.push_back(clone_rank);
    incoming.push_back(clone_rank);
    const Arc& clone = database.get_arc_list().at(clone_rank);
    if (!active(clone) || clone.get_timing_cell_arc() != parallel_member || outgoing.size() != 2 || outgoing[0] != original_rank || outgoing[1] != clone_rank
        || incoming.size() != incoming_before + 1) {
      fail("parallel append witness differs");
    }
    parallel = {{"applicable", true},        {"original_rank", original_rank},
                {"clone_rank", clone_rank},  {"pre_outgoing", Json::array({original_rank})},
                {"post_outgoing", outgoing}, {"same_member", true},
                {"active_count", 2},         {"disabled_count", 0},
                {"outgoing_added", 1},       {"incoming_added", 1},
                {"complete", true}};
  }
  const Json replay = replayLevels(database);
  return {{"disabled", std::move(disabled)},
          {"parallel", std::move(parallel)},
          {"level_order", levelReceipt(saved_native, replay)},
          {"coverage", coverageWitness(database, options)}};
}
void requireFullState(const Database& database, const ProbeOptions& options)
{
  std::vector<std::string> clocks;
  for (const auto& [name, clock] : database.get_timing_constraint().get_clock_map()) {
    clocks.push_back(name);
  }
  if (clocks != options.expected_clocks || database.get_pin_map().empty() || database.get_arc_list().empty() || database.get_end_point_list().empty()
      || database.get_timing_order_list().empty()) {
    fail("production full state or expected clocks differ");
  }
}
class ScopedDelayRun
{
 public:
  ScopedDelayRun() { DelayCalculator::getInst().init(); }
  ~ScopedDelayRun() { DelayCalculator::getInst().destroy(); }
};
template <typename Stage>
class ScopedNativeSingleton
{
 public:
  ScopedNativeSingleton() { Stage::initInst(); }
  ~ScopedNativeSingleton() { Stage::destroyInst(); }
};
}  // namespace
void runNativeExistingGraphTiming(Database&)
{
  ScopedDelayRun delay;
  {
    ScopedNativeSingleton<ClockPropagator> stage;
    ClockPropagator::getInst().propagate();
  }
  {
    ScopedNativeSingleton<TimingPropagator> stage;
    TimingPropagator::getInst().propagate();
  }
  {
    ScopedNativeSingleton<TimingAnalyzer> stage;
    TimingAnalyzer::getInst().analyze();
  }
}
ProbeExecution executeProbeScenario(const ProbeOptions& options, const DatabaseWork& work)
{
  loadProbeDesign(options);
  IncrementalStaSession session({StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError});
  throwOnStaFailure(session.initialize(probeAnalysisContext(), makeProbeConfig(options)), session.request());
  const auto token = session.sessionToken();
  if (session.state() != StaSessionState::kInitialized || !token || *token == 0) {
    fail("initialized session state/token differs");
  }
  PreFullState pre;
  StaCsrSessionProbeAccess::use(*token, StaSessionState::kInitialized, [&](Database& database) { pre = disableBeforeFull(database, options, *token); });
  const StaExecutionRecord record = requireStaExecutionRecord(session.runFullBaseline(), session.request());
  if (record.executed_backend != StaBackend::kCpu || record.executed_update_strategy != StaUpdateStrategy::kFull
      || record.execution_kind != StaExecutionKind::kFullUpdate || record.propagation_scope != StaPropagationScope::kFullGraph
      || record.incremental_batch_executed || record.fallback_to_full || record.fallback_reason || session.snapshotObservation()) {
    fail("production full execution record differs");
  }
  ProbeExecution execution;
  StaCsrSessionProbeAccess::use(*token, StaSessionState::kBaselineExecuted, [&](Database& database) {
    requireFullState(database, options);
    Json post = transformAfterFull(database, options, pre);
    runNativeExistingGraphTiming(database);
    execution.scenario = {{"schema", "ista-csr-real-differential-scenario/1.0.0"},
                          {"pre_full", pre.receipt},
                          {"disabled", post.at("disabled")},
                          {"parallel", post.at("parallel")},
                          {"level_order", post.at("level_order")},
                          {"coverage", post.at("coverage")},
                          {"stabilization",
                           {{"existing_graph_runs", 1},
                            {"stages", Json::array({"delay_runtime", "clock_propagator", "timing_propagator", "timing_analyzer"})},
                            {"complete", true}}}};
    execution.payload = work(database, *token, execution.scenario);
  });
  throwOnStaFailure(session.close(), session.request());
  STAInterface::destroyInst();
  return execution;
}
}  // namespace ista::csr::real_diff
