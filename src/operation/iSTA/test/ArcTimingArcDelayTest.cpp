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

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "Arc.hpp"
#include "TimingPathState.hpp"
#include "TimingPath.hpp"

namespace {

constexpr ista::TransType kInputTransition = ista::TransType::kRise;
constexpr ista::TransType kOutputTransition = ista::TransType::kFall;

static_assert(std::is_same_v<decltype(std::declval<ista::Arc&>().get_timing_arc_delay_map()), ista::TimingArcDelayMap&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::Arc&>().get_timing_arc_delay_map()), const ista::TimingArcDelayMap&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::Arc&>().get_timing_arc_delay_state()), const ista::TimingArcDelayState&>);
static_assert(noexcept(std::declval<ista::Arc&>().get_timing_arc_delay_map()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_timing_arc_delay_map()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_timing_arc_delay_state()));
static_assert(noexcept(std::declval<ista::Arc&>().replace_timing_arc_delay_state(std::declval<ista::TimingArcDelayState&&>())));
static_assert(std::is_nothrow_swappable_v<ista::TimingArcDelayMap>);
static_assert(std::is_nothrow_swappable_v<ista::TimingArcDelayFallbackKeySet>);
static_assert(std::is_nothrow_swappable_v<ista::TimingArcDelayState>);

using TransDelayMap = std::remove_reference_t<decltype(std::declval<ista::Arc&>().get_trans_delay_map())>;
using InputOutputDelayMap = std::remove_reference_t<decltype(std::declval<ista::Arc&>().get_input_output_delay_map())>;
using GraphDelayMap = std::remove_reference_t<decltype(std::declval<ista::Arc&>().get_graph_delay_map())>;
using TransTypeMap = std::remove_reference_t<decltype(std::declval<ista::Arc&>().get_trans_type_map())>;

static_assert(std::is_same_v<decltype(std::declval<const ista::Arc&>().get_trans_delay_map()), const TransDelayMap&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::Arc&>().get_input_output_delay_map()), const InputOutputDelayMap&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::Arc&>().get_graph_delay_map()), const GraphDelayMap&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::Arc&>().get_trans_type_map()), const TransTypeMap&>);
static_assert(noexcept(std::declval<ista::Arc&>().get_trans_delay_map()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_trans_delay_map()));
static_assert(noexcept(std::declval<ista::Arc&>().get_input_output_delay_map()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_input_output_delay_map()));
static_assert(noexcept(std::declval<ista::Arc&>().get_graph_delay_map()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_graph_delay_map()));
static_assert(noexcept(std::declval<ista::Arc&>().get_trans_type_map()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_trans_type_map()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_delay()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_delay_min()));
static_assert(noexcept(std::declval<const ista::Arc&>().get_delay_max()));
static_assert(noexcept(std::declval<ista::Arc&>().set_delay(0.0)));
static_assert(noexcept(std::declval<ista::Arc&>().set_delay_min(0.0)));
static_assert(noexcept(std::declval<ista::Arc&>().set_delay_max(0.0)));

static_assert(std::is_same_v<decltype(std::declval<ista::TimingPathState&>().get_start_point()), std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::TimingPathState&>().get_start_point()), const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<ista::TimingPathState&>().get_predecessor()), std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::TimingPathState&>().get_predecessor()), const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<ista::TimingPathState&>().get_clock_name()), std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::TimingPathState&>().get_clock_name()), const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<ista::TimingPathState&>().get_crpr_clock_pin()), std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const ista::TimingPathState&>().get_crpr_clock_pin()), const std::string&>);
static_assert(noexcept(std::declval<const ista::TimingPathState&>().get_start_point()));
static_assert(noexcept(std::declval<const ista::TimingPathState&>().get_predecessor()));
static_assert(noexcept(std::declval<const ista::TimingPathState&>().get_clock_name()));
static_assert(noexcept(std::declval<const ista::TimingPathState&>().get_crpr_clock_pin()));

ista::TimingArcDelayFallbackKey fallbackKey(std::int32_t timing_arc_idx, ista::AnalysisType analysis_type)
{
  return {timing_arc_idx, analysis_type, kInputTransition, kOutputTransition};
}

double getDelay(const ista::Arc& arc, std::int32_t timing_arc_idx, ista::AnalysisType analysis_type)
{
  return arc.get_timing_arc_delay_map().at(timing_arc_idx).at(analysis_type).at(kInputTransition).at(kOutputTransition);
}

bool isFallback(const ista::Arc& arc, std::int32_t timing_arc_idx, ista::AnalysisType analysis_type)
{
  return arc.get_timing_arc_delay_state().fallback_key_set.contains(fallbackKey(timing_arc_idx, analysis_type));
}

bool testMinDelayReplacesInitializationFallback()
{
  ista::Arc arc;
  constexpr std::int32_t kTimingArcIndex = 3;

  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.010, true);
  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.008, true);
  if (getDelay(arc, kTimingArcIndex, ista::AnalysisType::kMin) != 0.008 || !isFallback(arc, kTimingArcIndex, ista::AnalysisType::kMin)) {
    return false;
  }

  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.020, false);
  if (getDelay(arc, kTimingArcIndex, ista::AnalysisType::kMin) != 0.020 || isFallback(arc, kTimingArcIndex, ista::AnalysisType::kMin)) {
    return false;
  }

  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.018, false);
  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.025, false);
  return getDelay(arc, kTimingArcIndex, ista::AnalysisType::kMin) == 0.018;
}

bool testMaxDelayReplacesInitializationFallback()
{
  ista::Arc arc;
  constexpr std::int32_t kTimingArcIndex = 7;

  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 0.010, true);
  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 0.012, true);
  if (getDelay(arc, kTimingArcIndex, ista::AnalysisType::kMax) != 0.012 || !isFallback(arc, kTimingArcIndex, ista::AnalysisType::kMax)) {
    return false;
  }

  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 0.005, false);
  if (getDelay(arc, kTimingArcIndex, ista::AnalysisType::kMax) != 0.005 || isFallback(arc, kTimingArcIndex, ista::AnalysisType::kMax)) {
    return false;
  }

  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 0.007, false);
  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 0.004, false);
  return getDelay(arc, kTimingArcIndex, ista::AnalysisType::kMax) == 0.007;
}

bool testFallbackKeysAreIndependent()
{
  ista::Arc arc;
  constexpr std::int32_t kFirstTimingArcIndex = 11;
  constexpr std::int32_t kSecondTimingArcIndex = 12;

  arc.update_timing_arc_delay(kFirstTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.010, true);
  arc.update_timing_arc_delay(kSecondTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.020, true);
  arc.update_timing_arc_delay(kFirstTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.030, false);
  if (getDelay(arc, kFirstTimingArcIndex, ista::AnalysisType::kMin) != 0.030 || isFallback(arc, kFirstTimingArcIndex, ista::AnalysisType::kMin)
      || !isFallback(arc, kSecondTimingArcIndex, ista::AnalysisType::kMin)) {
    return false;
  }

  arc.update_timing_arc_delay(kSecondTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.040, false);
  return getDelay(arc, kSecondTimingArcIndex, ista::AnalysisType::kMin) == 0.040 && !isFallback(arc, kSecondTimingArcIndex, ista::AnalysisType::kMin);
}

bool testEqualFirstDynamicClearsFallback()
{
  ista::Arc arc;
  constexpr std::int32_t kTimingArcIndex = 15;
  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 0.25, true);

  const ista::Arc& read_only_arc = arc;
  if (getDelay(read_only_arc, kTimingArcIndex, ista::AnalysisType::kMax) != 0.25 || !isFallback(read_only_arc, kTimingArcIndex, ista::AnalysisType::kMax)) {
    return false;
  }

  arc.update_timing_arc_delay(kTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 0.25, false);
  return getDelay(arc, kTimingArcIndex, ista::AnalysisType::kMax) == 0.25 && !isFallback(arc, kTimingArcIndex, ista::AnalysisType::kMax);
}

bool testLossyMutableGetterAndLegacySetterClearFallbacks()
{
  ista::Arc arc;
  constexpr std::int32_t kFirstTimingArcIndex = 21;
  constexpr std::int32_t kSecondTimingArcIndex = 22;
  constexpr std::int32_t kReplacementTimingArcIndex = 23;
  arc.update_timing_arc_delay(kFirstTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 1.0, true);
  arc.update_timing_arc_delay(kSecondTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 2.0, true);

  static_cast<void>(arc.get_timing_arc_delay_map());
  if (!arc.get_timing_arc_delay_state().fallback_key_set.empty() || getDelay(arc, kFirstTimingArcIndex, ista::AnalysisType::kMin) != 1.0
      || getDelay(arc, kSecondTimingArcIndex, ista::AnalysisType::kMax) != 2.0) {
    return false;
  }

  arc.update_timing_arc_delay(kReplacementTimingArcIndex, ista::AnalysisType::kMin, kInputTransition, kOutputTransition, 0.5, true);
  ista::TimingArcDelayMap replacement = static_cast<const ista::Arc&>(arc).get_timing_arc_delay_map();
  arc.set_timing_arc_delay_map(replacement);
  return arc.get_timing_arc_delay_state().fallback_key_set.empty() && getDelay(arc, kReplacementTimingArcIndex, ista::AnalysisType::kMin) == 0.5;
}

bool testEscapedMutableReferenceCannotLeaveStaleFallbacks()
{
  ista::Arc arc;
  constexpr std::int32_t kRebuiltTimingArcIndex = 31;
  constexpr std::int32_t kUntouchedTimingArcIndex = 32;
  ista::TimingArcDelayMap& escaped_delay_map = arc.get_timing_arc_delay_map();

  arc.update_timing_arc_delay(kRebuiltTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 3.0, true);
  arc.update_timing_arc_delay(kUntouchedTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 4.0, true);
  escaped_delay_map.erase(kRebuiltTimingArcIndex);
  escaped_delay_map.erase(kUntouchedTimingArcIndex);

  arc.update_timing_arc_delay(kRebuiltTimingArcIndex, ista::AnalysisType::kMax, kInputTransition, kOutputTransition, 5.0, false);
  return getDelay(arc, kRebuiltTimingArcIndex, ista::AnalysisType::kMax) == 5.0 && arc.get_timing_arc_delay_state().fallback_key_set.empty();
}

bool testTypedStateReplacement()
{
  ista::Arc arc;
  if (!arc.get_timing_arc_delay_state().delay_map.empty() || !arc.get_timing_arc_delay_state().fallback_key_set.empty()) {
    return false;
  }

  constexpr std::int32_t kTimingArcIndex = 41;
  ista::TimingArcDelayState replacement;
  replacement.delay_map[kTimingArcIndex][ista::AnalysisType::kMax][kInputTransition][kOutputTransition] = 6.0;
  replacement.fallback_key_set.insert(fallbackKey(kTimingArcIndex, ista::AnalysisType::kMax));
  const ista::TimingArcDelayState expected = replacement;

  arc.replace_timing_arc_delay_state(std::move(replacement));
  return arc.get_timing_arc_delay_state() == expected && getDelay(arc, kTimingArcIndex, ista::AnalysisType::kMax) == 6.0
         && isFallback(arc, kTimingArcIndex, ista::AnalysisType::kMax);
}

bool testPublicTypedStateReplacementNormalizesInvalidSubset()
{
  ista::Arc arc;
  ista::TimingArcDelayState invalid;
  invalid.fallback_key_set.insert(fallbackKey(43, ista::AnalysisType::kMin));

  arc.replace_timing_arc_delay_state(std::move(invalid));
  return arc.get_timing_arc_delay_state().delay_map.empty() && arc.get_timing_arc_delay_state().fallback_key_set.empty();
}

bool testTimingReportValueMoves()
{
  static_assert(std::is_nothrow_move_constructible_v<ista::TimingPath>);
  static_assert(std::is_nothrow_move_assignable_v<ista::TimingPath>);
  static_assert(std::is_nothrow_move_constructible_v<ista::TimingPathPoint>);
  static_assert(std::is_nothrow_move_assignable_v<ista::TimingPathPoint>);
  ista::TimingPath original;
  original.set_start_point(std::string(80, 's'));
  original.set_end_point(std::string(80, 'e'));
  original.set_slack(-3.25);
  ista::TimingPathPoint point;
  point.set_pin_name(std::string(80, 'p'));
  point.set_arrival(7.5);
  original.get_point_list().push_back(std::move(point));
  const auto copy = original;
  auto* points = original.get_point_list().data();
  auto moved = std::move(original);
  ista::TimingPath assigned;
  assigned = std::move(moved);
  if (assigned.get_point_list().data() != points || assigned.get_slack() != -3.25
      || assigned.get_start_point() != std::string(80, 's') || assigned.get_end_point() != std::string(80, 'e')
      || assigned.get_point_list().front().get_pin_name() != std::string(80, 'p')
      || assigned.get_point_list().front().get_arrival() != 7.5)
    return false;
  assigned.get_point_list().clear();
  auto independent = copy;
  return independent.get_point_list().size() == 1 && independent.get_point_list().front().get_arrival() == 7.5;
}

bool testTimingPathStateConstStringGetters()
{
  ista::TimingPathState state;
  state.set_start_point("start");
  state.set_predecessor("predecessor");
  state.set_clock_name("clock");
  state.set_crpr_clock_pin("crpr");

  const ista::TimingPathState& read_only_state = state;
  if (read_only_state.get_start_point() != "start" || read_only_state.get_predecessor() != "predecessor" || read_only_state.get_clock_name() != "clock"
      || read_only_state.get_crpr_clock_pin() != "crpr") {
    return false;
  }

  state.get_predecessor().clear();
  return read_only_state.get_predecessor().empty() && read_only_state.get_start_point() == "start" && read_only_state.get_clock_name() == "clock"
         && read_only_state.get_crpr_clock_pin() == "crpr";
}

}  // namespace

int main()
{
  return testMinDelayReplacesInitializationFallback() && testMaxDelayReplacesInitializationFallback() && testFallbackKeysAreIndependent()
                 && testEqualFirstDynamicClearsFallback() && testLossyMutableGetterAndLegacySetterClearFallbacks()
                 && testEscapedMutableReferenceCannotLeaveStaleFallbacks() && testTypedStateReplacement()
                 && testPublicTypedStateReplacementNormalizesInvalidSubset() && testTimingPathStateConstStringGetters() && testTimingReportValueMoves()
             ? 0
             : 1;
}
