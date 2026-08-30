// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <utility>

#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputRawProjectionInternal.hpp"
#include "Database.hpp"
#include "ParasiticLibrary.hpp"
#include "ParasiticNet.hpp"
#include "ParasiticNode.hpp"
#include "ParasiticResistor.hpp"

namespace ista::csr::test {

void projectCsrReferenceInputRawParasitics(const ista::Database& database, CsrReferenceInputRawProjection& projection)
{
  const auto& library = database.get_parasitic_library();
  if (library.get_spef_file_path().empty() && library.get_capacitive_unit().empty() && library.get_resistance_unit().empty() && library.get_net_map().empty())
    return;

  RawParasitics raw;
  raw.source_path = library.get_spef_file_path();
  raw.capacitance_unit = library.get_capacitive_unit();
  raw.resistance_unit = library.get_resistance_unit();
  raw.nets.reserve(library.get_net_map().size());
  for (const auto& [map_key, native] : library.get_net_map()) {
    RawParasiticNet net;
    net.map_key = map_key;
    net.embedded_name = native.get_net_name();
    net.total_capacitance = native.get_lumped_capacitance();
    net.nodes.reserve(native.get_node_map().size());
    net.resistors.reserve(native.get_resistor_list().size());
    for (const auto& [node_key, node] : native.get_node_map())
      net.nodes.push_back({node_key, node.get_node_name(), node.get_capacitance(), node.get_x(), node.get_y()});
    for (const auto& resistor : native.get_resistor_list())
      net.resistors.push_back({resistor.get_source_node(), resistor.get_sink_node(), resistor.get_resistance()});
    raw.nets.push_back(std::move(net));
  }
  projection.parasitics = std::move(raw);
}

namespace raw_wire {
namespace {

void appendParasiticNode(Writer& writer, const RawParasiticNode& node)
{
  writer.string(node.map_key);
  writer.string(node.embedded_name);
  writer.finite(node.capacitance);
  writer.finite(node.x);
  writer.finite(node.y);
}

void appendResistor(Writer& writer, const RawParasiticResistor& resistor)
{
  writer.string(resistor.source);
  writer.string(resistor.sink);
  writer.finite(resistor.resistance);
}

void appendParasiticNet(Writer& writer, const RawParasiticNet& net)
{
  writer.string(net.map_key);
  writer.string(net.embedded_name);
  writer.finite(net.total_capacitance);
  writer.vector(net.nodes, appendParasiticNode);
  writer.vector(net.resistors, appendResistor);
}

}  // namespace

void appendParasitics(Writer& writer, const RawParasitics& parasitics)
{
  writer.string(parasitics.source_path);
  writer.string(parasitics.capacitance_unit);
  writer.string(parasitics.resistance_unit);
  writer.vector(parasitics.nets, appendParasiticNet);
}

}  // namespace raw_wire
}  // namespace ista::csr::test
