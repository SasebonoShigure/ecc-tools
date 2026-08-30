// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <stdexcept>
#include <utility>

#include "Arc.hpp"
#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputRawProjectionInternal.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "Net.hpp"
#include "Pin.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingLibrary.hpp"

namespace ista::csr::test {
namespace {

RawDirection directionToken(ista::PinDirection direction)
{
  switch (direction) {
    case ista::PinDirection::kNone:
      return RawDirection::kNone;
    case ista::PinDirection::kInput:
      return RawDirection::kInput;
    case ista::PinDirection::kOutput:
      return RawDirection::kOutput;
    case ista::PinDirection::kInout:
      return RawDirection::kInout;
  }
  throw std::runtime_error("raw projector: unknown pin direction");
}

RawArcKind arcKindToken(ista::ArcType kind)
{
  switch (kind) {
    case ista::ArcType::kCell:
      return RawArcKind::kCell;
    case ista::ArcType::kNet:
      return RawArcKind::kNet;
    case ista::ArcType::kNone:
      break;
  }
  throw std::runtime_error("raw projector: unknown arc kind");
}

RawTransition transitionToken(ista::TransType transition)
{
  switch (transition) {
    case ista::TransType::kRise:
      return RawTransition::kRise;
    case ista::TransType::kFall:
      return RawTransition::kFall;
    case ista::TransType::kNone:
      return RawTransition::kNone;
  }
  throw std::runtime_error("raw projector: unknown transition");
}

std::optional<RawSelectedClockArc> selectedClockArc(const ista::Instance& instance)
{
  const ista::TimingCellArc& native = instance.get_clock_to_q_arc();
  if (!native.get_is_clock_arc())
    return std::nullopt;
  RawSelectedClockArc selected;
  selected.source = native.get_source_port();
  selected.sink = native.get_sink_port();
  for (const auto& child : native.get_timing_arc_list())
    selected.ordered_child_triggers.push_back(transitionToken(child.get_trigger_trans_type()));
  return selected;
}

RawInstance projectInstance(const ista::Database& database, const std::string& map_key, const ista::Instance& instance)
{
  RawInstance raw;
  raw.map_key = map_key;
  raw.cell_name = instance.get_cell_name();
  raw.sequential = instance.get_is_sequential();
  raw.gating = instance.get_is_clock_gating();
  const auto cell = database.get_timing_library().get_cell_map().find(raw.cell_name);
  raw.macro = cell != database.get_timing_library().get_cell_map().end() && cell->second.get_is_macro();
  raw.clock_pin_name = instance.get_clock_pin_name();
  raw.data_pin_name = instance.get_data_pin_name();
  raw.output_pin_name = instance.get_output_pin_name();
  raw.ordered_pin_names = instance.get_pin_name_list();
  raw.selected_clock_arc = selectedClockArc(instance);
  return raw;
}

RawNet projectNet(const std::string& map_key, const ista::Net& net)
{
  RawNet raw;
  raw.map_key = map_key;
  raw.embedded_name = net.get_net_name();
  raw.ordered_drivers = net.get_driver_pin_list();
  if (raw.ordered_drivers.empty() && !net.get_driver_pin().empty())
    raw.ordered_drivers.push_back(net.get_driver_pin());
  raw.ordered_loads = net.get_load_pin_list();
  return raw;
}

}  // namespace

CsrReferenceInputRawProjection projectCsrReferenceInputRaw(const ista::Database& database)
{
  CsrReferenceInputRawProjection projection;
  projection.nodes.reserve(database.get_pin_map().size());
  for (const auto& [map_key, pin] : database.get_pin_map())
    projection.nodes.push_back({map_key, pin.get_full_name(), pin.get_is_port(), directionToken(pin.get_direction()), pin.get_instance_name()});

  projection.instances.reserve(database.get_instance_map().size());
  for (const auto& [map_key, instance] : database.get_instance_map())
    projection.instances.push_back(projectInstance(database, map_key, instance));

  projection.arcs.reserve(database.get_arc_list().size());
  for (std::size_t rank = 0; rank < database.get_arc_list().size(); ++rank) {
    const auto& arc = database.get_arc_list().at(rank);
    projection.arcs.push_back({static_cast<std::uint64_t>(rank), arc.get_source_pin(), arc.get_sink_pin(), arcKindToken(arc.get_type()), arc.get_owner_name(),
                               arc.get_library_source_port(), arc.get_library_sink_port(), arc.get_is_clock_arc(), arc.get_is_disable_arc(),
                               arc.get_is_loop_disable()});
  }

  projection.nets.reserve(database.get_net_map().size());
  for (const auto& [map_key, net] : database.get_net_map())
    projection.nets.push_back(projectNet(map_key, net));

  projectCsrReferenceInputRawConstraints(database, projection);
  projectCsrReferenceInputRawLibrary(database, projection);
  projectCsrReferenceInputRawParasitics(database, projection);
  return projection;
}

namespace raw_wire {

void appendNode(Writer& writer, const RawNode& node)
{
  writer.string(node.map_key);
  writer.string(node.full_name);
  writer.boolean(node.is_port);
  writer.enumeration(node.direction);
  writer.string(node.instance_name);
}

void appendInstance(Writer& writer, const RawInstance& instance)
{
  writer.string(instance.map_key);
  writer.string(instance.cell_name);
  writer.boolean(instance.sequential);
  writer.boolean(instance.gating);
  writer.boolean(instance.macro);
  writer.string(instance.clock_pin_name);
  writer.string(instance.data_pin_name);
  writer.string(instance.output_pin_name);
  writer.vector(instance.ordered_pin_names, [](Writer& local, const std::string& value) { local.string(value); });
  writer.optional(instance.selected_clock_arc, [](Writer& local, const RawSelectedClockArc& selected) {
    local.string(selected.source);
    local.string(selected.sink);
    local.vector(selected.ordered_child_triggers, [](Writer& nested, RawTransition trigger) { nested.enumeration(trigger); });
  });
}

void appendArc(Writer& writer, const RawArc& arc)
{
  writer.u64(arc.native_rank);
  writer.string(arc.source);
  writer.string(arc.sink);
  writer.enumeration(arc.kind);
  writer.string(arc.owner);
  writer.string(arc.library_source_port);
  writer.string(arc.library_sink_port);
  writer.boolean(arc.is_clock_arc);
  writer.boolean(arc.disabled);
  writer.boolean(arc.loop_disabled);
}

void appendNet(Writer& writer, const RawNet& net)
{
  writer.string(net.map_key);
  writer.string(net.embedded_name);
  writer.vector(net.ordered_drivers, [](Writer& local, const std::string& value) { local.string(value); });
  writer.vector(net.ordered_loads, [](Writer& local, const std::string& value) { local.string(value); });
}

}  // namespace raw_wire

std::vector<std::uint8_t> canonicalIndependentReferenceInputRawBytes(const CsrReferenceInputRawProjection& projection)
{
  raw_wire::Writer writer;
  writer.string("ista-csr-reference-input-raw/1.0.0");
  writer.vector(projection.nodes, raw_wire::appendNode);
  writer.vector(projection.instances, raw_wire::appendInstance);
  writer.vector(projection.arcs, raw_wire::appendArc);
  writer.vector(projection.nets, raw_wire::appendNet);
  writer.vector(projection.clock_constraints, raw_wire::appendClock);
  writer.vector(projection.port_constraints, raw_wire::appendPort);
  writer.vector(projection.case_constraints, raw_wire::appendCase);
  writer.vector(projection.timing_checks, raw_wire::appendCheck);
  writer.vector(projection.referenced_library_cells, raw_wire::appendLibraryCell);
  writer.optional(projection.parasitics, raw_wire::appendParasitics);
  return writer.bytes();
}

}  // namespace ista::csr::test
