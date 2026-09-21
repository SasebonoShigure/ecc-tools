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
#include "CsrRealDifferentialReferenceEncoding.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "CsrRealDifferentialProbeSupport.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::real_diff {
namespace {

[[noreturn]] void fail(std::string_view message)
{
  throw std::runtime_error("real differential reference encoding: " + std::string(message));
}

void require(bool condition, std::string_view message)
{
  if (!condition) {
    fail(message);
  }
}

std::size_t analysisIndex(AnalysisMode analysis)
{
  switch (analysis) {
    case AnalysisMode::kMax:
      return 0;
    case AnalysisMode::kMin:
      return 1;
  }
  fail("unknown analysis mode");
}

std::size_t transitionIndex(Transition transition)
{
  switch (transition) {
    case Transition::kRise:
      return 0;
    case Transition::kFall:
      return 1;
    case Transition::kNone:
      break;
  }
  fail("none/unknown transition cannot identify a lane");
}

const NodeKey& nodeKey(const CsrReferenceInput& input, NodeId id)
{
  require(id.value() < input.nodes.size(), "NodeId is out of range");
  require(input.nodes[id.value()].id == id, "NodeId does not identify its canonical record");
  return input.nodes[id.value()].key;
}

const ArcKey& arcKey(const CsrReferenceInput& input, ArcId id)
{
  require(id.value() < input.arcs.size(), "ArcId is out of range");
  require(input.arcs[id.value()].id == id, "ArcId does not identify its canonical record");
  return input.arcs[id.value()].key;
}

template <typename T>
const T& requiredValue(const OptionalSoa<T>& values, std::size_t index, std::string_view label)
{
  require(index < values.presence.size() && index < values.values.size(), label);
  require(values.presence[index] != 0, label);
  return values.values[index];
}

template <typename T, typename Convert>
void importOptional(const OptionalSoa<T>& source, std::size_t flat_index, std::optional<std::invoke_result_t<Convert, const T&>>& destination, Convert convert)
{
  require(flat_index < source.presence.size() && flat_index < source.values.size(), "optional result lane is out of range");
  if (source.presence[flat_index] != 0) {
    destination = convert(source.values[flat_index]);
  }
}

std::size_t nodeLane(std::size_t node, std::size_t analysis, std::size_t transition)
{
  return (node * kAnalysisCount + analysis) * kTransitionCount + transition;
}

std::size_t pathRow(std::size_t node, std::size_t analysis, std::size_t source, std::size_t transition)
{
  return ((node * kAnalysisCount + analysis) * kPathSourceCount + source) * kTransitionCount + transition;
}

std::size_t arcTransitionLane(std::size_t arc, std::size_t analysis, std::size_t input_transition)
{
  return (arc * kAnalysisCount + analysis) * kTransitionCount + input_transition;
}

std::size_t arcIoLane(std::size_t arc, std::size_t analysis, std::size_t input_transition, std::size_t output_transition)
{
  return arcTransitionLane(arc, analysis, input_transition) * kTransitionCount + output_transition;
}

std::size_t timingArcLane(std::size_t child, std::size_t analysis, std::size_t input_transition, std::size_t output_transition)
{
  return ((child * kAnalysisCount + analysis) * kTransitionCount + input_transition) * kTransitionCount + output_transition;
}

NeutralClockState encodeClockState(const CsrReferenceInput& input, const ClockResultSoa& source, std::size_t node)
{
  NeutralClockState state;
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    const std::size_t neutral_analysis = analysisIndex(input.analysis_order[analysis]);
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      const std::size_t neutral_transition = transitionIndex(input.transition_order[transition]);
      const std::size_t lane = nodeLane(node, analysis, transition);
      importOptional(source.arrivals, lane, state.arrivals[neutral_analysis][neutral_transition], [](const double& value) { return value; });
      importOptional(source.slews, lane, state.slews[neutral_analysis][neutral_transition], [](const double& value) { return value; });
      importOptional(source.predecessor_nodes, lane, state.predecessor_nodes[neutral_analysis][neutral_transition],
                     [&](const NodeId& value) { return nodeKey(input, value); });
      importOptional(source.predecessor_arc_delays, lane, state.predecessor_arc_delays[neutral_analysis][neutral_transition],
                     [](const double& value) { return value; });
      importOptional(source.predecessor_transitions, lane, state.predecessor_transitions[neutral_analysis][neutral_transition],
                     [](const Transition& value) { return value; });
    }
  }
  return state;
}

bool byteLess(std::string_view lhs, std::string_view rhs)
{
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                      [](char left, char right) { return static_cast<unsigned char>(left) < static_cast<unsigned char>(right); });
}

bool pathLess(const NeutralPath& lhs, const NeutralPath& rhs)
{
  if (token(lhs.analysis) != token(rhs.analysis)) {
    return token(lhs.analysis) < token(rhs.analysis);
  }
  if (token(lhs.source) != token(rhs.source)) {
    return token(lhs.source) < token(rhs.source);
  }
  if (token(lhs.lane_transition) != token(rhs.lane_transition)) {
    return token(lhs.lane_transition) < token(rhs.lane_transition);
  }
  return byteLess(lhs.clock_tag, rhs.clock_tag);
}

void appendPaths(const CsrReferenceInput& input, const CsrTimingResult& result, std::size_t node, std::vector<NeutralPath>& destination)
{
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t source = 0; source < kPathSourceCount; ++source) {
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        const std::size_t row = pathRow(node, analysis, source, transition);
        require(row + 1 < result.paths.row_offsets.size(), "path row offset is out of range");
        const std::uint64_t begin = result.paths.row_offsets[row];
        const std::uint64_t end = result.paths.row_offsets[row + 1];
        require(begin <= end && end <= result.paths.clock_tags.values.size(), "path row extent is invalid");
        for (std::uint64_t entry64 = begin; entry64 < end; ++entry64) {
          const std::size_t entry = static_cast<std::size_t>(entry64);
          NeutralPath path;
          path.analysis = input.analysis_order[analysis];
          path.source = input.path_source_order[source];
          path.lane_transition = input.transition_order[transition];
          const ClockTagId clock_tag = requiredValue(result.paths.clock_tags, entry, "path clock tag is absent/out of range");
          require(clock_tag.value() < input.clock_tags.size(), "path clock tag id is out of range");
          path.clock_tag = input.clock_tags[clock_tag.value()];
          path.clock_name = path.clock_tag;
          path.arrival = requiredValue(result.paths.arrivals, entry, "path arrival is absent/out of range");
          path.slew = requiredValue(result.paths.slews, entry, "path slew is absent/out of range");
          path.launch = requiredValue(result.paths.launches, entry, "path launch is absent/out of range");
          path.start_node = nodeKey(input, requiredValue(result.paths.start_nodes, entry, "path start node is absent/out of range"));
          path.state_transition = path.lane_transition;
          importOptional(result.paths.predecessor_nodes, entry, path.predecessor_node, [&](const NodeId& value) { return nodeKey(input, value); });
          importOptional(result.paths.predecessor_arcs, entry, path.predecessor_arc, [&](const ArcId& value) { return arcKey(input, value); });
          if (entry < result.paths.predecessor_arc_delays.presence.size() && result.paths.predecessor_arc_delays.presence[entry] != 0) {
            path.predecessor_arc_delay = result.paths.predecessor_arc_delays.values.at(entry);
          }
          if (entry < result.paths.predecessor_transitions.presence.size() && result.paths.predecessor_transitions.presence[entry] != 0) {
            path.predecessor_transition = result.paths.predecessor_transitions.values.at(entry);
          }
          importOptional(result.paths.crpr_nodes, entry, path.crpr_node, [&](const NodeId& value) { return nodeKey(input, value); });
          if (entry < result.paths.crpr_transitions.presence.size() && result.paths.crpr_transitions.presence[entry] != 0) {
            path.crpr_transition = result.paths.crpr_transitions.values.at(entry);
          }
          destination.push_back(std::move(path));
        }
      }
    }
  }
  std::sort(destination.begin(), destination.end(), pathLess);
}

NeutralNode encodeNode(const CsrReferenceInput& input, const CsrTimingResult& result, std::size_t node_index)
{
  NeutralNode node;
  node.key = input.nodes[node_index].key;
  require(result.node_to_level.at(node_index) <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()), "node level exceeds int32");
  node.level = static_cast<std::int32_t>(result.node_to_level[node_index]);
  node.is_clock_point = requiredValue(result.nodes.is_clock_point, node_index, "node clock flag is absent/out of range") != 0;
  node.arrival = requiredValue(result.nodes.arrivals, node_index, "node arrival is absent/out of range");
  node.required = requiredValue(result.nodes.requireds, node_index, "node required is absent/out of range");
  node.slack = requiredValue(result.nodes.slacks, node_index, "node slack is absent/out of range");
  node.launch = requiredValue(result.nodes.launches, node_index, "node launch is absent/out of range");
  importOptional(result.nodes.clock_names, node_index, node.clock_name, [](const std::string& value) { return value; });
  importOptional(result.nodes.predecessor_nodes, node_index, node.predecessor_node, [&](const NodeId& value) { return nodeKey(input, value); });
  importOptional(result.nodes.predecessor_arcs, node_index, node.predecessor_arc, [&](const ArcId& value) { return arcKey(input, value); });
  node.effective_clock = encodeClockState(input, result.effective_clock, node_index);
  node.physical_clock = encodeClockState(input, result.physical_clock, node_index);
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      const std::size_t lane = nodeLane(node_index, analysis, transition);
      importOptional(result.data_slews, lane,
                     node.data_slews[analysisIndex(input.analysis_order[analysis])][transitionIndex(input.transition_order[transition])],
                     [](const double& value) { return value; });
    }
  }
  appendPaths(input, result, node_index, node.paths);
  return node;
}

NeutralArc encodeArc(const CsrReferenceInput& input, const CsrTimingResult& result, std::size_t arc_index)
{
  NeutralArc arc;
  arc.key = input.arcs[arc_index].key;
  arc.delay = requiredValue(result.arcs.delays, arc_index, "arc delay is absent/out of range");
  arc.min_delay = requiredValue(result.arcs.min_delays, arc_index, "arc minimum delay is absent/out of range");
  arc.max_delay = requiredValue(result.arcs.max_delays, arc_index, "arc maximum delay is absent/out of range");
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    const std::size_t neutral_analysis = analysisIndex(input.analysis_order[analysis]);
    for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
      const std::size_t neutral_input = transitionIndex(input.transition_order[input_transition]);
      const std::size_t trans_lane = arcTransitionLane(arc_index, analysis, input_transition);
      importOptional(result.arcs.transition_delays, trans_lane, arc.transition_delays[neutral_analysis][neutral_input],
                     [](const double& value) { return value; });
      for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
        const std::size_t neutral_output = transitionIndex(input.transition_order[output_transition]);
        const std::size_t lane = arcIoLane(arc_index, analysis, input_transition, output_transition);
        importOptional(result.arcs.input_output_delays, lane, arc.input_output_delays[neutral_analysis][neutral_input][neutral_output],
                       [](const double& value) { return value; });
        importOptional(result.arcs.graph_delays, lane, arc.graph_delays[neutral_analysis][neutral_input][neutral_output],
                       [](const double& value) { return value; });
      }
    }
  }
  for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
    const std::size_t lane = arc_index * kTransitionCount + input_transition;
    importOptional(result.arcs.transition_types, lane, arc.transition_types[transitionIndex(input.transition_order[input_transition])],
                   [](const Transition& value) { return value; });
  }

  require(arc_index + 1 < result.arcs.timing_arc_row_offsets.size(), "timing-arc row offset is out of range");
  const std::uint64_t begin = result.arcs.timing_arc_row_offsets[arc_index];
  const std::uint64_t end = result.arcs.timing_arc_row_offsets[arc_index + 1];
  require(begin <= end && end <= result.arcs.timing_arc_indices.size(), "timing-arc row extent is invalid");
  for (std::uint64_t child64 = begin; child64 < end; ++child64) {
    const std::size_t child = static_cast<std::size_t>(child64);
    NeutralArcEntry entry;
    entry.timing_arc_index = result.arcs.timing_arc_indices[child];
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
        for (std::size_t output_transition = 0; output_transition < kTransitionCount; ++output_transition) {
          const std::size_t lane = timingArcLane(child, analysis, input_transition, output_transition);
          require(lane < result.arcs.timing_arc_delays.presence.size() && lane < result.arcs.timing_arc_fallbacks.presence.size(),
                  "timing-arc lane is out of range");
          const bool has_delay = result.arcs.timing_arc_delays.presence[lane] != 0;
          require(has_delay == (result.arcs.timing_arc_fallbacks.presence[lane] != 0), "timing-arc delay/fallback presence mismatch");
          if (has_delay) {
            const std::size_t neutral_analysis = analysisIndex(input.analysis_order[analysis]);
            const std::size_t neutral_input = transitionIndex(input.transition_order[input_transition]);
            const std::size_t neutral_output = transitionIndex(input.transition_order[output_transition]);
            entry.lanes[neutral_analysis][neutral_input][neutral_output]
                = NeutralArcDelay{result.arcs.timing_arc_delays.values.at(lane), result.arcs.timing_arc_fallbacks.values.at(lane) != 0};
          }
        }
      }
    }
    arc.timing_arcs.push_back(std::move(entry));
  }
  return arc;
}

Json extentToJson(const CsrBufferExtent& extent)
{
  return {{"logical_size", extent.logical_size}, {"capacity", extent.capacity}};
}

Json optionalString(const std::optional<std::string>& value)
{
  return value ? Json(*value) : Json(nullptr);
}

Json fingerprintsToJson(const CsrCurrentFingerprints& fingerprints)
{
  return {{"topology_fingerprint", optionalString(fingerprints.topology_fingerprint)},
          {"clock_constraint_fingerprint", optionalString(fingerprints.clock_constraint_fingerprint)},
          {"library_fingerprint", optionalString(fingerprints.library_fingerprint)},
          {"corner_fingerprint", optionalString(fingerprints.corner_fingerprint)},
          {"parasitic_fingerprint", optionalString(fingerprints.parasitic_fingerprint)},
          {"timing_input_fingerprint", optionalString(fingerprints.timing_input_fingerprint)}};
}

}  // namespace

std::string encodeSnapshotCandidate(const CsrReferenceInput& input)
{
  const std::vector<std::uint8_t> bytes = test::canonicalIndependentReferenceInputBytes(input);
  return bytesToHex(bytes);
}

NeutralWire encodeReferenceTiming(const CsrReferenceInput& input, const CsrTimingResult& result)
{
  NeutralWire wire;
  wire.timing_order.reserve(result.level_nodes.size());
  for (NodeId id : result.level_nodes) {
    wire.timing_order.push_back(nodeKey(input, id));
  }
  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    NeutralAdjacencyRow out{input.nodes[node].key, {}};
    NeutralAdjacencyRow in{input.nodes[node].key, {}};
    require(node + 1 < input.out_row_offsets.size() && node + 1 < input.in_row_offsets.size(), "adjacency row offset is out of range");
    for (std::uint64_t offset = input.out_row_offsets[node]; offset < input.out_row_offsets[node + 1]; ++offset) {
      out.arcs.push_back(arcKey(input, input.out_arc_ids.at(static_cast<std::size_t>(offset))));
    }
    for (std::uint64_t offset = input.in_row_offsets[node]; offset < input.in_row_offsets[node + 1]; ++offset) {
      in.arcs.push_back(arcKey(input, input.in_arc_ids.at(static_cast<std::size_t>(offset))));
    }
    wire.out_rows.push_back(std::move(out));
    wire.in_rows.push_back(std::move(in));
    wire.nodes.push_back(encodeNode(input, result, node));
  }
  for (std::size_t arc = 0; arc < input.arcs.size(); ++arc) {
    wire.arcs.push_back(encodeArc(input, result, arc));
  }
  std::sort(wire.nodes.begin(), wire.nodes.end(), [](const NeutralNode& lhs, const NeutralNode& rhs) { return nodeKeyLess(lhs.key, rhs.key); });
  std::sort(wire.arcs.begin(), wire.arcs.end(), [](const NeutralArc& lhs, const NeutralArc& rhs) { return arcKeyLess(lhs.key, rhs.key); });
  return wire;
}

Json identityToJson(const CsrSnapshotIdentity& identity)
{
  return {{"fingerprint_schema_version", identity.fingerprint_schema_version},
          {"session_token", identity.session_token},
          {"graph_generation", identity.graph_generation},
          {"topology_fingerprint", identity.topology_fingerprint},
          {"clock_constraint_fingerprint", identity.clock_constraint_fingerprint},
          {"library_fingerprint", identity.library_fingerprint},
          {"corner_fingerprint", identity.corner_fingerprint},
          {"parasitic_fingerprint", identity.parasitic_fingerprint},
          {"timing_input_fingerprint", identity.timing_input_fingerprint}};
}

Json observationToJson(const CsrSnapshotObservation& observation)
{
  Json reasons = Json::array();
  for (CsrReferenceUnsupportedReason reason : observation.support.unsupported_reasons) {
    reasons.push_back(std::string(token(reason)));
  }
  return {{"schema_version", observation.schema_version},
          {"state", std::string(token(observation.state))},
          {"identity", identityToJson(observation.identity)},
          {"counts", {{"nodes", observation.counts.nodes}, {"arcs", observation.counts.arcs}, {"levels", observation.counts.levels}}},
          {"buffers",
           {{"node_keys", extentToJson(observation.buffers.node_keys)},
            {"arc_keys", extentToJson(observation.buffers.arc_keys)},
            {"out_row_offsets", extentToJson(observation.buffers.out_row_offsets)},
            {"out_arc_ids", extentToJson(observation.buffers.out_arc_ids)},
            {"in_row_offsets", extentToJson(observation.buffers.in_row_offsets)},
            {"in_arc_ids", extentToJson(observation.buffers.in_arc_ids)},
            {"level_offsets", extentToJson(observation.buffers.level_offsets)},
            {"level_nodes", extentToJson(observation.buffers.level_nodes)},
            {"node_to_level", extentToJson(observation.buffers.node_to_level)}}},
          {"capacity", {{"last_action", std::string(token(observation.capacity.last_action))}, {"event_count", observation.capacity.event_count}}},
          {"support", {{"reference_supported", observation.support.reference_supported}, {"unsupported_reasons", std::move(reasons)}}},
          {"timing_ms",
           {{"export", observation.timing_ms.export_ms},
            {"reference", observation.timing_ms.reference_ms ? Json(*observation.timing_ms.reference_ms) : Json(nullptr)},
            {"scatter", observation.timing_ms.scatter_ms ? Json(*observation.timing_ms.scatter_ms) : Json(nullptr)}}},
          {"host_bytes", {{"logical", observation.host_bytes.logical}, {"capacity", observation.host_bytes.capacity}}},
          {"last_rejection", nullptr},
          {"last_invalidation", nullptr}};
}

Json scatterToJson(const CsrTimingResultScatterAttempt& attempt)
{
  require(!attempt.current_capture_error, "successful scatter retained current-capture error");
  return {{"code", std::string(token(attempt.scatter_result.code))},
          {"writes_applied", attempt.scatter_result.writes_applied},
          {"invalidation_reason",
           attempt.scatter_result.invalidation_reason ? Json(std::string(token(*attempt.scatter_result.invalidation_reason))) : Json(nullptr)},
          {"current_fingerprints", fingerprintsToJson(attempt.current_fingerprints)},
          {"current_capture_error", nullptr}};
}

}  // namespace ista::csr::real_diff
