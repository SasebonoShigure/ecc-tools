// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingPropagator.hpp"

#include <algorithm>
#include <string_view>
#include "DataManager.hpp"

namespace ista {

void TimingPropagator::propagateDataArrival()
{
  Database& database = STADM.getDatabase();
  struct Candidate {
    TimingPathState* source;
    std::string_view tag;
    std::size_t arc;
    TransType input;
    double arrival;
    double delay;
    bool seed;
  };
  struct Extreme { std::string_view tag; double arrival; bool selected; };
  std::vector<Candidate> candidates;
  std::vector<Extreme> extremes;
  for (std::string& pin_name : database.get_timing_order_list()) {
    auto& sink = database.get_timing_point_map().at(pin_name);
    const auto& incoming = database.get_incoming_arc_list_map()[pin_name];
    double extreme = sink.get_arrival();
    for (auto index : incoming) {
      auto& arc = database.get_arc_list()[index];
      if (isDisableArc(arc) || shouldStopDataPropagation(arc))
        continue;
      auto& source = database.get_timing_point_map().at(arc.get_source_pin());
      if (isFinite(source.get_arrival()))
        extreme = std::max(extreme, source.get_arrival() + arc.get_delay());
    }
    if (!isFinite(sink.get_arrival()) || extreme > sink.get_arrival() + 1e-9) {
      for (auto index : incoming) {
        auto& arc = database.get_arc_list()[index];
        if (isDisableArc(arc) || shouldStopDataPropagation(arc))
          continue;
        auto& source = database.get_timing_point_map().at(arc.get_source_pin());
        const double value = source.get_arrival() + arc.get_delay();
        if (isFinite(source.get_arrival()) && !(extreme > value + 1e-9)) {
          sink.set_arrival(value);
          sink.set_predecessor(arc.get_source_pin());
          sink.set_predecessor_arc_idx(index);
          sink.set_launch_time(source.get_launch_time());
          sink.set_clock_name(source.get_clock_name());
          break;
        }
      }
    }

    for (auto analysis : {AnalysisType::kMax, AnalysisType::kMin})
      for (auto kind : {PathSourceType::kInput, PathSourceType::kRegister})
        for (auto output : {TransType::kRise, TransType::kFall}) {
          candidates.clear();
          extremes.clear();
          const auto add = [&](Candidate candidate) {
            candidates.push_back(candidate);
            const std::string_view tag = candidate.tag;
            const auto found = std::lower_bound(extremes.begin(), extremes.end(), tag,
                                                [](const Extreme& value, std::string_view key) { return value.tag < key; });
            if (found == extremes.end() || found->tag != tag)
              extremes.insert(found, {tag, candidate.arrival, false});
            else if (analysis == AnalysisType::kMax ? candidate.arrival > found->arrival : candidate.arrival < found->arrival)
              found->arrival = candidate.arrival;
          };
          if (hasPathState(sink, analysis, kind, output))
            for (auto& [tag, path] : getPathStateMap(sink, analysis, kind, output))
              if (isFinite(path.get_arrival()))
                add({&path, tag, 0, output, path.get_arrival(), 0.0, true});
          for (auto index : incoming) {
            auto& arc = database.get_arc_list()[index];
            if (isDisableArc(arc) || shouldStopDataPropagation(arc))
              continue;
            auto& source = database.get_timing_point_map().at(arc.get_source_pin());
            for (auto input : {TransType::kRise, TransType::kFall}) {
              if (!hasPathState(source, analysis, kind, input))
                continue;
              const auto outputs = getOutputTransTypeList(arc, analysis, input);
              if (std::find(outputs.begin(), outputs.end(), output) == outputs.end())
                continue;
              const auto delay = getArcDelay(arc, analysis, input, output);
              for (auto& [tag, path] : getPathStateMap(source, analysis, kind, input))
                if (isFinite(path.get_arrival()))
                  add({&path, tag, index, input, roundTime(path.get_arrival() + delay), delay, false});
            }
          }
          for (const auto& candidate : candidates) {
            const std::string_view tag = candidate.tag;
            const auto found = std::lower_bound(extremes.begin(), extremes.end(), tag,
                                                [](const Extreme& value, std::string_view key) { return value.tag < key; });
            if (found->selected || isBetterArrival(found->arrival, candidate.arrival, analysis))
              continue;
            found->selected = true;
            if (candidate.seed)
              continue;
            auto& source = *candidate.source;
            auto& arc = database.get_arc_list()[candidate.arc];
            auto& path = getPathStateMap(sink, analysis, kind, output)[source.get_clock_name()];
            path.set_arrival(candidate.arrival);
            path.set_slew(getDataSlew(sink, analysis, output));
            path.set_start_point(source.get_start_point());
            path.set_predecessor(arc.get_source_pin());
            path.set_predecessor_arc_idx(candidate.arc);
            path.set_predecessor_arc_delay(candidate.delay);
            path.set_launch_time(source.get_launch_time());
            path.set_clock_name(source.get_clock_name());
            path.set_crpr_clock_pin(source.get_crpr_clock_pin());
            path.set_trans_type(output);
            path.set_predecessor_trans_type(candidate.input);
            path.set_crpr_clock_trans_type(source.get_crpr_clock_trans_type());
          }
        }
  }
}

}  // namespace ista
