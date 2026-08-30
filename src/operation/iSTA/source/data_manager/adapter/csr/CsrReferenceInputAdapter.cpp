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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "CsrReferenceNativeModelValidatorInternal.hpp"
#include "CsrReferenceValidator.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::csr::detail {

CsrReferenceInputExportFailure ReferenceExportProgress::failure(CsrCaptureCode code) const
{
  return CsrReferenceInputExportFailure{code, failed_phase, current_fingerprints};
}

[[noreturn]] void abortReferenceExport(CsrCaptureCode code)
{
  throw ReferenceExportAbort(code);
}

[[noreturn]] void malformedReferenceInput()
{
  abortReferenceExport(CsrCaptureCode::kMalformedTopology);
}

[[noreturn]] void illegalReferenceNumeric()
{
  abortReferenceExport(CsrCaptureCode::kIllegalNumeric);
}

[[noreturn]] void invalidReferenceTable()
{
  abortReferenceExport(CsrCaptureCode::kInvalidTableShape);
}

void requireFiniteReference(double value)
{
  if (!std::isfinite(value))
    illegalReferenceNumeric();
}

void requireNonnegativeReference(double value)
{
  requireFiniteReference(value);
  if (value < 0.0)
    illegalReferenceNumeric();
}

bool referenceByteStringLess(const std::string& lhs, const std::string& rhs)
{
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                      [](char left, char right) { return static_cast<unsigned char>(left) < static_cast<unsigned char>(right); });
}

ReferenceBuildState::ReferenceBuildState(const ista::Database& native_database, CsrGraphSnapshot graph, StaAnalysisContext context) : database(native_database)
{
  input.analysis_context = std::move(context);
  input.nodes = std::move(graph.nodes);
  input.node_ids_by_key = std::move(graph.node_ids_by_key);
  input.instances = std::move(graph.instances);
  input.arc_ids_by_key = std::move(graph.arc_ids_by_key);
  input.out_row_offsets = std::move(graph.out_row_offsets);
  input.out_arc_ids = std::move(graph.out_arc_ids);
  input.in_row_offsets = std::move(graph.in_row_offsets);
  input.in_arc_ids = std::move(graph.in_arc_ids);
  input.ordered_root_ids = std::move(graph.ordered_root_ids);
  input.ordered_start_ids = std::move(graph.ordered_start_ids);
  input.ordered_end_ids = std::move(graph.ordered_end_ids);

  input.arcs.reserve(graph.arcs.size());
  for (auto& graph_arc : graph.arcs) {
    ArcRecord arc;
    arc.id = graph_arc.id;
    arc.key = std::move(graph_arc.key);
    arc.source = graph_arc.source;
    arc.sink = graph_arc.sink;
    arc.kind = graph_arc.kind;
    arc.native_traversal_rank = graph_arc.native_traversal_rank;
    arc.is_clock_arc = graph_arc.is_clock_arc;
    arc.is_disabled = graph_arc.is_disabled;
    arc.is_loop_disabled = graph_arc.is_loop_disabled;
    input.arcs.push_back(std::move(arc));
  }

  for (std::size_t index = 0; index < input.nodes.size(); ++index) {
    const NodeRecord& node = input.nodes[index];
    if (node.id.value() != index || !node_ids_by_full_name.emplace(node.key.full_name, node.id).second)
      malformedReferenceInput();
  }
  for (std::size_t index = 0; index < input.instances.size(); ++index) {
    const InstanceRecord& instance = input.instances[index];
    if (!instance_indices_by_name.emplace(instance.instance_name, index).second)
      malformedReferenceInput();
  }
  if (database.get_arc_list().size() != input.arcs.size() || database.get_instance_map().size() != input.instances.size()
      || database.get_pin_map().size() != input.nodes.size())
    malformedReferenceInput();

  port_constraint_indices_by_node.resize(input.nodes.size());
  capacitance_indices_by_node.resize(input.nodes.size());
  clock_owner_by_node.resize(input.nodes.size());
  clock_source_nodes.assign(input.nodes.size(), 0);
  clock_reachable_nodes.assign(input.nodes.size(), 0);
  replayed_clock_arcs.assign(input.arcs.size(), 0);
  timing_check_data_nodes.assign(input.nodes.size(), 0);
}

std::optional<NodeId> resolveNode(const ReferenceBuildState& state, const std::string& full_name)
{
  const auto found = state.node_ids_by_full_name.find(full_name);
  return found == state.node_ids_by_full_name.end() ? std::nullopt : std::optional<NodeId>{found->second};
}

const NodeRecord& requireNode(const ReferenceBuildState& state, NodeId node)
{
  if (node.value() >= state.input.nodes.size())
    malformedReferenceInput();
  const NodeRecord& result = state.input.nodes[node.value()];
  if (result.id != node)
    malformedReferenceInput();
  return result;
}

const InstanceRecord* findInstanceRecord(const ReferenceBuildState& state, const std::string& instance_name)
{
  const auto found = state.instance_indices_by_name.find(instance_name);
  if (found == state.instance_indices_by_name.end())
    return nullptr;
  if (found->second >= state.input.instances.size())
    malformedReferenceInput();
  return &state.input.instances[found->second];
}

const ista::Instance& requireNativeInstance(const ReferenceBuildState& state, const std::string& instance_name)
{
  const auto found = state.database.get_instance_map().find(instance_name);
  if (found == state.database.get_instance_map().end() || found->second.get_instance_name() != instance_name)
    malformedReferenceInput();
  return found->second;
}

const ista::TimingCell* findNativeTimingCell(const ReferenceBuildState& state, const std::string& cell_name)
{
  const auto& cells = state.database.get_timing_library().get_cell_map();
  const auto found = cells.find(cell_name);
  if (found == cells.end())
    return nullptr;
  if (found->second.get_cell_name() != cell_name)
    malformedReferenceInput();
  return std::addressof(found->second);
}

bool nativeArcMatchesReferenceRecord(const ista::Arc& native_arc, const ArcRecord& arc, const NodeRecord& source, const NodeRecord& sink)
{
  const bool kind_matches = (arc.kind == ArcKind::kCell && native_arc.get_type() == ista::ArcType::kCell)
                            || (arc.kind == ArcKind::kNet && native_arc.get_type() == ista::ArcType::kNet);
  const bool unbound_cell = native_arc.get_type() == ista::ArcType::kCell && native_arc.get_timing_cell_arc() == nullptr;
  const bool library_ports_match
      = unbound_cell
            ? native_arc.get_library_source_port().empty() && native_arc.get_library_sink_port().empty()
                  && arc.key.base.library_source_port == source.key.pin_name && arc.key.base.library_sink_port == sink.key.pin_name
            : native_arc.get_library_source_port() == arc.key.base.library_source_port && native_arc.get_library_sink_port() == arc.key.base.library_sink_port;
  return arc.key.base.source == source.key && arc.key.base.sink == sink.key && arc.kind == arc.key.base.kind && kind_matches
         && native_arc.get_source_pin() == source.key.full_name && native_arc.get_sink_pin() == sink.key.full_name
         && native_arc.get_owner_name() == arc.key.base.owner_name && library_ports_match && native_arc.get_is_clock_arc() == arc.is_clock_arc
         && native_arc.get_is_disable_arc() == arc.is_disabled && native_arc.get_is_loop_disable() == arc.is_loop_disabled;
}

const ista::Arc& requireNativeArc(const ReferenceBuildState& state, const ArcRecord& arc)
{
  const auto& native_arcs = state.database.get_arc_list();
  if (arc.native_traversal_rank >= native_arcs.size())
    malformedReferenceInput();
  const ista::Arc& native_arc = native_arcs.at(static_cast<std::size_t>(arc.native_traversal_rank));
  const NodeRecord& source = requireNode(state, arc.source);
  const NodeRecord& sink = requireNode(state, arc.sink);
  if (!nativeArcMatchesReferenceRecord(native_arc, arc, source, sink))
    malformedReferenceInput();
  return native_arc;
}

}  // namespace ista::csr::detail

namespace ista::csr {

CsrReferenceInputExportResult exportCsrReferenceInputCandidate(const ista::Database& database, const StaAnalysisContext& canonical_context,
                                                               std::uint64_t caller_session_token)
{
  if (caller_session_token == 0)
    throw std::invalid_argument("CSR reference input export requires a nonzero session token");

  detail::ReferenceExportProgress progress;
  if (!validateAnalysisContextIntrinsic(canonical_context).empty())
    return progress.failure(CsrCaptureCode::kInvalidAnalysisContext);

  progress.failed_phase = CsrReferenceInputExportPhase::kTopology;
  try {
    CsrGraphExportResult graph_result = exportCsrGraphSnapshot(database);
    if (const auto* failure = std::get_if<CsrGraphExportFailure>(&graph_result))
      return progress.failure(failure->code);

    detail::ReferenceBuildState state(database, std::move(std::get<CsrGraphSnapshot>(graph_result)), canonical_context);
    progress.current_fingerprints.topology_fingerprint = makeReferenceTopologyFingerprint(state.input);

    progress.failed_phase = CsrReferenceInputExportPhase::kClockConstraint;
    detail::exportReferenceConstraints(state);
    progress.current_fingerprints.clock_constraint_fingerprint = makeReferenceClockConstraintFingerprint(state.input);

    progress.failed_phase = CsrReferenceInputExportPhase::kLibrary;
    detail::exportReferenceLiberty(state);
    progress.current_fingerprints.library_fingerprint = makeReferenceLibraryFingerprint(state.input);

    progress.failed_phase = CsrReferenceInputExportPhase::kCorner;
    progress.current_fingerprints.corner_fingerprint = makeReferenceCornerFingerprint(state.input);

    progress.failed_phase = CsrReferenceInputExportPhase::kParasitic;
    detail::exportReferenceParasitics(state);
    if (!detail::validateParasiticContextCrossField(state.input).empty())
      return progress.failure(CsrCaptureCode::kInvalidAnalysisContext);
    progress.current_fingerprints.parasitic_fingerprint = makeReferenceParasiticFingerprint(state.input);

    progress.failed_phase = CsrReferenceInputExportPhase::kTimingInput;
    detail::exportReferenceTimingChecks(state);
    detail::exportReferenceClockSeeds(state);
    detail::exportReferenceLoadsAndSupport(state);

    state.input.source_identity = makeReferenceInputIdentity(state.input, caller_session_token, 0);
    const auto violations = validateReferenceInputForSession(state.input, caller_session_token);
    if (!violations.empty())
      throw std::logic_error("native CSR reference projection violated its complete contract: " + violations.front().field + ": " + violations.front().message);
    return std::move(state.input);
  } catch (const detail::ReferenceExportAbort& error) {
    return progress.failure(error.code());
  }
}

}  // namespace ista::csr
