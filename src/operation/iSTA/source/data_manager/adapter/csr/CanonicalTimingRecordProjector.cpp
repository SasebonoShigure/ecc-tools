// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CanonicalTimingRecordProjectorInternal.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "TimingGraphCsrAdapterInternal.hpp"

namespace ista::csr::detail {
namespace {

const ista::Instance& requireInstance(const ista::Database& database, const ista::Pin& pin)
{
  const auto match = database.get_instance_map().find(pin.get_instance_name());
  if (match == database.get_instance_map().end())
    malformed();
  return match->second;
}
const ista::Pin& requirePin(const CanonicalArcKeyContext& context, const std::string& name)
{
  const auto pin = context.database.get_pin_map().find(name);
  if (pin == context.database.get_pin_map().end())
    malformed();
  return pin->second;
}
NodeId requireNodeId(const CanonicalArcKeyContext& context, const std::string& name)
{
  const auto node = context.node_ids_by_full_name.find(name);
  if (node == context.node_ids_by_full_name.end())
    malformed();
  return node->second;
}
const ista::Instance& requireArcInstance(const CanonicalArcKeyContext& context, const ista::Arc& arc, const ista::Pin& source, const ista::Pin& sink)
{
  if (source.get_is_port() || sink.get_is_port() || source.get_instance_name().empty() || source.get_instance_name() != sink.get_instance_name()
      || arc.get_owner_name() != source.get_instance_name())
    malformed();
  const auto instance = context.database.get_instance_map().find(source.get_instance_name());
  if (instance == context.database.get_instance_map().end() || instance->first != instance->second.get_instance_name())
    malformed();
  const auto source_owner = context.ordered_pin_owners.find(source.get_full_name());
  const auto sink_owner = context.ordered_pin_owners.find(sink.get_full_name());
  if (source_owner == context.ordered_pin_owners.end() || sink_owner == context.ordered_pin_owners.end() || source_owner->second != instance->first
      || sink_owner->second != instance->first)
    malformed();
  return instance->second;
}
const ista::TimingCell* findTimingCell(const CanonicalArcKeyContext& context, const ista::Instance& instance)
{
  const auto cell = context.database.get_timing_library().get_cell_map().find(instance.get_cell_name());
  return cell == context.database.get_timing_library().get_cell_map().end() ? nullptr : std::addressof(cell->second);
}
const ista::TimingCellArc& requireBoundCellArc(const ista::Arc& arc, const ista::TimingCell& cell)
{
  const ista::TimingCellArc* native_pointer = arc.get_timing_cell_arc();
  if (native_pointer == nullptr)
    malformed();
  for (const auto& candidate : cell.get_cell_arc_list())
    if (std::addressof(candidate) == native_pointer && candidate.get_is_timing_graph_arc())
      return candidate;
  malformed();
}
bool isInputLike(PinDirection direction)
{
  return direction == PinDirection::kInput || direction == PinDirection::kInout;
}
bool isOutputLike(PinDirection direction)
{
  return direction == PinDirection::kOutput || direction == PinDirection::kInout;
}
std::vector<LibraryArcSignature> projectLibrarySignatures(const ista::TimingCellArc& cell_arc)
{
  std::vector<LibraryArcSignature> result;
  result.reserve(cell_arc.get_timing_arc_list().size());
  for (const auto& timing_arc : cell_arc.get_timing_arc_list()) {
    if (!timing_arc.get_check_table_map().empty())
      malformed();
    try {
      result.push_back(exportReferenceLibraryArcSignature(timing_arc));
    } catch (const ReferenceExportAbort&) {
      malformed();
    }
  }
  return result;
}
ArcKeyBase projectCellArcKeyBase(const CanonicalArcKeyContext& context, const ista::Arc& arc, const ista::Pin& source, const ista::Pin& sink)
{
  const auto& instance = requireArcInstance(context, arc, source, sink);
  const auto* cell = findTimingCell(context, instance);
  const bool fallback_cell = cell == nullptr || cell->get_cell_arc_list().empty();
  const bool raw_source_empty = arc.get_library_source_port().empty();
  const bool raw_sink_empty = arc.get_library_sink_port().empty();
  ArcKeyBase result;
  result.source = context.nodes[requireNodeId(context, source.get_full_name()).value()].key;
  result.sink = context.nodes[requireNodeId(context, sink.get_full_name()).value()].key;
  result.kind = ArcKind::kCell;
  result.owner_name = instance.get_instance_name();
  if (arc.get_timing_cell_arc() == nullptr) {
    if (!fallback_cell || !raw_source_empty || !raw_sink_empty || source.get_full_name() == sink.get_full_name()
        || !isInputLike(convertPinDirection(source.get_direction())) || !isOutputLike(convertPinDirection(sink.get_direction())))
      malformed();
    result.library_source_port = source.get_pin_name();
    result.library_sink_port = sink.get_pin_name();
    return result;
  }
  if (fallback_cell || raw_source_empty || raw_sink_empty)
    malformed();
  const auto& cell_arc = requireBoundCellArc(arc, *cell);
  if (arc.get_library_source_port() != source.get_pin_name() || arc.get_library_sink_port() != sink.get_pin_name()
      || arc.get_library_source_port() != cell_arc.get_source_port() || arc.get_library_sink_port() != cell_arc.get_sink_port())
    malformed();
  result.library_source_port = arc.get_library_source_port();
  result.library_sink_port = arc.get_library_sink_port();
  result.library_arcs = projectLibrarySignatures(cell_arc);
  return result;
}
ArcKeyBase projectNetArcKeyBase(const CanonicalArcKeyContext& context, const ista::Arc& arc, const ista::Pin& source, const ista::Pin& sink)
{
  if (arc.get_timing_cell_arc() != nullptr || !arc.get_library_source_port().empty() || !arc.get_library_sink_port().empty())
    malformed();
  if (source.get_net_name().empty() || source.get_net_name() != sink.get_net_name() || arc.get_owner_name() != source.get_net_name())
    malformed();
  const auto net = context.database.get_net_map().find(arc.get_owner_name());
  if (net == context.database.get_net_map().end() || net->first != net->second.get_net_name())
    malformed();
  return {context.nodes[requireNodeId(context, source.get_full_name()).value()].key,
          context.nodes[requireNodeId(context, sink.get_full_name()).value()].key,
          ArcKind::kNet,
          arc.get_owner_name(),
          {},
          {},
          {}};
}
}  // namespace

NodeRecord projectCanonicalNodeRecord(const ista::Database& database, const std::string& map_name, const ista::Pin& pin,
                                      const std::set<std::string>& starts, const std::set<std::string>& ends,
                                      const std::unordered_map<std::string, std::string>& ordered_pin_owners)
{
  if (map_name.empty() || pin.get_full_name() != map_name)
    malformed();
  NodeRecord result;
  result.direction = convertPinDirection(pin.get_direction());
  result.net_name = pin.get_net_name();
  result.is_port = pin.get_is_port();
  result.is_startpoint = starts.contains(map_name);
  result.is_endpoint = ends.contains(map_name);
  if (!result.net_name.empty()) {
    const auto net = database.get_net_map().find(result.net_name);
    if (net == database.get_net_map().end() || net->first != net->second.get_net_name())
      malformed();
  }
  if (result.is_port) {
    if (pin.get_pin_name() != map_name || !pin.get_instance_name().empty())
      malformed();
    result.key = {NodeKind::kPort, map_name, {}, {}};
    return result;
  }
  if (pin.get_pin_name().empty() || pin.get_instance_name().empty() || map_name != pin.get_instance_name() + ":" + pin.get_pin_name())
    malformed();
  const auto& instance = requireInstance(database, pin);
  const auto owner = ordered_pin_owners.find(map_name);
  if (owner == ordered_pin_owners.end() || owner->second != instance.get_instance_name())
    malformed();
  result.key = {NodeKind::kInstancePin, map_name, pin.get_instance_name(), pin.get_pin_name()};
  result.instance_cell = instance.get_cell_name();
  result.is_sequential = instance.get_is_sequential();
  result.is_clock_pin = !instance.get_clock_pin_name().empty() && map_name == instance.get_clock_pin_name();
  result.is_data_pin = !instance.get_data_pin_name().empty() && map_name == instance.get_data_pin_name();
  result.is_output_pin = !instance.get_output_pin_name().empty() && map_name == instance.get_output_pin_name();
  return result;
}
InstanceRecord projectCanonicalInstanceRecord(const ista::Database& database, const std::string& map_name, const ista::Instance& instance,
                                              const std::map<std::string, NodeId>& node_ids_by_full_name)
{
  if (map_name != instance.get_instance_name())
    malformed();
  InstanceRecord result;
  result.instance_name = map_name;
  result.cell_name = instance.get_cell_name();
  result.is_sequential = instance.get_is_sequential();
  result.has_timing_checks = !instance.get_check_arc_list().empty();
  result.is_clock_gating = instance.get_is_clock_gating();
  const auto cell = database.get_timing_library().get_cell_map().find(result.cell_name);
  if (cell != database.get_timing_library().get_cell_map().end())
    result.is_macro = cell->second.get_is_macro();
  for (const auto& pin_name : instance.get_pin_name_list()) {
    const auto pin = database.get_pin_map().find(pin_name);
    const auto id = node_ids_by_full_name.find(pin_name);
    if (pin == database.get_pin_map().end() || id == node_ids_by_full_name.end() || pin->second.get_is_port()
        || pin->second.get_instance_name() != map_name)
      malformed();
    result.ordered_pins.push_back(id->second);
  }
  return result;
}
ArcKeyBase projectCanonicalArcKeyBase(const CanonicalArcKeyContext& context, const ista::Arc& arc)
{
  const auto& source = requirePin(context, arc.get_source_pin());
  const auto& sink = requirePin(context, arc.get_sink_pin());
  switch (arc.get_type()) {
    case ista::ArcType::kCell:
      return projectCellArcKeyBase(context, arc, source, sink);
    case ista::ArcType::kNet:
      return projectNetArcKeyBase(context, arc, source, sink);
    case ista::ArcType::kNone:
      malformed();
  }
  malformed();
}
ArcEligibility projectCanonicalArcEligibility(const ArcRecord& arc, const NodeRecord& sink, bool sink_clock_reachable, bool replayed_clock_arc)
{
  const bool active = !arc.is_disabled && !arc.is_loop_disabled;
  const bool sequential_clock_sink = arc.kind == ArcKind::kNet && sink.is_sequential && sink.is_clock_pin;
  return {active, active && !sequential_clock_sink, active && !(sequential_clock_sink && sink_clock_reachable), replayed_clock_arc};
}
std::optional<ArcOutputLoadMatrix> projectCanonicalNoParasiticOutputLoad(
    const std::string& sink_net_name, const std::map<std::string, ArcOutputLoadMatrix>& net_loads)
{
  const auto load = net_loads.find(sink_net_name);
  return load == net_loads.end() ? std::optional<ArcOutputLoadMatrix>{ArcOutputLoadMatrix{}} : std::optional<ArcOutputLoadMatrix>{load->second};
}
CellTransferModel projectCanonicalArcTransfer(const ArcRecord& arc, const ista::Arc& native_arc)
{
  if (arc.kind == ArcKind::kNet) {
    CellTransferModel transfer;
    transfer.delay = 0.0;
    transfer.delay_min = 0.0;
    transfer.delay_max = 0.0;
    return transfer;
  }
  const ista::TimingCellArc* native_cell_arc = native_arc.get_timing_cell_arc();
  if (native_cell_arc == nullptr) {
    CellTransferModel transfer;
    transfer.is_clock_arc = arc.is_clock_arc;
    transfer.is_disabled = arc.is_disabled;
    return transfer;
  }
  return exportReferenceCellTransfer(*native_cell_arc);
}
}  // namespace ista::csr::detail
