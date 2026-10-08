// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrRealDifferentialOracleEncoding.hpp"
#include "CsrNativeSemanticKeyTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "Database.hpp"
#include <array>
#include <stdexcept>

void verifyNeutralArcIdentity()
{
  using namespace ista;
  using namespace ista::csr;
  Database database;
  for (const std::string port : {"A", "Y"}) {
    Pin pin;
    pin.set_full_name("u:" + port); pin.set_pin_name(port); pin.set_instance_name("u");
    database.get_pin_map().emplace(pin.get_full_name(), pin);
    database.get_timing_point_map().emplace(pin.get_full_name(), TimingPoint{});
    database.get_timing_order_list().push_back(pin.get_full_name());
  }
  database.get_timing_point_map().at("u:A").set_level(0);
  database.get_timing_point_map().at("u:Y").set_level(1);
  std::array<TimingCellArc, 2> cells;
  for (std::size_t index = 0; index < cells.size(); ++index) {
    auto& cell = cells[index];
    cell.set_source_port("A"); cell.set_sink_port("Y");
    TimingArc child;
    child.set_arc_idx(0); child.set_library_name("test.lib");
    child.set_sense(TimingArcSense::kPositive);
    child.set_sdf_cond(index == 0 ? "z" : "a");
    cell.get_timing_arc_list().push_back(std::move(child));
    Arc arc;
    arc.set_type(ArcType::kCell); arc.set_source_pin("u:A"); arc.set_sink_pin("u:Y");
    arc.set_owner_name("u"); arc.set_library_source_port("A"); arc.set_library_sink_port("Y");
    arc.set_timing_cell_arc(&cell);
    database.get_arc_list().push_back(std::move(arc));
  }
  database.get_outgoing_arc_list_map()["u:A"] = {0, 1};
  database.get_incoming_arc_list_map()["u:Y"] = {0, 1};
  database.get_timing_point_map().at("u:Y").set_predecessor("u:A");
  database.get_timing_point_map().at("u:Y").set_predecessor_arc_idx(0);
  const auto keys = test::nativeSemanticArcKeys(database);
  if (!(canonicalArcKeyBytes(keys[1]) < canonicalArcKeyBytes(keys[0]))
      || keys[0].parallel_ordinal != 0 || keys[1].parallel_ordinal != 0)
    throw std::runtime_error("conditional Arc fixture does not reverse canonical signature order");
  const auto wire = real_diff::readAndEncodeNativeTiming(database);
  for (const auto& row : wire.out_rows)
    if (row.node.full_name == "u:A" && row.arcs != keys)
      throw std::runtime_error("conditional outgoing Arc identities were rebound by visible endpoints");
  for (const auto& row : wire.in_rows)
    if (row.node.full_name == "u:Y" && row.arcs != keys)
      throw std::runtime_error("conditional incoming Arc identities were rebound by visible endpoints");
  for (const auto& node : wire.nodes)
    if (node.key.full_name == "u:Y" && node.predecessor_arc != keys[0])
      throw std::runtime_error("conditional Arc winner identity differs from adjacency");
}
