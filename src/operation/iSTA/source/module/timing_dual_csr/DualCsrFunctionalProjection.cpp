// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrFunctionalProjectionInternal.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

#include "DualCsrStateValidation.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr {
namespace {

const csr::NodeKey& nodeKey(const DualCsrState& state, csr::NodeId id)
{
  return state.nodes.at(id.value()).key;
}

std::optional<csr::NodeKey> projectedNode(
    const DualCsrState& state, const std::optional<csr::NodeId>& id)
{
  if (!id)
    return std::nullopt;
  return nodeKey(state, *id);
}

std::vector<csr::NodeKey> projectedNodes(
    const DualCsrState& state, const std::vector<csr::NodeId>& ids)
{
  std::vector<csr::NodeKey> result;
  result.reserve(ids.size());
  for (const auto id : ids)
    result.push_back(nodeKey(state, id));
  return result;
}

DualCsrProjectedNode projectNode(const csr::NodeRecord& source)
{
  return {
      .key = source.key,
      .direction = source.direction,
      .instance_cell = source.instance_cell,
      .net_name = source.net_name,
      .is_port = source.is_port,
      .is_startpoint = source.is_startpoint,
      .is_endpoint = source.is_endpoint,
      .is_sequential = source.is_sequential,
      .is_clock_pin = source.is_clock_pin,
      .is_data_pin = source.is_data_pin,
      .is_output_pin = source.is_output_pin};
}

DualCsrProjectedArc projectArc(const DualCsrState& state,
                               const csr::ArcSemanticPayload& source)
{
  return {
      .key = source.key,
      .source = nodeKey(state, source.source),
      .sink = nodeKey(state, source.sink),
      .kind = source.kind,
      .is_clock_arc = source.is_clock_arc,
      .is_disabled = source.is_disabled,
      .is_loop_disabled = source.is_loop_disabled,
      .eligibility = source.eligibility,
      .transfer = source.transfer,
      .output_load = source.output_load};
}

DualCsrProjectedInstance projectInstance(
    const DualCsrState& state, const csr::InstanceRecord& source)
{
  return {
      .instance_name = source.instance_name,
      .cell_name = source.cell_name,
      .ordered_pins = projectedNodes(state, source.ordered_pins),
      .is_sequential = source.is_sequential,
      .has_timing_checks = source.has_timing_checks,
      .is_clock_gating = source.is_clock_gating,
      .is_macro = source.is_macro};
}

DualCsrProjectedRawNodeReference projectRawNodeReference(
    const DualCsrState& state, const csr::RawNodeReference& source)
{
  return {source.full_name, projectedNode(state, source.resolved_node)};
}

DualCsrProjectedClockConstraint projectClock(
    const DualCsrState& state, const csr::ClockConstraintModel& source)
{
  DualCsrProjectedClockConstraint result;
  result.map_key = source.map_key;
  result.embedded_clock_name = source.embedded_clock_name;
  result.ordered_sources.reserve(source.ordered_sources.size());
  for (const auto& item : source.ordered_sources)
    result.ordered_sources.push_back(projectRawNodeReference(state, item));
  result.period = source.period;
  result.edges = source.edges;
  result.propagated = source.propagated;
  return result;
}

DualCsrProjectedPortConstraint projectPortConstraint(
    const DualCsrState& state, const csr::PortConstraintModel& source)
{
  return {
      .map_key = source.map_key,
      .embedded_port_name = source.embedded_port_name,
      .resolved_node = projectedNode(state, source.resolved_node),
      .input_delay = source.input_delay,
      .input_transition = source.input_transition,
      .output_delay = source.output_delay,
      .output_load = source.output_load,
      .clock_name = source.clock_name};
}

DualCsrProjectedCaseAnalysis projectCaseAnalysis(
    const DualCsrState& state,
    const csr::CaseAnalysisConstraintModel& source)
{
  return {source.raw_name, projectedNode(state, source.resolved_node),
          source.value};
}

DualCsrProjectedTimingCheckEndpoint projectTimingCheckEndpoint(
    const DualCsrState& state,
    const csr::TimingCheckEndpointModel& source)
{
  return {
      .instance_name = source.instance_name,
      .referenced_check_ordinal = source.referenced_check_ordinal,
      .check_kind = source.check_kind,
      .clock_pin_raw_name = source.clock_pin_raw_name,
      .data_pin_raw_name = source.data_pin_raw_name,
      .clock_pin_full_name = source.clock_pin_full_name,
      .data_pin_full_name = source.data_pin_full_name,
      .resolved_clock_node = projectedNode(
          state, source.resolved_clock_node),
      .resolved_data_node = projectedNode(
          state, source.resolved_data_node)};
}

DualCsrProjectedStartSeed projectStartSeed(
    const DualCsrState& state, const csr::StartSeedModel& source)
{
  return {
      .seed_node = nodeKey(state, source.seed_node),
      .path_start_node = nodeKey(state, source.path_start_node),
      .availability = source.availability,
      .path_source = source.path_source,
      .clock_tag = source.clock_tag,
      .arrival = source.arrival,
      .slew = source.slew,
      .crpr_node = projectedNode(state, source.crpr_node),
      .crpr_transition = source.crpr_transition};
}

DualCsrProjectedEndpointRequired projectEndpointRequired(
    const DualCsrState& state, const csr::EndpointRequiredModel& source)
{
  return {
      .node = nodeKey(state, source.node),
      .clock_name = source.clock_name,
      .clock_period = source.clock_period,
      .output_delay = source.output_delay};
}

DualCsrProjectedCellPinCapacitance projectCellPinCapacitance(
    const DualCsrState& state,
    const csr::CellPinCapacitanceModel& source)
{
  return {
      .node = nodeKey(state, source.node),
      .default_capacitance = source.default_capacitance,
      .transition_capacitance = source.transition_capacitance};
}

template <typename T, typename Projector>
auto projectVector(const std::vector<T>& source, Projector projector)
{
  using Projected = decltype(projector(source.front()));
  std::vector<Projected> result;
  result.reserve(source.size());
  for (const auto& item : source)
    result.push_back(projector(item));
  return result;
}

bool projectedNodeLess(const csr::NodeKey& lhs, const csr::NodeKey& rhs)
{
  return csr::nodeKeyLess(lhs, rhs);
}

DualCsrProjectedReferencePayload projectReferencePayload(
    const DualCsrState& state,
    const detail::CanonicalSemanticIndex& index)
{
  const auto& source = state.reference_payload;
  DualCsrProjectedReferencePayload result;
  result.schema_version = source.schema_version;
  result.analysis_context = source.analysis_context;
  result.analysis_order = source.analysis_order;
  result.path_source_order = source.path_source_order;
  result.transition_order = source.transition_order;
  result.instances = projectVector(
      source.instances, [&](const auto& item) {
        return projectInstance(state, item);
      });
  std::ranges::sort(result.instances, {},
                    &DualCsrProjectedInstance::instance_name);
  result.ordered_roots = projectedNodes(state, source.ordered_root_ids);
  result.ordered_starts = projectedNodes(state, source.ordered_start_ids);
  result.ordered_ends = projectedNodes(state, source.ordered_end_ids);
  result.clock_tags = source.clock_tags;
  result.clocks = projectVector(source.clocks, [&](const auto& item) {
    return projectClock(state, item);
  });
  std::ranges::sort(result.clocks, {},
                    &DualCsrProjectedClockConstraint::map_key);
  result.clock_owners.reserve(index.node_ids.size());
  for (const auto id : index.node_ids)
    result.clock_owners.push_back({
        state.nodes[id].key, source.clock_owner_by_node[id]});
  result.port_constraints = projectVector(
      source.port_constraints, [&](const auto& item) {
        return projectPortConstraint(state, item);
      });
  std::ranges::sort(result.port_constraints, {},
                    &DualCsrProjectedPortConstraint::map_key);
  result.case_analysis_constraints = projectVector(
      source.case_analysis_constraints, [&](const auto& item) {
        return projectCaseAnalysis(state, item);
      });
  std::ranges::sort(result.case_analysis_constraints, {},
                    &DualCsrProjectedCaseAnalysis::raw_name);
  result.timing_check_endpoints = projectVector(
      source.timing_check_endpoints, [&](const auto& item) {
        return projectTimingCheckEndpoint(state, item);
      });
  std::ranges::sort(
      result.timing_check_endpoints,
      [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.instance_name, lhs.referenced_check_ordinal,
                        lhs.clock_pin_full_name, lhs.data_pin_full_name)
               < std::tie(rhs.instance_name, rhs.referenced_check_ordinal,
                          rhs.clock_pin_full_name, rhs.data_pin_full_name);
      });
  result.referenced_library_cells = source.referenced_library_cells;
  std::ranges::sort(result.referenced_library_cells, {},
                    &csr::ReferencedLibraryCellModel::cell_name);
  result.parasitics = source.parasitics;
  std::ranges::sort(result.parasitics.nets, {},
                    &csr::ParasiticNetModel::map_key);
  for (auto& net : result.parasitics.nets) {
    std::ranges::sort(net.nodes, {}, &csr::ParasiticNodeModel::map_key);
    std::ranges::sort(net.resistors, [](const auto& lhs, const auto& rhs) {
      return std::tie(lhs.source, lhs.sink, lhs.resistance)
             < std::tie(rhs.source, rhs.sink, rhs.resistance);
    });
  }
  result.start_seeds = projectVector(
      source.start_seeds, [&](const auto& item) {
        return projectStartSeed(state, item);
      });
  std::ranges::sort(result.start_seeds,
                    [](const auto& lhs, const auto& rhs) {
                      if (projectedNodeLess(lhs.seed_node, rhs.seed_node))
                        return true;
                      if (projectedNodeLess(rhs.seed_node, lhs.seed_node))
                        return false;
                      if (projectedNodeLess(lhs.path_start_node,
                                            rhs.path_start_node))
                        return true;
                      if (projectedNodeLess(rhs.path_start_node,
                                            lhs.path_start_node))
                        return false;
                      return lhs.clock_tag < rhs.clock_tag;
                    });
  result.endpoint_required = projectVector(
      source.endpoint_required, [&](const auto& item) {
        return projectEndpointRequired(state, item);
      });
  std::ranges::sort(result.endpoint_required,
                    [](const auto& lhs, const auto& rhs) {
                      return projectedNodeLess(lhs.node, rhs.node);
                    });
  result.cell_pin_capacitances = projectVector(
      source.cell_pin_capacitances, [&](const auto& item) {
        return projectCellPinCapacitance(state, item);
      });
  std::ranges::sort(result.cell_pin_capacitances,
                    [](const auto& lhs, const auto& rhs) {
                      return projectedNodeLess(lhs.node, rhs.node);
                    });
  result.unsupported_reasons = source.unsupported_reasons;
  std::ranges::sort(result.unsupported_reasons);
  return result;
}

}  // namespace

namespace detail {

CanonicalSemanticIndex makeCanonicalSemanticIndex(
    const DualCsrState& state)
{
  CanonicalSemanticIndex result;
  result.node_ids.resize(state.nodes.size());
  std::iota(result.node_ids.begin(), result.node_ids.end(), 0);
  std::ranges::sort(result.node_ids, [&](std::size_t lhs,
                                         std::size_t rhs) {
    if (csr::nodeKeyLess(state.nodes[lhs].key, state.nodes[rhs].key))
      return true;
    if (csr::nodeKeyLess(state.nodes[rhs].key, state.nodes[lhs].key))
      return false;
    return lhs < rhs;
  });

  for (std::size_t id = 0; id < state.arc_slots.size(); ++id)
    if (state.arc_slots[id].active)
      result.active_arc_ids.push_back(id);
  std::ranges::sort(result.active_arc_ids,
                    [&](std::size_t lhs, std::size_t rhs) {
    const auto& left = state.arc_slots[lhs].payload.key;
    const auto& right = state.arc_slots[rhs].payload.key;
    if (csr::arcKeyLess(left, right))
      return true;
    if (csr::arcKeyLess(right, left))
      return false;
    return lhs < rhs;
  });
  return result;
}

const csr::NodeKey& semanticNodeKey(const DualCsrState& state,
                                    csr::NodeId id)
{
  return nodeKey(state, id);
}

const csr::ArcKey& semanticArcKey(const DualCsrState& state,
                                  csr::ArcId id)
{
  return state.arc_slots.at(id.value()).payload.key;
}

DualCsrFunctionalProjection projectValidatedDualCsrState(
    const DualCsrState& state, const CanonicalSemanticIndex& index)
{
  DualCsrFunctionalProjection projection;
  projection.nodes.reserve(index.node_ids.size());
  for (const auto id : index.node_ids)
    projection.nodes.push_back(projectNode(state.nodes[id]));
  projection.active_arcs.reserve(index.active_arc_ids.size());
  for (const auto id : index.active_arc_ids)
    projection.active_arcs.push_back(
        projectArc(state, state.arc_slots[id].payload));
  projection.reference_payload = projectReferencePayload(state, index);
  return projection;
}

}  // namespace detail

DualCsrProjectionAttempt projectDualCsrState(const DualCsrState& state)
{
  DualCsrProjectionAttempt attempt;
  attempt.violations = validateDualCsrState(state);
  if (!attempt.violations.empty())
    return attempt;
  const auto index = detail::makeCanonicalSemanticIndex(state);
  attempt.projection = detail::projectValidatedDualCsrState(state, index);
  return attempt;
}

}  // namespace ista::timing_dual_csr
