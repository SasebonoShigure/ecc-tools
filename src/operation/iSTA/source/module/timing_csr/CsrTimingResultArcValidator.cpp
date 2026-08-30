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
#include <algorithm>

#include "CsrTimingResultValidatorInternal.hpp"

namespace ista::csr::detail {

using ResultClass = CsrTimingResultViolationClass;

void validateArcResultSoa(const CsrReferenceInput& input, const ArcResultSoa& result, const OptionalSoa<double>& data_slews, TimingResultViolations& errors)
{
  const std::size_t arc_count = input.arcs.size();
  const auto finite_arc = [&](double value, std::size_t) {
    if (!isFinite(value))
      reject(errors, ResultClass::kNumeric, "result.arcs", "present delay must be finite");
  };
  const bool delays_ok = validateOptionalSoa(result.delays, arc_count, true, errors, ResultClass::kShape, "result.arcs.delays", finite_arc);
  const bool min_delays_ok = validateOptionalSoa(result.min_delays, arc_count, true, errors, ResultClass::kShape, "result.arcs.min_delays", finite_arc);
  const bool max_delays_ok = validateOptionalSoa(result.max_delays, arc_count, true, errors, ResultClass::kShape, "result.arcs.max_delays", finite_arc);
  std::size_t transition_lanes = 0;
  std::size_t graph_lanes = 0;
  std::size_t type_lanes = 0;
  checkedProduct(arc_count, kAnalysisCount * kTransitionCount, transition_lanes, errors, ResultClass::kShape, "result.arcs.transition_delays");
  checkedProduct(arc_count, kAnalysisCount * kTransitionCount * kTransitionCount, graph_lanes, errors, ResultClass::kShape, "result.arcs.graph_delays");
  checkedProduct(arc_count, kTransitionCount, type_lanes, errors, ResultClass::kShape, "result.arcs.transition_types");
  const bool transition_ok
      = validateOptionalSoa(result.transition_delays, transition_lanes, true, errors, ResultClass::kShape, "result.arcs.transition_delays", finite_arc);
  const auto all_present = [](const auto& values) { return std::ranges::all_of(values.presence, [](std::uint8_t presence) { return presence == 1; }); };
  if (delays_ok && min_delays_ok && max_delays_ok && transition_ok && all_present(result.delays) && all_present(result.min_delays)
      && all_present(result.max_delays) && all_present(result.transition_delays)) {
    for (std::size_t arc = 0; arc < arc_count; ++arc) {
      const std::size_t base = arc * kAnalysisCount * kTransitionCount;
      const double expected_max = std::max(result.transition_delays.values[base], result.transition_delays.values[base + 1]);
      const double expected_min
          = std::min(result.transition_delays.values[base + kTransitionCount], result.transition_delays.values[base + kTransitionCount + 1]);
      if (result.max_delays.values[arc] != expected_max || result.min_delays.values[arc] != expected_min || result.delays.values[arc] != expected_max)
        reject(errors, ResultClass::kNumeric, "result.arcs.delays", "scalar delays must exactly match initialized transition-delay extrema");
    }
  }
  const bool io_ok
      = validateOptionalSoa(result.input_output_delays, graph_lanes, false, errors, ResultClass::kShape, "result.arcs.input_output_delays", finite_arc);
  const bool graph_ok = validateOptionalSoa(result.graph_delays, graph_lanes, false, errors, ResultClass::kShape, "result.arcs.graph_delays", finite_arc);
  const bool types_ok = validateOptionalSoa(
      result.transition_types, type_lanes, false, errors, ResultClass::kShape, "result.arcs.transition_types", [&](Transition value, std::size_t) {
        if (!knownEnum(value) || value == Transition::kNone)
          reject(errors, ResultClass::kIdMapping, "result.arcs.transition_types", "present transition must be rise or fall");
      });
  std::size_t data_lanes = 0;
  const bool data_shape = checkedProduct(input.nodes.size(), kAnalysisCount * kTransitionCount, data_lanes, errors, ResultClass::kShape, "result.data_slews")
                          && data_slews.presence.size() == data_lanes && data_slews.values.size() == data_lanes;
  if (input.parasitics.nets.empty() && data_shape && delays_ok && min_delays_ok && max_delays_ok && transition_ok && io_ok && graph_ok && types_ok) {
    for (std::size_t arc = 0; arc < arc_count; ++arc) {
      if (input.arcs[arc].kind != ArcKind::kNet)
        continue;
      if (result.delays.values[arc] != 0 || result.min_delays.values[arc] != 0 || result.max_delays.values[arc] != 0)
        reject(errors, ResultClass::kNumeric, "result.arcs.delays", "no-SPEF net scalar delays must be zero");
      for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
        for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
          const std::size_t transition_lane = (arc * kAnalysisCount + analysis) * kTransitionCount + input_transition;
          if (result.transition_delays.presence[transition_lane] != 1)
            reject(errors, ResultClass::kShape, "result.arcs.transition_delays", "no-SPEF net transition delays must be present zero");
          else if (result.transition_delays.values[transition_lane] != 0)
            reject(errors, ResultClass::kNumeric, "result.arcs.transition_delays", "no-SPEF net transition delays must be present zero");
          const std::size_t type_lane = arc * kTransitionCount + input_transition;
          if (result.transition_types.presence[type_lane] != 1)
            reject(errors, ResultClass::kShape, "result.arcs.transition_types", "no-SPEF net transitions must be identity mappings");
          else if (result.transition_types.values[type_lane] != input.transition_order[input_transition])
            reject(errors, ResultClass::kIdMapping, "result.arcs.transition_types", "no-SPEF net transitions must be identity mappings");
          for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
            const std::size_t lane = transition_lane * kTransitionCount + output_transition;
            const std::uint8_t same_transition = input_transition == output_transition ? 1 : 0;
            const std::size_t source_lane = (input.arcs[arc].source.value() * kAnalysisCount + analysis) * kTransitionCount + input_transition;
            const std::uint8_t graph_presence = input.arcs[arc].eligibility.data_slew && data_slews.presence[source_lane] == 1 ? same_transition : 0;
            if (result.input_output_delays.presence[lane] != same_transition || result.graph_delays.presence[lane] != graph_presence) {
              reject(errors, ResultClass::kShape, "result.arcs.graph_delays", "no-SPEF net maps must contain only eligible present-zero identity lanes");
            } else if ((same_transition != 0 && result.input_output_delays.values[lane] != 0)
                       || (graph_presence != 0 && result.graph_delays.values[lane] != 0)) {
              reject(errors, ResultClass::kNumeric, "result.arcs.graph_delays", "no-SPEF net maps must contain only eligible present-zero identity lanes");
            }
          }
        }
      }
    }
  }

  const std::size_t timing_entries = result.timing_arc_indices.size();
  const bool rows_ok
      = validateOffsets(result.timing_arc_row_offsets, arc_count, timing_entries, errors, ResultClass::kShape, "result.arcs.timing_arc_row_offsets");
  if (rows_ok) {
    for (std::size_t arc = 0; arc < arc_count; ++arc) {
      const std::uint64_t begin = result.timing_arc_row_offsets[arc];
      const std::uint64_t end = result.timing_arc_row_offsets[arc + 1];
      if (end - begin != input.arcs[arc].transfer.children.size()) {
        reject(errors, ResultClass::kShape, "result.arcs.timing_arc_indices", "row cardinality differs from input children");
        continue;
      }
      for (std::size_t child = 0; child < input.arcs[arc].transfer.children.size(); ++child)
        if (result.timing_arc_indices[begin + child] != input.arcs[arc].transfer.children[child].signature.arc_index)
          reject(errors, ResultClass::kIdMapping, "result.arcs.timing_arc_indices", "row must exactly preserve child arc-index order");
    }
  }
  std::size_t timing_lanes = 0;
  checkedProduct(timing_entries, kAnalysisCount * kTransitionCount * kTransitionCount, timing_lanes, errors, ResultClass::kShape,
                 "result.arcs.timing_arc_delays");
  const bool timing_ok
      = validateOptionalSoa(result.timing_arc_delays, timing_lanes, false, errors, ResultClass::kShape, "result.arcs.timing_arc_delays", finite_arc);
  const bool fallback_ok = validateOptionalSoa(result.timing_arc_fallbacks, timing_lanes, false, errors, ResultClass::kShape,
                                               "result.arcs.timing_arc_fallbacks", [&](std::uint8_t value, std::size_t) {
                                                 if (value > 1)
                                                   reject(errors, ResultClass::kShape, "result.arcs.timing_arc_fallbacks", "fallback must be zero or one");
                                               });
  if (timing_ok && fallback_ok && result.timing_arc_delays.presence != result.timing_arc_fallbacks.presence)
    reject(errors, ResultClass::kShape, "result.arcs.timing_arc_fallbacks", "delay and fallback presence must match");
}

}  // namespace ista::csr::detail
