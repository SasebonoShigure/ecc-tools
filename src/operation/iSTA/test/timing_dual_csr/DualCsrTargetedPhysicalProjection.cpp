// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTargetedPhysicalProjection.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#include "DataManager.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#include "idm.h"
#pragma GCC diagnostic pop

namespace ista::timing_dual_csr::test {
namespace {

using namespace buffer_trace;

std::string endpointName(idb::IdbPin* pin)
{
  if (pin == nullptr)
    return "<null>";
  return pin->get_instance() == nullptr
             ? "PIN/" + pin->get_pin_name()
             : pin->get_instance()->get_name() + "/"
                   + pin->get_pin_name();
}

SourcePinFunctionalRecord capturePin(idb::IdbPin* pin, bool io)
{
  if (pin == nullptr)
    return {.name = "<null>", .instance = {}, .net = {},
            .direction = -1, .io = io};
  const auto* term = pin->get_term();
  return {.name = pin->get_pin_name(),
          .instance = pin->get_instance() == nullptr
                          ? "" : pin->get_instance()->get_name(),
          .net = pin->get_net_name(),
          .direction = term == nullptr
                           ? -1
                           : static_cast<std::int32_t>(
                                 term->get_direction()),
          .io = io};
}

void captureInstances(idb::IdbDesign& design,
                      SourceIdbFunctionalProjection& result)
{
  auto* list = design.get_instance_list();
  if (list == nullptr)
    return;
  for (auto* instance : list->get_instance_list()) {
    if (instance == nullptr) {
      result.instances.push_back(
          {.name = "<null>", .master = {}, .type = 0, .status = 0,
           .orient = 0, .x = 0, .y = 0, .pins = {}});
      continue;
    }
    auto* coordinate = instance->get_coordinate();
    SourceInstanceFunctionalRecord record{
        .name = instance->get_name(),
        .master = instance->get_cell_master() == nullptr
                      ? "" : instance->get_cell_master()->get_name(),
        .type = static_cast<std::int32_t>(instance->get_type()),
        .status = static_cast<std::int32_t>(instance->get_status()),
        .orient = static_cast<std::int32_t>(instance->get_orient()),
        .x = coordinate == nullptr ? 0 : coordinate->get_x(),
        .y = coordinate == nullptr ? 0 : coordinate->get_y(),
        .pins = {}};
    if (auto* pins = instance->get_pin_list(); pins != nullptr) {
      for (auto* pin : pins->get_pin_list()) {
        record.pins.push_back(capturePin(pin, false));
        result.connected_pin_count
            += pin != nullptr && pin->get_net() != nullptr;
      }
    }
    std::ranges::sort(record.pins, {},
                      &SourcePinFunctionalRecord::name);
    result.placed_instance_count += instance->is_placed();
    result.fixed_instance_count += instance->is_fixed();
    result.unplaced_instance_count += instance->is_unplaced();
    result.instances.push_back(std::move(record));
  }
  std::ranges::sort(result.instances, {},
                    &SourceInstanceFunctionalRecord::name);
}

void captureIoPins(idb::IdbDesign& design,
                   SourceIdbFunctionalProjection& result)
{
  auto* pins = design.get_io_pin_list();
  if (pins == nullptr)
    return;
  for (auto* pin : pins->get_pin_list()) {
    result.io_pins.push_back(capturePin(pin, true));
    result.connected_pin_count
        += pin != nullptr && pin->get_net() != nullptr;
  }
  std::ranges::sort(result.io_pins, {},
                    &SourcePinFunctionalRecord::name);
}

SourceWireSegmentFunctionalRecord captureSegment(
    idb::IdbRegularWireSegment* segment,
    SourceIdbFunctionalProjection& result)
{
  SourceWireSegmentFunctionalRecord record;
  if (segment == nullptr)
    return record;
  record = {.layer = segment->get_layer_name(),
            .new_layer = segment->is_new_layer(),
            .wire = segment->is_wire(),
            .via = segment->is_via(),
            .rect = segment->is_rect(),
            .points = {},
            .vias = {},
            .delta_rect = std::nullopt};
  for (auto* point : segment->get_point_list())
    if (point != nullptr)
      record.points.push_back(
          {point->get_x(), point->get_y(),
           segment->is_virtual(point) ? 1 : 0});
  for (auto* via : segment->get_via_list())
    record.vias.push_back(via == nullptr ? "<null>" : via->get_name());
  if (auto* rect = segment->get_delta_rect(); rect != nullptr)
    record.delta_rect = {{rect->get_low_x(), rect->get_low_y(),
                          rect->get_high_x(), rect->get_high_y()}};
  result.wire_point_count += record.points.size();
  result.via_count += record.vias.size();
  return record;
}

SourceWireFunctionalRecord captureWire(
    idb::IdbRegularWire* wire,
    SourceIdbFunctionalProjection& result)
{
  SourceWireFunctionalRecord record;
  if (wire == nullptr)
    return record;
  record.statement
      = static_cast<std::int32_t>(wire->get_wire_statement());
  record.shield = wire->get_shiled_name();
  result.wire_length += wire->wireLength();
  result.segment_count += wire->get_segment_list().size();
  for (auto* segment : wire->get_segment_list())
    record.segments.push_back(captureSegment(segment, result));
  return record;
}

void captureNets(idb::IdbDesign& design,
                 SourceIdbFunctionalProjection& result)
{
  auto* list = design.get_net_list();
  if (list == nullptr)
    return;
  for (auto* net : list->get_net_list()) {
    if (net == nullptr) {
      result.nets.push_back(
          {.name = "<null>", .type = 0, .driver = {},
           .loads = {}, .wires = {}});
      continue;
    }
    SourceNetFunctionalRecord record{
        .name = net->get_net_name(),
        .type = static_cast<std::int32_t>(net->get_connect_type()),
        .driver = endpointName(net->get_driving_pin()),
        .loads = {},
        .wires = {}};
    const auto capture_loads = [&](idb::IdbPins* pins) {
      if (pins != nullptr)
        for (auto* pin : pins->get_pin_list())
          if (pin != net->get_driving_pin())
            record.loads.push_back(endpointName(pin));
    };
    capture_loads(net->get_instance_pin_list());
    capture_loads(net->get_io_pins());
    std::ranges::sort(record.loads);
    if (auto* wires = net->get_wire_list(); wires != nullptr) {
      result.wire_count += wires->get_wire_list().size();
      for (auto* wire : wires->get_wire_list())
        record.wires.push_back(captureWire(wire, result));
    }
    result.nets.push_back(std::move(record));
  }
  std::ranges::sort(result.nets, {}, &SourceNetFunctionalRecord::name);
}

SourceIdbFunctionalProjection captureIdb(idb::IdbDesign* design)
{
  SourceIdbFunctionalProjection result;
  if (design == nullptr)
    return result;
  result.present = true;
  result.design_name = design->get_design_name();
  captureInstances(*design, result);
  captureIoPins(*design, result);
  captureNets(*design, result);
  return result;
}

}  // namespace

SourcePhysicalFunctionalReceipt captureTargetedSourcePhysical()
{
  return {
      .primary_idb = captureIdb(dmInst->get_idb_design()),
      .optional_netlist_view = captureIdb(
          dmInst->get_netlist_idb_design()),
      .spef_reader_present = dmInst->get_spef_reader() != nullptr};
}

}  // namespace ista::timing_dual_csr::test
