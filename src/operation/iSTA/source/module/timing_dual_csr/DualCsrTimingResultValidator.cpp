// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTimingResultValidator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "CsrReferenceValidatorDetail.hpp"
#include "DualCsrStateValidation.hpp"
#include "DualCsrTimingResultSemantics.hpp"
#include "DualCsrValidationInternal.hpp"

namespace ista::timing_dual_csr {
namespace {

using detail::reject;

template <typename T>
bool canonicalAbsent(const T& value)
{
  return csr::detail::canonicalAbsent(value);
}

bool checkedProduct(std::size_t lhs, std::size_t rhs,
                    std::size_t& product, const std::string& field,
                    DualCsrViolations& violations)
{
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    reject(violations, DualCsrViolationCode::kTimingResult, field,
           "lane count overflows size_t");
    return false;
  }
  product = lhs * rhs;
  return true;
}

template <typename T>
bool validateSoa(const csr::OptionalSoa<T>& values, std::size_t expected,
                 const std::string& field, DualCsrViolations& violations)
{
  if (values.presence.size() != expected || values.values.size() != expected) {
    reject(violations, DualCsrViolationCode::kTimingResult, field,
           "presence and value lanes must have the expected size");
    return false;
  }
  bool valid = true;
  for (std::size_t lane = 0; lane < expected; ++lane) {
    if (values.presence[lane] > 1) {
      reject(violations, DualCsrViolationCode::kTimingResult, field,
             "presence must be zero or one");
      valid = false;
      continue;
    }
    if (values.presence[lane] == 0 && !canonicalAbsent(values.values[lane])) {
      reject(violations, DualCsrViolationCode::kTimingResult, field,
             "absent lane must contain its canonical default");
      valid = false;
    }
  }
  return valid;
}

template <typename T, typename Predicate>
bool validateSoaDomain(const csr::OptionalSoa<T>& values,
                       std::size_t expected, const std::string& field,
                       Predicate predicate, const std::string& message,
                       DualCsrViolations& violations)
{
  if (!validateSoa(values, expected, field, violations))
    return false;
  bool valid = true;
  for (std::size_t lane = 0; lane < expected; ++lane)
    if (values.presence[lane] == 1 && !predicate(values.values[lane])) {
      reject(violations, DualCsrViolationCode::kTimingResult, field,
             message);
      valid = false;
    }
  return valid;
}

template <typename T>
bool requirePresent(const csr::OptionalSoa<T>& values,
                    const std::vector<std::uint8_t>& expected,
                    const std::string& field,
                    DualCsrViolations& violations)
{
  if (!validateSoa(values, expected.size(), field, violations))
    return false;
  bool valid = true;
  for (std::size_t lane = 0; lane < expected.size(); ++lane)
    if (values.presence[lane] != expected[lane]) {
      reject(violations, DualCsrViolationCode::kTimingResult, field,
             "presence does not match active/required lanes");
      valid = false;
    }
  return valid;
}

template <typename T, typename Predicate>
bool requirePresentDomain(const csr::OptionalSoa<T>& values,
                          const std::vector<std::uint8_t>& expected,
                          const std::string& field, Predicate predicate,
                          const std::string& message,
                          DualCsrViolations& violations)
{
  const bool domain_valid = validateSoaDomain(
      values, expected.size(), field, predicate, message, violations);
  if (!domain_valid)
    return false;
  bool presence_valid = true;
  for (std::size_t lane = 0; lane < expected.size(); ++lane)
    if (values.presence[lane] != expected[lane]) {
      reject(violations, DualCsrViolationCode::kTimingResult, field,
             "presence does not match active/required lanes");
      presence_valid = false;
    }
  return presence_valid;
}

bool validateOffsets(const std::vector<std::uint64_t>& offsets,
                     std::size_t rows, std::size_t entries,
                     const std::string& field,
                     DualCsrViolations& violations)
{
  if (offsets.size() != rows + 1 || offsets.empty()
      || offsets.front() != 0 || offsets.back() != entries) {
    reject(violations, DualCsrViolationCode::kTimingResult, field,
           "offsets must delimit exactly the requested rows and entries");
    return false;
  }
  for (std::size_t row = 0; row < rows; ++row)
    if (offsets[row] > offsets[row + 1]) {
      reject(violations, DualCsrViolationCode::kTimingResult, field,
             "offsets must be monotonic");
      return false;
    }
  return true;
}

void validateLevelization(const DualCsrState& state,
                          const csr::CsrTimingResult& result,
                          DualCsrViolations& violations)
{
  const std::size_t node_count = state.nodes.size();
  if (result.level_nodes.size() != node_count
      || result.node_to_level.size() != node_count
      || result.timing_order != result.level_nodes) {
    reject(violations, DualCsrViolationCode::kTimingResult,
           "result.levelization",
           "level nodes, node-to-level, and timing order must cover all nodes");
    return;
  }
  if (result.level_offsets.empty() || result.level_offsets.front() != 0
      || result.level_offsets.back() != node_count) {
    reject(violations, DualCsrViolationCode::kTimingResult,
           "result.level_offsets", "level offsets must span every node");
    return;
  }
  if (node_count == 0 && result.level_offsets.size() != 1) {
    reject(violations, DualCsrViolationCode::kTimingResult,
           "result.level_offsets", "an empty graph has only the zero offset");
    return;
  }

  std::vector<std::uint8_t> seen(node_count, 0);
  for (std::size_t level = 0; level + 1 < result.level_offsets.size();
       ++level) {
    const auto begin = result.level_offsets[level];
    const auto end = result.level_offsets[level + 1];
    if (begin >= end || end > node_count) {
      reject(violations, DualCsrViolationCode::kTimingResult,
             "result.level_offsets",
             "nonempty graphs require nonempty monotonic levels");
      return;
    }
    for (std::uint64_t position = begin; position < end; ++position) {
      const auto node = result.level_nodes[position].value();
      if (node >= node_count || seen[node] != 0
          || result.node_to_level[node] != level + 1) {
        reject(violations, DualCsrViolationCode::kTimingResult,
               "result.level_nodes",
               "level nodes must be a permutation consistent with node-to-level");
        return;
      }
      seen[node] = 1;
    }
  }
  for (const auto& slot : state.arc_slots) {
    if (!slot.active || !slot.payload.eligibility.levelization)
      continue;
    if (result.node_to_level[slot.payload.source.value()]
        >= result.node_to_level[slot.payload.sink.value()])
      reject(violations, DualCsrViolationCode::kTimingResult,
             "result.node_to_level",
             "every active levelizing arc must advance to a later level");
  }
}

void validatePredecessorArcs(const csr::OptionalSoa<csr::ArcId>& values,
                             const DualCsrState& state,
                             const std::string& field,
                             DualCsrViolations& violations)
{
  for (std::size_t lane = 0; lane < values.presence.size(); ++lane) {
    if (values.presence[lane] == 0)
      continue;
    const auto arc = values.values[lane].value();
    if (arc >= state.arc_slots.size() || !state.arc_slots[arc].active)
      reject(violations, DualCsrViolationCode::kTimingResult, field,
             "predecessor ArcId must identify an active stable slot");
  }
}

void validateNodeAndPathResults(const DualCsrState& state,
                                const csr::CsrTimingResult& result,
                                DualCsrViolations& violations)
{
  const std::size_t node_count = state.nodes.size();
  const std::vector<std::uint8_t> required_nodes(node_count, 1);
  requirePresent(result.nodes.is_clock_point, required_nodes,
                 "result.nodes.is_clock_point", violations);
  requirePresent(result.nodes.arrivals, required_nodes,
                 "result.nodes.arrivals", violations);
  requirePresent(result.nodes.requireds, required_nodes,
                 "result.nodes.requireds", violations);
  requirePresent(result.nodes.slacks, required_nodes,
                 "result.nodes.slacks", violations);
  requirePresent(result.nodes.launches, required_nodes,
                 "result.nodes.launches", violations);
  validateSoa(result.nodes.clock_names, node_count,
              "result.nodes.clock_names", violations);
  const bool predecessor_nodes_ok
      = validateSoa(result.nodes.predecessor_nodes, node_count,
                    "result.nodes.predecessor_nodes", violations);
  const bool predecessor_arcs_ok
      = validateSoa(result.nodes.predecessor_arcs, node_count,
                    "result.nodes.predecessor_arcs", violations);
  if (predecessor_nodes_ok)
    for (std::size_t lane = 0; lane < node_count; ++lane)
      if (result.nodes.predecessor_nodes.presence[lane] != 0
          && result.nodes.predecessor_nodes.values[lane].value()
                 >= node_count)
        reject(violations, DualCsrViolationCode::kTimingResult,
               "result.nodes.predecessor_nodes",
               "predecessor NodeId is out of range");
  if (predecessor_arcs_ok)
    validatePredecessorArcs(result.nodes.predecessor_arcs, state,
                            "result.nodes.predecessor_arcs", violations);

  std::size_t node_lanes = 0;
  if (!checkedProduct(node_count,
                      csr::kAnalysisCount * csr::kTransitionCount,
                      node_lanes, "result.node_lanes", violations))
    return;
  const auto validate_clock = [&](const csr::ClockResultSoa& clock,
                                  const std::string& field) {
    validateSoa(clock.arrivals, node_lanes, field + ".arrivals", violations);
    validateSoa(clock.slews, node_lanes, field + ".slews", violations);
    validateSoa(clock.predecessor_nodes, node_lanes,
                field + ".predecessor_nodes", violations);
    validateSoa(clock.predecessor_arc_delays, node_lanes,
                field + ".predecessor_arc_delays", violations);
    validateSoa(clock.predecessor_transitions, node_lanes,
                field + ".predecessor_transitions", violations);
  };
  validate_clock(result.effective_clock, "result.effective_clock");
  validate_clock(result.physical_clock, "result.physical_clock");
  validateSoa(result.data_slews, node_lanes, "result.data_slews",
              violations);

  std::size_t path_rows = 0;
  if (!checkedProduct(node_count,
                      csr::kAnalysisCount * csr::kPathSourceCount
                          * csr::kTransitionCount,
                      path_rows, "result.paths", violations))
    return;
  const std::size_t entries = result.paths.clock_tags.presence.size();
  validateOffsets(result.paths.row_offsets, path_rows, entries,
                  "result.paths.row_offsets", violations);
  const auto required_entries = std::vector<std::uint8_t>(entries, 1);
  requirePresent(result.paths.clock_tags, required_entries,
                 "result.paths.clock_tags", violations);
  requirePresent(result.paths.arrivals, required_entries,
                 "result.paths.arrivals", violations);
  requirePresent(result.paths.slews, required_entries,
                 "result.paths.slews", violations);
  requirePresent(result.paths.launches, required_entries,
                 "result.paths.launches", violations);
  requirePresent(result.paths.start_nodes, required_entries,
                 "result.paths.start_nodes", violations);
  validateSoa(result.paths.predecessor_nodes, entries,
              "result.paths.predecessor_nodes", violations);
  const bool path_arcs_ok
      = validateSoa(result.paths.predecessor_arcs, entries,
                    "result.paths.predecessor_arcs", violations);
  validateSoa(result.paths.predecessor_arc_delays, entries,
              "result.paths.predecessor_arc_delays", violations);
  validateSoa(result.paths.predecessor_transitions, entries,
              "result.paths.predecessor_transitions", violations);
  validateSoa(result.paths.crpr_nodes, entries,
              "result.paths.crpr_nodes", violations);
  validateSoa(result.paths.crpr_transitions, entries,
              "result.paths.crpr_transitions", violations);
  if (path_arcs_ok)
    validatePredecessorArcs(result.paths.predecessor_arcs, state,
                            "result.paths.predecessor_arcs", violations);
}

void validateInactiveArcLanes(const csr::OptionalSoa<double>& values,
                              const std::vector<std::uint8_t>& active,
                              std::size_t width, const std::string& field,
                              DualCsrViolations& violations)
{
  if (!validateSoa(values, active.size() * width, field, violations))
    return;
  for (std::size_t arc = 0; arc < active.size(); ++arc)
    if (active[arc] == 0)
      for (std::size_t lane = arc * width; lane < (arc + 1) * width; ++lane)
        if (values.presence[lane] != 0)
          reject(violations, DualCsrViolationCode::kTimingResult, field,
                 "inactive ArcId lanes must be absent");
}

void validateArcResults(const DualCsrState& state,
                        const DualCsrTimingResult& result,
                        DualCsrViolations& violations)
{
  const std::size_t arc_slots = state.arc_slots.size();
  const auto& arcs = result.values.arcs;
  requirePresent(arcs.delays, result.active_arc_mask,
                 "result.arcs.delays", violations);
  requirePresent(arcs.min_delays, result.active_arc_mask,
                 "result.arcs.min_delays", violations);
  requirePresent(arcs.max_delays, result.active_arc_mask,
                 "result.arcs.max_delays", violations);

  constexpr std::size_t transition_width
      = csr::kAnalysisCount * csr::kTransitionCount;
  constexpr std::size_t graph_width
      = transition_width * csr::kTransitionCount;
  constexpr std::size_t type_width = csr::kTransitionCount;
  std::vector<std::uint8_t> active_transitions(arc_slots * transition_width);
  for (std::size_t arc = 0; arc < arc_slots; ++arc)
    for (std::size_t lane = arc * transition_width;
         lane < (arc + 1) * transition_width; ++lane)
      active_transitions[lane] = result.active_arc_mask[arc];
  requirePresent(arcs.transition_delays, active_transitions,
                 "result.arcs.transition_delays", violations);
  validateInactiveArcLanes(arcs.input_output_delays,
                           result.active_arc_mask, graph_width,
                           "result.arcs.input_output_delays", violations);
  validateInactiveArcLanes(arcs.graph_delays, result.active_arc_mask,
                           graph_width, "result.arcs.graph_delays",
                           violations);
  if (validateSoa(arcs.transition_types, arc_slots * type_width,
                  "result.arcs.transition_types", violations)) {
    for (std::size_t arc = 0; arc < arc_slots; ++arc)
      for (std::size_t lane = arc * type_width;
           lane < (arc + 1) * type_width; ++lane) {
        const auto presence = arcs.transition_types.presence[lane];
        const auto value = arcs.transition_types.values[lane];
        if (result.active_arc_mask[arc] == 0 && presence != 0)
          reject(violations, DualCsrViolationCode::kTimingResult,
                 "result.arcs.transition_types",
                 "inactive ArcId lanes must be absent");
        if (presence != 0 && value != csr::Transition::kRise
            && value != csr::Transition::kFall)
          reject(violations, DualCsrViolationCode::kTimingResult,
                 "result.arcs.transition_types",
                 "present transition must be rise or fall");
      }
  }

  const bool rows_ok = validateOffsets(
      arcs.timing_arc_row_offsets, arc_slots,
      arcs.timing_arc_indices.size(), "result.arcs.timing_arc_row_offsets",
      violations);
  if (rows_ok) {
    for (std::size_t arc = 0; arc < arc_slots; ++arc) {
      const auto begin = arcs.timing_arc_row_offsets[arc];
      const auto end = arcs.timing_arc_row_offsets[arc + 1];
      const auto& children = state.arc_slots[arc].payload.transfer.children;
      if (result.active_arc_mask[arc] == 0) {
        if (begin != end)
          reject(violations, DualCsrViolationCode::kTimingResult,
                 "result.arcs.timing_arc_row_offsets",
                 "inactive ArcId timing-arc row must be empty");
        continue;
      }
      if (end - begin != children.size()) {
        reject(violations, DualCsrViolationCode::kTimingResult,
               "result.arcs.timing_arc_indices",
               "active timing-arc row must preserve child cardinality");
        continue;
      }
      for (std::size_t child = 0; child < children.size(); ++child)
        if (arcs.timing_arc_indices[begin + child]
            != children[child].signature.arc_index)
          reject(violations, DualCsrViolationCode::kTimingResult,
                 "result.arcs.timing_arc_indices",
                 "active timing-arc row must preserve child order");
    }
  }
  std::size_t timing_lanes = 0;
  if (checkedProduct(arcs.timing_arc_indices.size(), graph_width,
                     timing_lanes, "result.arcs.timing_arc_delays",
                     violations)) {
    const bool delays_ok = validateSoa(
        arcs.timing_arc_delays, timing_lanes,
        "result.arcs.timing_arc_delays", violations);
    const bool fallbacks_ok = validateSoa(
        arcs.timing_arc_fallbacks, timing_lanes,
        "result.arcs.timing_arc_fallbacks", violations);
    if (delays_ok && fallbacks_ok
        && arcs.timing_arc_delays.presence
               != arcs.timing_arc_fallbacks.presence)
      reject(violations, DualCsrViolationCode::kTimingResult,
             "result.arcs.timing_arc_fallbacks",
             "timing-arc delay and fallback presence must match");
    if (fallbacks_ok)
      for (const auto fallback : arcs.timing_arc_fallbacks.values)
        if (fallback > 1)
          reject(violations, DualCsrViolationCode::kTimingResult,
                 "result.arcs.timing_arc_fallbacks",
                 "fallback value must be zero or one");
  }
}

}  // namespace

DualCsrViolations validateDualCsrTimingResult(
    const DualCsrState& state, const DualCsrTimingResult& result)
{
  DualCsrViolations violations = validateDualCsrState(state);
  if (!violations.empty())
    return violations;
  if (result.values.schema_version != state.reference_payload.schema_version)
    reject(violations, DualCsrViolationCode::kTimingResult,
           "result.schema_version", "schema must match the Dual state payload");
  if (!(result.values.source_identity
        == state.reference_payload.source_identity))
    reject(violations, DualCsrViolationCode::kTimingResult,
           "result.source_identity",
           "source identity must match the installed Dual state");

  if (result.active_arc_mask.size() != state.arc_slots.size()) {
    reject(violations, DualCsrViolationCode::kTimingResult,
           "result.active_arc_mask",
           "active mask must contain one lane per stable ArcId");
    return violations;
  }
  for (std::size_t arc = 0; arc < state.arc_slots.size(); ++arc) {
    const auto expected = state.arc_slots[arc].active ? 1U : 0U;
    if (result.active_arc_mask[arc] != expected)
      reject(violations, DualCsrViolationCode::kTimingResult,
             "result.active_arc_mask",
             "active mask must exactly match logical arc activity");
  }

  validateLevelization(state, result.values, violations);
  validateNodeAndPathResults(state, result.values, violations);
  validateArcResults(state, result, violations);
  detail::validateDualCsrTimingSemantics(state, result, violations);
  return violations;
}

}  // namespace ista::timing_dual_csr
