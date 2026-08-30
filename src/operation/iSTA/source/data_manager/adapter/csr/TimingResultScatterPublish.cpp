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
#include <type_traits>
#include <utility>

#include "CsrSessionSnapshotAdapterInternal.hpp"

namespace ista::csr::detail {
namespace {

static_assert(std::is_nothrow_swappable_v<std::unique_ptr<CsrSnapshotObservation>>);
static_assert(std::is_nothrow_swappable_v<std::string>);
static_assert(std::is_nothrow_swappable_v<NativeLaneDoubleMap>);
static_assert(std::is_nothrow_swappable_v<NativeLaneStringMap>);
static_assert(std::is_nothrow_swappable_v<NativeLaneTransitionMap>);
static_assert(std::is_nothrow_swappable_v<NativeArcIoDelayMap>);
static_assert(std::is_nothrow_swappable_v<NativeArcTransitionMap>);
static_assert(std::is_nothrow_swappable_v<NativePathStateMap>);
static_assert(std::is_nothrow_swappable_v<ista::TimingArcDelayState>);
static_assert(std::is_nothrow_swappable_v<std::vector<std::string>>);

static_assert(noexcept(std::declval<ista::TimingPoint&>().set_arrival(0.0)));
static_assert(noexcept(std::declval<ista::TimingPoint&>().set_required(0.0)));
static_assert(noexcept(std::declval<ista::TimingPoint&>().set_slack(0.0)));
static_assert(noexcept(std::declval<ista::TimingPoint&>().set_launch_time(0.0)));
static_assert(noexcept(std::declval<ista::TimingPoint&>().set_level(0)));
static_assert(noexcept(std::declval<ista::TimingPoint&>().set_is_clock_point(false)));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_predecessor()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_clock_name()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().set_predecessor_arc_idx(0)));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_clock_arrival_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_clock_slew_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_clock_predecessor_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_clock_predecessor_arc_delay_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_clock_predecessor_trans_type_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_physical_clock_arrival_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_physical_clock_slew_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_physical_clock_predecessor_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_physical_clock_predecessor_arc_delay_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_physical_clock_predecessor_trans_type_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_data_slew_map()));
static_assert(noexcept(std::declval<ista::TimingPoint&>().get_path_state_map()));

static_assert(noexcept(std::declval<ista::Arc&>().set_delay(0.0)));
static_assert(noexcept(std::declval<ista::Arc&>().set_delay_min(0.0)));
static_assert(noexcept(std::declval<ista::Arc&>().set_delay_max(0.0)));
static_assert(noexcept(std::declval<ista::Arc&>().get_trans_delay_map()));
static_assert(noexcept(std::declval<ista::Arc&>().get_input_output_delay_map()));
static_assert(noexcept(std::declval<ista::Arc&>().get_graph_delay_map()));
static_assert(noexcept(std::declval<ista::Arc&>().get_trans_type_map()));

void publishClockState(ista::TimingPoint& target, PreparedClockState& prepared, bool physical) noexcept
{
  if (physical) {
    target.get_physical_clock_arrival_map().swap(prepared.arrivals);
    target.get_physical_clock_slew_map().swap(prepared.slews);
    target.get_physical_clock_predecessor_map().swap(prepared.predecessors);
    target.get_physical_clock_predecessor_arc_delay_map().swap(prepared.predecessor_arc_delays);
    target.get_physical_clock_predecessor_trans_type_map().swap(prepared.predecessor_transitions);
    return;
  }
  target.get_clock_arrival_map().swap(prepared.arrivals);
  target.get_clock_slew_map().swap(prepared.slews);
  target.get_clock_predecessor_map().swap(prepared.predecessors);
  target.get_clock_predecessor_arc_delay_map().swap(prepared.predecessor_arc_delays);
  target.get_clock_predecessor_trans_type_map().swap(prepared.predecessor_transitions);
}

}  // namespace

void publishPreparedTimingResult(PreparedTimingResult& prepared) noexcept
{
  for (auto& node : prepared.nodes) {
    node.target->set_arrival(node.arrival);
    node.target->set_required(node.required);
    node.target->set_slack(node.slack);
    node.target->set_launch_time(node.launch);
    node.target->set_level(node.level);
    node.target->set_is_clock_point(node.is_clock_point);
    node.target->get_predecessor().swap(node.predecessor);
    node.target->get_clock_name().swap(node.clock_name);
    node.target->set_predecessor_arc_idx(node.predecessor_arc_index);
    publishClockState(*node.target, node.effective_clock, false);
    publishClockState(*node.target, node.physical_clock, true);
    node.target->get_data_slew_map().swap(node.data_slews);
    node.target->get_path_state_map().swap(node.path_states);
  }

  for (auto& arc : prepared.arcs) {
    arc.target->set_delay(arc.delay);
    arc.target->set_delay_min(arc.minimum_delay);
    arc.target->set_delay_max(arc.maximum_delay);
    arc.target->get_trans_delay_map().swap(arc.transition_delays);
    arc.target->get_input_output_delay_map().swap(arc.input_output_delays);
    arc.target->get_graph_delay_map().swap(arc.graph_delays);
    arc.target->_timing_arc_delay_state.swap(arc.timing_arc_delays);
    arc.target->get_trans_type_map().swap(arc.transition_types);
  }

  prepared.timing_order_target->swap(prepared.timing_order);
}

void publishPreparedSessionObservationPair(CsrCommittedSessionSnapshot& snapshot, PreparedSessionObservationPair& prepared) noexcept
{
  snapshot.active_observation.swap(prepared.active);
  snapshot.released_observation.swap(prepared.released);
}

void publishPreparedTimingResultAndSessionObservationPair(PreparedTimingResult& timing_result, CsrCommittedSessionSnapshot& snapshot,
                                                          PreparedSessionObservationPair& observation_pair) noexcept
{
  publishPreparedTimingResult(timing_result);
  publishPreparedSessionObservationPair(snapshot, observation_pair);
}

void publishReleasedSessionObservation(CsrCommittedSessionSnapshot& snapshot) noexcept
{
  if (!snapshot.active_observation || !snapshot.released_observation || snapshot.active_observation->state == CsrSnapshotState::kReleased)
    return;
  snapshot.active_observation.swap(snapshot.released_observation);
}

}  // namespace ista::csr::detail
