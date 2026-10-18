// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingResultValueDecode.hpp"
#include "Database.hpp"
#include "DualCsrState.hpp"
#include "TimingEditResolver.hpp"
#include <algorithm>
#include <stdexcept>

namespace ista::csr::detail {
namespace {
AnalysisType analysis(AnalysisMode value) { return value == AnalysisMode::kMax ? AnalysisType::kMax : AnalysisType::kMin; }
PathSourceType source(PathSource value) { return value == PathSource::kInput ? PathSourceType::kInput : PathSourceType::kRegister; }
TransType transition(Transition value)
{
  switch (value) {
    case Transition::kRise: return TransType::kRise;
    case Transition::kFall: return TransType::kFall;
    case Transition::kNone: return TransType::kNone;
  }
  throw std::logic_error("invalid committed report transition");
}
}  // namespace

CsrReportNodeReader::CsrReportNodeReader(Database& database, const timing_dual_csr::DualCsrState& graph,
    const timing_edit::TimingEditResolverState& resolver, const CsrReportValues& values,
    std::span<const std::string> clock_names, PrepareCsrReport prepare)
    : _database(database), _graph(graph), _resolver(resolver), _values(values),
      _clock_names(clock_names), _prepare(std::move(prepare)) {}

std::optional<NodeId> CsrReportNodeReader::findNode(const std::string& name) const
{
  const auto pin = _database.get_pin_map().find(name);
  if (pin == _database.get_pin_map().end()) return std::nullopt;
  const auto& value = pin->second;
  const NodeKey key = value.get_is_port() ? NodeKey{NodeKind::kPort, name, {}, {}}
      : NodeKey{NodeKind::kInstancePin, name, value.get_instance_name(), value.get_pin_name()};
  const auto* entry = timing_edit::TimingEditResolverView(_resolver).findNodeEntryForKey(key);
  return entry ? std::optional{NodeId{entry->second.slot.value()}} : std::nullopt;
}
bool CsrReportNodeReader::contains(const std::string& name) const { return findNode(name).has_value(); }
NodeId CsrReportNodeReader::nodeId(const std::string& name) const { return findNode(name).value(); }
const std::string& CsrReportNodeReader::nodeName(std::uint64_t id) const { return _graph.nodes.at(id).key.full_name; }
ClockTagId CsrReportNodeReader::clockTag(std::string_view name) const
{
  const auto& tags = _graph.reference_payload.clock_tags;
  const auto found = std::ranges::find(tags, name);
  return ClockTagId{found == tags.end() ? value_detail::absent : static_cast<std::uint64_t>(found - tags.begin())};
}

void CsrReportNodeReader::prepare(const CsrReportRequest& request)
{
  // Analysis may insert missing-tag defaults into a decoded point.
  _points.clear();
  if (_values.complete) return;
  const auto paths = _prepare(request);
  for (const auto& path : paths) _path_slots[path.row].insert_or_assign(path.tag, path.slot);
}
void CsrReportNodeReader::preparePathTrace()
{
  _path_trace = true;
  _points.clear();
}

std::size_t CsrReportNodeReader::arcRank(std::uint64_t id)
{
  if (!_path_trace || id == value_detail::absent) return std::numeric_limits<std::size_t>::max();
  if (const auto found = _arc_ranks.find(id); found != _arc_ranks.end()) return found->second;
  const auto& payload = _graph.arc_slots.at(id).payload;
  const auto& key = payload.key.base;
  const TimingCellArc* child = nullptr;
  if (payload.native_cell_arc_ordinal) {
    const auto& instance = _database.get_instance_map().at(key.owner_name);
    const auto& cell = _database.get_timing_library().get_cell_map().at(instance.get_cell_name());
    child = &cell.get_cell_arc_list().at(*payload.native_cell_arc_ordinal);
  }
  std::vector<std::size_t> ranks;
  for (const auto rank : _database.get_outgoing_arc_list_map().at(key.source.full_name)) {
    const auto& arc = _database.get_arc_list().at(rank);
    if (arc.get_sink_pin() == key.sink.full_name && arc.get_owner_name() == key.owner_name
        && arc.get_type() == (payload.kind == ArcKind::kCell ? ArcType::kCell : ArcType::kNet)
        && arc.get_timing_cell_arc() == child) ranks.push_back(rank);
  }
  std::vector<std::uint64_t> peers;
  const auto collect = [&](ArcId candidate) {
    const auto& slot = _graph.arc_slots[candidate.value()];
    if (slot.active && slot.payload.key.base == key
        && slot.payload.native_cell_arc_ordinal == payload.native_cell_arc_ordinal)
      peers.push_back(candidate.value());
  };
  const auto node = nodeId(key.source.full_name).value();
  for (auto slot = _graph.base_out.row_offsets[node]; slot != _graph.base_out.row_offsets[node + 1]; ++slot)
    if (_graph.base_out.physical_slots[slot]) collect(*_graph.base_out.physical_slots[slot]);
  const auto aux = _graph.aux_out.source_to_aux_row[node];
  if (aux >= 0) for (const auto candidate : _graph.aux_out.row_arcs[aux]) collect(candidate);
  // Canonical ordinals span all children; match rank within this native child only.
  std::ranges::sort(peers, [&](auto left, auto right) {
    return _graph.arc_slots[left].payload.key.parallel_ordinal < _graph.arc_slots[right].payload.key.parallel_ordinal;
  });
  std::ranges::sort(ranks);
  for (std::size_t index = 0; index != peers.size(); ++index) _arc_ranks.emplace(peers[index], ranks.at(index));
  return _arc_ranks.at(id);
}

void CsrReportNodeReader::appendPath(PreparedTimingPointState& prepared, std::uint64_t row,
                                    std::uint64_t tag, const value_detail::PathState& value)
{
  if (!value.present) return;
  const auto& layout = _graph.reference_payload;
  const auto trans = transition(layout.transition_order[row % 2]);
  const auto& tag_name = layout.clock_tags.at(tag);
  auto state = preparePathBase(value, trans, tag_name, nodeName(value.start));
  if (value.predecessor_node != value_detail::absent)
    attachPathPredecessor(state, nodeName(value.predecessor_node), arcRank(value.predecessor_arc),
                          value.predecessor_delay, transition(static_cast<Transition>(value.predecessor_transition)));
  if (value.crpr_node != value_detail::absent) {
    state.set_crpr_clock_pin(nodeName(value.crpr_node));
    state.set_crpr_clock_trans_type(transition(static_cast<Transition>(value.crpr_transition)));
  }
  prepared.path_states[analysis(layout.analysis_order[(row / 4) % 2])]
      [source(layout.path_source_order[(row / 2) % 2])][trans].emplace(tag_name, std::move(state));
}

void CsrReportNodeReader::decodeSparse(std::size_t id, PreparedTimingPointState& prepared)
{
  const auto& record = _values.nodes.at(id);
  const auto& node = record.timing;
  prepared.arrival = node.arrival; prepared.required = node.required;
  prepared.slack = node.slack; prepared.launch = node.launch;
  prepared.level = static_cast<std::int32_t>(record.level);
  prepared.is_clock_point = node.clock_point;
  prepared.predecessor_arc_index = arcRank(node.predecessor_arc);
  if (node.predecessor_node != value_detail::absent) prepared.predecessor = nodeName(node.predecessor_node);
  if (node.clock_name != value_detail::absent) prepared.clock_name = _clock_names[node.clock_name];
  const auto& layout = _graph.reference_payload;
  for (std::size_t lane = 0; lane != 4; ++lane) {
    const auto mode = analysis(layout.analysis_order[lane / 2]);
    const auto trans = transition(layout.transition_order[lane % 2]);
    const auto& physical = node.physical_clock[lane];
    const auto* predecessor = physical.predecessor == value_detail::absent ? nullptr : &nodeName(physical.predecessor);
    if (!appendClockLane(prepared.physical_clock, mode, trans, physical, predecessor))
      throw std::logic_error("invalid committed physical clock");
    auto effective = physical;
    if (!node.propagated_clock) {
      effective.arrival = {0.0, node.clock_point};
      effective.slew = {0.0, node.clock_point};
      predecessor = nullptr;
    }
    if (!appendClockLane(prepared.effective_clock, mode, trans, effective, predecessor))
      throw std::logic_error("invalid committed effective clock");
    if (node.slew[lane].present) prepared.data_slews[mode][trans] = node.slew[lane].value;
  }
  for (auto row = _path_slots.lower_bound(id * 8); row != _path_slots.end() && row->first < (id + 1) * 8; ++row)
    for (const auto& [tag, slot] : row->second) appendPath(prepared, row->first, tag, _values.paths.at(slot));
}

void CsrReportNodeReader::decodeComplete(std::size_t id, PreparedTimingPointState& prepared)
{
  const auto& result = *_values.complete;
  const auto& nodes = result.nodes;
  prepared.arrival = nodes.arrivals.values.at(id); prepared.required = nodes.requireds.values.at(id);
  prepared.slack = nodes.slacks.values.at(id); prepared.launch = nodes.launches.values.at(id);
  prepared.level = static_cast<std::int32_t>(result.node_to_level.at(id));
  prepared.is_clock_point = nodes.is_clock_point.values.at(id) != 0;
  prepared.predecessor_arc_index = std::numeric_limits<std::size_t>::max();
  if (nodes.clock_names.presence.at(id)) prepared.clock_name = nodes.clock_names.values.at(id);
  if (nodes.predecessor_nodes.presence.at(id)) {
    prepared.predecessor = nodeName(nodes.predecessor_nodes.values.at(id).value());
    prepared.predecessor_arc_index = arcRank(nodes.predecessor_arcs.values.at(id).value());
  }
  const auto& layout = _graph.reference_payload;
  for (std::size_t lane = 0; lane != 4; ++lane) {
    const auto position = id * 4 + lane;
    const auto mode = analysis(layout.analysis_order[lane / 2]);
    const auto trans = transition(layout.transition_order[lane % 2]);
    const auto clock = [&](const ClockResultSoa& values, PreparedClockState& target) {
      const auto* predecessor = values.predecessor_nodes.presence.at(position)
          ? &nodeName(values.predecessor_nodes.values.at(position).value()) : nullptr;
      const value_detail::ClockLane value{
          {values.arrivals.values.at(position), values.arrivals.presence.at(position) != 0},
          {values.slews.values.at(position), values.slews.presence.at(position) != 0},
          value_detail::absent, values.predecessor_arc_delays.values.at(position),
          static_cast<std::uint8_t>(values.predecessor_transitions.values.at(position))};
      if (!appendClockLane(target, mode, trans, value, predecessor)) throw std::logic_error("invalid committed clock");
    };
    clock(result.effective_clock, prepared.effective_clock);
    clock(result.physical_clock, prepared.physical_clock);
    if (result.data_slews.presence.at(position)) prepared.data_slews[mode][trans] = result.data_slews.values.at(position);
  }
  const auto& paths = result.paths;
  for (std::uint64_t row = id * 8; row != (id + 1) * 8; ++row) {
    const auto begin = paths.row_offsets.at(row), end = paths.row_offsets.at(row + 1);
    for (auto slot = begin; slot != end; ++slot) {
      value_detail::PathState value;
      value.present = true;
      value.arrival = paths.arrivals.values.at(slot); value.slew = paths.slews.values.at(slot);
      value.launch = paths.launches.values.at(slot); value.start = paths.start_nodes.values.at(slot).value();
      if (paths.predecessor_nodes.presence.at(slot)) {
        value.predecessor_node = paths.predecessor_nodes.values.at(slot).value();
        value.predecessor_arc = paths.predecessor_arcs.values.at(slot).value();
        value.predecessor_delay = paths.predecessor_arc_delays.values.at(slot);
        value.predecessor_transition = static_cast<std::uint8_t>(paths.predecessor_transitions.values.at(slot));
      }
      if (paths.crpr_nodes.presence.at(slot)) {
        value.crpr_node = paths.crpr_nodes.values.at(slot).value();
        value.crpr_transition = static_cast<std::uint8_t>(paths.crpr_transitions.values.at(slot));
      }
      appendPath(prepared, row, paths.clock_tags.values.at(slot).value(), value);
    }
  }
}

TimingPoint& CsrReportNodeReader::point(const std::string& name)
{
  const auto id = nodeId(name).value();
  if (const auto found = _points.find(id); found != _points.end()) return found->second;
  PreparedTimingPointState prepared;
  if (_values.complete) decodeComplete(id, prepared);
  else decodeSparse(id, prepared);
  auto& result = _points.try_emplace(id).first->second;
  prepared.target = &result;
  publishPreparedTimingPoint(prepared);
  return result;
}
}  // namespace ista::csr::detail
