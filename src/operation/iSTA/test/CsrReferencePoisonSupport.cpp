// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CsrReferencePoisonSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Arc.hpp"
#include "CsrReferencePoisonEncodingInternal.hpp"
#include "Database.hpp"
#include "TimingPathState.hpp"
#include "TimingPoint.hpp"

namespace ista::csr::test {
namespace {

using namespace poison_wire;

std::int32_t chooseFallbackKey(const ista::Arc& arc)
{
  std::int32_t key = std::numeric_limits<std::int32_t>::min();
  const auto& values = arc.get_timing_arc_delay_map();
  while (values.contains(key)) {
    if (key == std::numeric_limits<std::int32_t>::max())
      throw std::runtime_error("no timing-arc index remains for fallback poison");
    ++key;
  }
  return key;
}

std::array<bool, 3> fallbackBehavior(ista::Arc arc, std::int32_t fallback_key)
{
  const auto find_unused = [&](std::int32_t start) {
    const auto& values = std::as_const(arc).get_timing_arc_delay_map();
    std::int32_t key = start;
    while (values.contains(key)) {
      if (key == std::numeric_limits<std::int32_t>::max())
        throw std::runtime_error("no ordinary timing-arc key remains");
      ++key;
    }
    return key;
  };
  const std::int32_t min_key = find_unused(fallback_key + 1);
  ista::Arc ordinary_min = arc;
  ordinary_min.update_timing_arc_delay(min_key, ista::AnalysisType::kMin, ista::TransType::kRise, ista::TransType::kRise, 10.0, false);
  ordinary_min.update_timing_arc_delay(min_key, ista::AnalysisType::kMin, ista::TransType::kRise, ista::TransType::kRise, 11.0, false);
  const bool min_replaced
      = std::as_const(ordinary_min).get_timing_arc_delay_map().at(min_key).at(ista::AnalysisType::kMin).at(ista::TransType::kRise).at(ista::TransType::kRise)
        != 10.0;

  const std::int32_t max_key = find_unused(min_key + 1);
  ista::Arc ordinary_max = arc;
  ordinary_max.update_timing_arc_delay(max_key, ista::AnalysisType::kMax, ista::TransType::kRise, ista::TransType::kRise, 10.0, false);
  ordinary_max.update_timing_arc_delay(max_key, ista::AnalysisType::kMax, ista::TransType::kRise, ista::TransType::kRise, 9.0, false);
  const bool max_replaced
      = std::as_const(ordinary_max).get_timing_arc_delay_map().at(max_key).at(ista::AnalysisType::kMax).at(ista::TransType::kRise).at(ista::TransType::kRise)
        != 10.0;

  ista::Arc fallback = arc;
  const double before
      = std::as_const(fallback).get_timing_arc_delay_map().at(fallback_key).at(ista::AnalysisType::kMax).at(ista::TransType::kRise).at(ista::TransType::kRise);
  if (!std::isfinite(before))
    throw std::runtime_error("fallback poison precondition must be finite");
  const double probe = std::nextafter(before, -std::numeric_limits<double>::infinity());
  fallback.update_timing_arc_delay(fallback_key, ista::AnalysisType::kMax, ista::TransType::kRise, ista::TransType::kRise, probe, false);
  const bool fallback_replaced
      = std::as_const(fallback).get_timing_arc_delay_map().at(fallback_key).at(ista::AnalysisType::kMax).at(ista::TransType::kRise).at(ista::TransType::kRise)
        != before;
  return {min_replaced, max_replaced, fallback_replaced};
}

std::vector<std::int32_t> planFallbackKeys(ista::Database& database, const CsrReferenceInputRawProjection& raw)
{
  if (raw.nodes.empty() || raw.arcs.empty())
    throw std::runtime_error("poison requires nonempty raw node and arc inventories");
  if (raw.arcs.size() != database.get_arc_list().size())
    throw std::runtime_error("raw arc inventory differs from Database rank inventory");
  std::vector<std::int32_t> result;
  result.reserve(raw.arcs.size());
  for (const RawArc& raw_arc : raw.arcs) {
    if (raw_arc.native_rank >= database.get_arc_list().size())
      throw std::runtime_error("raw poison arc rank is out of bounds");
    ista::Arc virtual_arc = database.get_arc_list().at(raw_arc.native_rank);
    if (std::as_const(virtual_arc).get_timing_arc_delay_map().empty())
      virtual_arc.get_timing_arc_delay_map()[0][ista::AnalysisType::kMax][ista::TransType::kRise][ista::TransType::kRise] = 4.0;
    result.push_back(chooseFallbackKey(virtual_arc));
  }
  return result;
}

void precondition(ista::Database& database, const CsrReferenceInputRawProjection& raw, const std::vector<std::int32_t>& fallback_keys)
{
  for (const RawNode& node : raw.nodes) {
    auto found = database.get_timing_point_map().find(node.full_name);
    if (found == database.get_timing_point_map().end())
      throw std::runtime_error("raw node has no TimingPoint poison subject");
    auto& point = found->second;
    if (point.get_clock_arrival_map().empty())
      point.get_clock_arrival_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = 1.0;
    if (point.get_physical_clock_arrival_map().empty())
      point.get_physical_clock_arrival_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = 2.0;
    if (point.get_clock_slew_map().empty())
      point.get_clock_slew_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = 3.0;
    if (point.get_physical_clock_slew_map().empty())
      point.get_physical_clock_slew_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = 4.0;
    if (point.get_clock_predecessor_map().empty())
      point.get_clock_predecessor_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = "__csr_predecessor__";
    if (point.get_physical_clock_predecessor_map().empty())
      point.get_physical_clock_predecessor_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = "__csr_physical_predecessor__";
    if (point.get_clock_predecessor_arc_delay_map().empty())
      point.get_clock_predecessor_arc_delay_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = 5.0;
    if (point.get_physical_clock_predecessor_arc_delay_map().empty())
      point.get_physical_clock_predecessor_arc_delay_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = 6.0;
    if (point.get_clock_predecessor_trans_type_map().empty())
      point.get_clock_predecessor_trans_type_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = ista::TransType::kRise;
    if (point.get_physical_clock_predecessor_trans_type_map().empty())
      point.get_physical_clock_predecessor_trans_type_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = ista::TransType::kRise;
    if (point.get_data_slew_map().empty())
      point.get_data_slew_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = 7.0;
    if (point.get_path_state_map().empty())
      point.get_path_state_map()[ista::AnalysisType::kMax][ista::PathSourceType::kInput][ista::TransType::kRise]["__csr_poison_clock__"]
          = ista::TimingPathState{};
  }

  for (std::size_t index = 0; index < raw.arcs.size(); ++index) {
    const RawArc& raw_arc = raw.arcs[index];
    auto& arc = database.get_arc_list().at(raw_arc.native_rank);
    if (arc.get_trans_delay_map().empty())
      arc.get_trans_delay_map()[ista::AnalysisType::kMax][ista::TransType::kRise] = 1.0;
    if (arc.get_trans_type_map().empty())
      arc.get_trans_type_map()[ista::TransType::kRise] = ista::TransType::kRise;
    if (arc.get_input_output_delay_map().empty())
      arc.get_input_output_delay_map()[ista::AnalysisType::kMax][ista::TransType::kRise][ista::TransType::kRise] = 2.0;
    if (arc.get_graph_delay_map().empty())
      arc.get_graph_delay_map()[ista::AnalysisType::kMax][ista::TransType::kRise][ista::TransType::kRise] = 3.0;
    if (std::as_const(arc).get_timing_arc_delay_map().empty())
      arc.get_timing_arc_delay_map()[0][ista::AnalysisType::kMax][ista::TransType::kRise][ista::TransType::kRise] = 4.0;
    const std::int32_t fallback_key = chooseFallbackKey(arc);
    if (fallback_key != fallback_keys.at(index))
      throw std::runtime_error("timing-arc fallback key plan drifted during scaffold");
    arc.update_timing_arc_delay(fallback_key, ista::AnalysisType::kMax, ista::TransType::kRise, ista::TransType::kRise, 40.0, true);
  }
  if (database.get_timing_order_list().size() < 2)
    throw std::runtime_error("timing-order poison needs at least two entries");
}

std::vector<std::uint8_t> encodeFamily(ista::Database& database, const CsrReferenceInputRawProjection& raw, const ReferencePoisonBinding& binding,
                                       std::size_t family, const std::vector<std::int32_t>& fallback_keys, bool virtual_fallback)
{
  Writer writer;
  appendBinding(writer, kReferencePoisonFamilyTokens.at(family), binding);
  if (family <= 9) {
    writer.appendU64(raw.nodes.size());
    for (const RawNode& node : raw.nodes) {
      writer.appendString(node.full_name);
      auto& point = database.get_timing_point_map().at(node.full_name);
      switch (family) {
        case 0:
          writer.appendI32(point.get_level());
          break;
        case 1:
          writer.appendBool(point.get_is_clock_point());
          break;
        case 2:
          appendCanaryDouble(writer, point.get_arrival());
          appendCanaryDouble(writer, point.get_required());
          appendCanaryDouble(writer, point.get_slack());
          appendCanaryDouble(writer, point.get_launch_time());
          writer.appendString(point.get_predecessor());
          writer.appendString(point.get_clock_name());
          writer.appendU64(point.get_predecessor_arc_idx());
          break;
        case 3:
          appendAnalysisTransitionMap(writer, point.get_clock_arrival_map(), appendCanaryDouble);
          appendAnalysisTransitionMap(writer, point.get_physical_clock_arrival_map(), appendCanaryDouble);
          break;
        case 4:
          appendAnalysisTransitionMap(writer, point.get_clock_slew_map(), appendCanaryDouble);
          appendAnalysisTransitionMap(writer, point.get_physical_clock_slew_map(), appendCanaryDouble);
          break;
        case 5:
          appendAnalysisTransitionMap(writer, point.get_clock_predecessor_map(), [](Writer& local, const std::string& value) { local.appendString(value); });
          appendAnalysisTransitionMap(writer, point.get_physical_clock_predecessor_map(),
                                      [](Writer& local, const std::string& value) { local.appendString(value); });
          break;
        case 6:
          appendAnalysisTransitionMap(writer, point.get_clock_predecessor_arc_delay_map(), appendCanaryDouble);
          appendAnalysisTransitionMap(writer, point.get_physical_clock_predecessor_arc_delay_map(), appendCanaryDouble);
          break;
        case 7:
          appendAnalysisTransitionMap(writer, point.get_clock_predecessor_trans_type_map(),
                                      [](Writer& local, ista::TransType value) { local.appendU8(transitionToken(value)); });
          appendAnalysisTransitionMap(writer, point.get_physical_clock_predecessor_trans_type_map(),
                                      [](Writer& local, ista::TransType value) { local.appendU8(transitionToken(value)); });
          break;
        case 8:
          appendAnalysisTransitionMap(writer, point.get_data_slew_map(), appendCanaryDouble);
          break;
        case 9:
          appendPathStates(writer, point);
          break;
        default:
          throw std::runtime_error("invalid point poison family");
      }
    }
  } else if (family <= 15) {
    writer.appendU64(raw.arcs.size());
    for (std::size_t index = 0; index < raw.arcs.size(); ++index) {
      const RawArc& raw_arc = raw.arcs[index];
      appendArcKey(writer, raw_arc);
      auto& arc = database.get_arc_list().at(raw_arc.native_rank);
      switch (family) {
        case 10:
          appendCanaryDouble(writer, arc.get_delay());
          appendCanaryDouble(writer, arc.get_delay_max());
          appendCanaryDouble(writer, arc.get_delay_min());
          break;
        case 11:
          appendAnalysisTransitionMap(writer, arc.get_trans_delay_map(), appendCanaryDouble);
          writer.appendU64(arc.get_trans_type_map().size());
          for (ista::TransType transition : kPoisonTransitions) {
            const auto found = arc.get_trans_type_map().find(transition);
            if (found == arc.get_trans_type_map().end())
              continue;
            writer.appendU8(transitionToken(transition));
            writer.appendU8(transitionToken(found->second));
          }
          break;
        case 12:
          appendInputOutputMap(writer, arc.get_input_output_delay_map());
          break;
        case 13:
          appendInputOutputMap(writer, arc.get_graph_delay_map());
          break;
        case 14:
          appendTimingArcMap(writer, std::as_const(arc).get_timing_arc_delay_map());
          break;
        case 15: {
          ista::Arc encoded = arc;
          if (virtual_fallback)
            encoded.update_timing_arc_delay(fallback_keys.at(index), ista::AnalysisType::kMax, ista::TransType::kRise, ista::TransType::kRise, 40.0, true);
          const auto behavior = fallbackBehavior(std::move(encoded), fallback_keys.at(index));
          writer.appendBool(behavior[0]);
          writer.appendBool(behavior[1]);
          writer.appendBool(behavior[2]);
          break;
        }
        default:
          throw std::runtime_error("invalid arc poison family");
      }
    }
  } else {
    writer.appendU64(1);
    writer.appendString("database");
    writer.appendU64(database.get_timing_order_list().size());
    for (const std::string& name : database.get_timing_order_list())
      writer.appendString(name);
  }
  return writer.bytes();
}

using EncodedPoisonFamilies = std::array<std::vector<std::uint8_t>, kReferencePoisonFamilyTokens.size()>;

EncodedPoisonFamilies encodeFamilies(ista::Database& database, const CsrReferenceInputRawProjection& raw, const ReferencePoisonBinding& binding,
                                     const std::vector<std::int32_t>& fallback_keys, bool virtual_fallback)
{
  EncodedPoisonFamilies result;
  for (std::size_t family = 0; family < result.size(); ++family)
    result[family] = encodeFamily(database, raw, binding, family, fallback_keys, virtual_fallback && family == 15);
  return result;
}

template <typename Value, typename Change>
void changeAnalysisTransitionMap(std::map<ista::AnalysisType, std::map<ista::TransType, Value>>& values, Change change)
{
  for (auto& [analysis, transitions] : values) {
    static_cast<void>(analysis);
    for (auto& [transition, value] : transitions) {
      static_cast<void>(transition);
      change(value);
    }
  }
}

void changeInputOutputMap(std::map<ista::AnalysisType, std::map<ista::TransType, std::map<ista::TransType, double>>>& values)
{
  for (auto& [analysis, inputs] : values) {
    static_cast<void>(analysis);
    for (auto& [input, outputs] : inputs) {
      static_cast<void>(input);
      for (auto& [output, value] : outputs) {
        static_cast<void>(output);
        value = changedDouble(value);
      }
    }
  }
}

void changeTimingArcMap(std::map<std::int32_t, std::map<ista::AnalysisType, std::map<ista::TransType, std::map<ista::TransType, double>>>>& values)
{
  for (auto& [arc_index, analyses] : values) {
    static_cast<void>(arc_index);
    for (auto& [analysis, inputs] : analyses) {
      static_cast<void>(analysis);
      for (auto& [input, outputs] : inputs) {
        static_cast<void>(input);
        for (auto& [output, value] : outputs) {
          static_cast<void>(output);
          value = changedDouble(value);
        }
      }
    }
  }
}

void poisonFamily(ista::Database& database, const CsrReferenceInputRawProjection& raw, std::size_t family, const std::vector<std::int32_t>& fallback_keys)
{
  if (family <= 9) {
    for (const RawNode& node : raw.nodes) {
      auto& point = database.get_timing_point_map().at(node.full_name);
      switch (family) {
        case 0:
          point.set_level(point.get_level() == std::numeric_limits<std::int32_t>::max() ? std::numeric_limits<std::int32_t>::min() : point.get_level() + 1);
          break;
        case 1:
          point.set_is_clock_point(!point.get_is_clock_point());
          break;
        case 2:
          point.set_arrival(changedDouble(point.get_arrival()));
          point.set_required(changedDouble(point.get_required()));
          point.set_slack(changedDouble(point.get_slack()));
          point.set_launch_time(changedDouble(point.get_launch_time()));
          point.set_predecessor(point.get_predecessor() + "#poison");
          point.set_clock_name(std::string(point.get_clock_name()) + "#poison");
          point.set_predecessor_arc_idx(point.get_predecessor_arc_idx() == std::numeric_limits<std::size_t>::max() ? 0 : point.get_predecessor_arc_idx() + 1);
          break;
        case 3:
          changeAnalysisTransitionMap(point.get_clock_arrival_map(), [](double& value) { value = changedDouble(value); });
          changeAnalysisTransitionMap(point.get_physical_clock_arrival_map(), [](double& value) { value = changedDouble(value); });
          break;
        case 4:
          changeAnalysisTransitionMap(point.get_clock_slew_map(), [](double& value) { value = changedDouble(value); });
          changeAnalysisTransitionMap(point.get_physical_clock_slew_map(), [](double& value) { value = changedDouble(value); });
          break;
        case 5:
          changeAnalysisTransitionMap(point.get_clock_predecessor_map(), [](std::string& value) { value += "#poison"; });
          changeAnalysisTransitionMap(point.get_physical_clock_predecessor_map(), [](std::string& value) { value += "#poison"; });
          break;
        case 6:
          changeAnalysisTransitionMap(point.get_clock_predecessor_arc_delay_map(), [](double& value) { value = changedDouble(value); });
          changeAnalysisTransitionMap(point.get_physical_clock_predecessor_arc_delay_map(), [](double& value) { value = changedDouble(value); });
          break;
        case 7:
          changeAnalysisTransitionMap(point.get_clock_predecessor_trans_type_map(), [](ista::TransType& value) { value = changedTransition(value); });
          changeAnalysisTransitionMap(point.get_physical_clock_predecessor_trans_type_map(), [](ista::TransType& value) { value = changedTransition(value); });
          break;
        case 8:
          changeAnalysisTransitionMap(point.get_data_slew_map(), [](double& value) { value = changedDouble(value); });
          break;
        case 9:
          for (auto& [analysis, sources] : point.get_path_state_map())
            for (auto& [source, transitions] : sources)
              for (auto& [transition, clocks] : transitions)
                for (auto& [clock, state] : clocks) {
                  static_cast<void>(analysis);
                  static_cast<void>(source);
                  static_cast<void>(transition);
                  static_cast<void>(clock);
                  state.set_arrival(changedDouble(state.get_arrival()));
                  state.set_slew(changedDouble(state.get_slew()));
                  state.set_launch_time(changedDouble(state.get_launch_time()));
                  state.set_start_point(state.get_start_point() + "#poison");
                  state.set_predecessor(state.get_predecessor() + "#poison");
                  state.set_clock_name(state.get_clock_name() + "#poison");
                  state.set_crpr_clock_pin(state.get_crpr_clock_pin() + "#poison");
                  state.set_predecessor_arc_idx(
                      state.get_predecessor_arc_idx() == std::numeric_limits<std::size_t>::max() ? 0 : state.get_predecessor_arc_idx() + 1);
                  state.set_predecessor_arc_delay(changedDouble(state.get_predecessor_arc_delay()));
                  state.set_trans_type(changedTransition(state.get_trans_type()));
                  state.set_predecessor_trans_type(changedTransition(state.get_predecessor_trans_type()));
                  state.set_crpr_clock_trans_type(changedTransition(state.get_crpr_clock_trans_type()));
                }
          break;
      }
    }
    return;
  }

  if (family <= 15) {
    for (std::size_t index = 0; index < raw.arcs.size(); ++index) {
      auto& arc = database.get_arc_list().at(raw.arcs[index].native_rank);
      switch (family) {
        case 10:
          arc.set_delay(changedDouble(arc.get_delay()));
          arc.set_delay_max(changedDouble(arc.get_delay_max()));
          arc.set_delay_min(changedDouble(arc.get_delay_min()));
          break;
        case 11:
          changeAnalysisTransitionMap(arc.get_trans_delay_map(), [](double& value) { value = changedDouble(value); });
          for (auto& [transition, value] : arc.get_trans_type_map()) {
            static_cast<void>(transition);
            value = changedTransition(value);
          }
          break;
        case 12:
          changeInputOutputMap(arc.get_input_output_delay_map());
          break;
        case 13:
          changeInputOutputMap(arc.get_graph_delay_map());
          break;
        case 14: {
          ista::TimingArcDelayState timing_arc_delay_state = arc.get_timing_arc_delay_state();
          changeTimingArcMap(timing_arc_delay_state.delay_map);
          arc.replace_timing_arc_delay_state(std::move(timing_arc_delay_state));
          break;
        }
        case 15: {
          const std::int32_t fallback_key = fallback_keys.at(index);
          const double before = std::as_const(arc)
                                    .get_timing_arc_delay_map()
                                    .at(fallback_key)
                                    .at(ista::AnalysisType::kMax)
                                    .at(ista::TransType::kRise)
                                    .at(ista::TransType::kRise);
          if (!std::isfinite(before))
            throw std::runtime_error("fallback provenance poison requires a finite cached value");
          arc.update_timing_arc_delay(fallback_key, ista::AnalysisType::kMax, ista::TransType::kRise, ista::TransType::kRise, before, false);
          break;
        }
      }
    }
    return;
  }

  auto& order = database.get_timing_order_list();
  std::rotate(order.begin(), order.begin() + 1, order.end());
}

}  // namespace

std::vector<ReferencePoisonCanary> poisonReferenceDerivedState(ista::Database& database, const CsrReferenceInputRawProjection& raw,
                                                               const ReferencePoisonBinding& binding)
{
  if (binding.input_case_sha256.empty() || binding.run_nonce.empty() || binding.session_token == 0)
    throw std::invalid_argument("poison canary binding is incomplete");
  const std::vector<std::int32_t> fallback_keys = planFallbackKeys(database, raw);
  const EncodedPoisonFamilies pristine = encodeFamilies(database, raw, binding, fallback_keys, true);
  precondition(database, raw, fallback_keys);
  const EncodedPoisonFamilies before = encodeFamilies(database, raw, binding, fallback_keys, false);

  std::vector<ReferencePoisonCanary> canaries;
  canaries.reserve(kReferencePoisonFamilyTokens.size());
  for (std::size_t family = 0; family < kReferencePoisonFamilyTokens.size(); ++family) {
    ReferencePoisonCanary canary;
    canary.family = kReferencePoisonFamilyTokens[family];
    canary.pristine_typed_bytes = pristine[family];
    canary.before_typed_bytes = before[family];
    canaries.push_back(std::move(canary));
  }

  for (std::size_t family = 0; family < kReferencePoisonFamilyTokens.size(); ++family) {
    const EncodedPoisonFamilies round_before = encodeFamilies(database, raw, binding, fallback_keys, false);
    poisonFamily(database, raw, family, fallback_keys);
    const EncodedPoisonFamilies round_after = encodeFamilies(database, raw, binding, fallback_keys, false);
    for (std::size_t observed = 0; observed < kReferencePoisonFamilyTokens.size(); ++observed) {
      const bool changed = round_before[observed] != round_after[observed];
      if (changed != (observed == family))
        throw std::runtime_error("poison family '" + std::string(kReferencePoisonFamilyTokens[family]) + "' has an inexact all-family change mask at '"
                                 + std::string(kReferencePoisonFamilyTokens[observed]) + "'");
    }
    canaries[family].after_typed_bytes = round_after[family];
  }
  return canaries;
}

}  // namespace ista::csr::test
