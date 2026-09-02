// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrSourceIsolationJson.hpp"

#include <stdexcept>

namespace ista::timing_dual_csr::test {
namespace {

using Json = nlohmann::json;

std::string bytesHex(std::string_view value)
{
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

Json artifact(const buffer_trace::SourceArtifactFunctionalRecord& value)
{
  return {{"role", value.role},
          {"logical_name", value.logical_name},
          {"canonical_path", value.canonical_path},
          {"size", value.size},
          {"bytes_hex", bytesHex(value.exact_bytes)}};
}

Json pin(const buffer_trace::SourcePinFunctionalRecord& value)
{
  return {{"name", value.name},
          {"instance", value.instance},
          {"net", value.net},
          {"direction", value.direction},
          {"io", value.io}};
}

Json instance(const buffer_trace::SourceInstanceFunctionalRecord& value)
{
  Json pins = Json::array();
  for (const auto& item : value.pins)
    pins.push_back(pin(item));
  return {{"name", value.name},
          {"master", value.master},
          {"type", value.type},
          {"status", value.status},
          {"orient", value.orient},
          {"x", value.x},
          {"y", value.y},
          {"pins", std::move(pins)}};
}

Json segment(const buffer_trace::SourceWireSegmentFunctionalRecord& value)
{
  Json points = Json::array();
  for (const auto& point : value.points)
    points.push_back({point[0], point[1], point[2]});
  Json rect = nullptr;
  if (value.delta_rect) {
    rect = {*value.delta_rect};
  }
  return {{"layer", value.layer},
          {"new_layer", value.new_layer},
          {"wire", value.wire},
          {"via", value.via},
          {"rect", value.rect},
          {"points", std::move(points)},
          {"vias", value.vias},
          {"delta_rect", std::move(rect)}};
}

Json wire(const buffer_trace::SourceWireFunctionalRecord& value)
{
  Json segments = Json::array();
  for (const auto& item : value.segments)
    segments.push_back(segment(item));
  return {{"statement", value.statement},
          {"shield", value.shield},
          {"segments", std::move(segments)}};
}

Json net(const buffer_trace::SourceNetFunctionalRecord& value)
{
  Json wires = Json::array();
  for (const auto& item : value.wires)
    wires.push_back(wire(item));
  return {{"name", value.name},
          {"type", value.type},
          {"driver", value.driver},
          {"loads", value.loads},
          {"wires", std::move(wires)}};
}

Json idb(const buffer_trace::SourceIdbFunctionalProjection& value)
{
  Json instances = Json::array();
  for (const auto& item : value.instances)
    instances.push_back(instance(item));
  Json pins = Json::array();
  for (const auto& item : value.io_pins)
    pins.push_back(pin(item));
  Json nets = Json::array();
  for (const auto& item : value.nets)
    nets.push_back(net(item));
  return {{"present", value.present},
          {"design_name", value.design_name},
          {"instances", std::move(instances)},
          {"io_pins", std::move(pins)},
          {"nets", std::move(nets)},
          {"connected_pin_count", value.connected_pin_count},
          {"placed_instance_count", value.placed_instance_count},
          {"fixed_instance_count", value.fixed_instance_count},
          {"unplaced_instance_count", value.unplaced_instance_count},
          {"wire_count", value.wire_count},
          {"segment_count", value.segment_count},
          {"wire_point_count", value.wire_point_count},
          {"via_count", value.via_count},
          {"wire_length", value.wire_length}};
}

Json physical(const buffer_trace::SourcePhysicalFunctionalReceipt& value)
{
  return {{"primary_idb", idb(value.primary_idb)},
          {"optional_netlist_view", idb(value.optional_netlist_view)},
          {"spef_reader_present", value.spef_reader_present}};
}

}  // namespace

nlohmann::json sourceIsolationToJson(
    const buffer_trace::SourceIsolationFunctionalReceipt& receipt)
{
  if (receipt.artifacts_before != receipt.artifacts_after
      || receipt.physical_before != receipt.physical_after) {
    throw std::runtime_error(
        "Stage 5 fresh adapter observed changed source evidence");
  }
  Json before = Json::array();
  Json after = Json::array();
  for (const auto& item : receipt.artifacts_before)
    before.push_back(artifact(item));
  for (const auto& item : receipt.artifacts_after)
    after.push_back(artifact(item));
  return {{"idb_before", physical(receipt.physical_before)},
          {"idb_after", physical(receipt.physical_after)},
          {"artifacts_before", std::move(before)},
          {"artifacts_after", std::move(after)},
          {"source_unchanged", true}};
}

}  // namespace ista::timing_dual_csr::test
