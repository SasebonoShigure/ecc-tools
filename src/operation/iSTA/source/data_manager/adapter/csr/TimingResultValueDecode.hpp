// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include "CsrQueryValues.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista::csr::detail {
// Presence pairs and identities are checked at the caller's input boundary.
inline bool appendClockLane(PreparedClockState& prepared, AnalysisType analysis, TransType transition,
                            const value_detail::ClockLane& clock, const std::string* predecessor)
{
  TransType predecessor_transition = TransType::kNone;
  if (predecessor) {
    switch (static_cast<Transition>(clock.predecessor_transition)) {
      case Transition::kRise: predecessor_transition = TransType::kRise; break;
      case Transition::kFall: predecessor_transition = TransType::kFall; break;
      case Transition::kNone: break;
      default: return false;
    }
  }
  if (clock.arrival.present) prepared.arrivals[analysis][transition] = clock.arrival.value;
  if (clock.slew.present) prepared.slews[analysis][transition] = clock.slew.value;
  if (predecessor) {
    prepared.predecessors[analysis][transition] = *predecessor;
    prepared.predecessor_arc_delays[analysis][transition] = clock.predecessor_delay;
    prepared.predecessor_transitions[analysis][transition] = predecessor_transition;
  }
  return true;
}

inline TimingPathState preparePathBase(const value_detail::PathState& value, TransType transition,
                                      const std::string& tag, const std::string& start)
{
  TimingPathState state;
  state.set_arrival(value.arrival);
  state.set_slew(value.slew);
  state.set_launch_time(value.launch);
  state.set_start_point(start);
  state.set_clock_name(tag);
  state.set_trans_type(transition);
  return state;
}

inline void attachPathPredecessor(TimingPathState& state, const std::string& predecessor,
                                  std::size_t native_rank, double delay, TransType transition)
{
  state.set_predecessor(predecessor);
  state.set_predecessor_arc_idx(native_rank);
  state.set_predecessor_arc_delay(delay);
  state.set_predecessor_trans_type(transition);
}
}  // namespace ista::csr::detail
