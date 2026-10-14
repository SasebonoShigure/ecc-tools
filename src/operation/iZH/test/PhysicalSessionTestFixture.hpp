// Physical-session test fixtures shared by transaction and candidate checks.
#pragma once
#include "idm.h"
#include "DataManager.hpp"
#include "PhysicalStaSessionState.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
namespace ista::physical_test {
inline void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
inline void constrainPorts(Database& database)
{
  auto& constraints = database.get_timing_constraint().get_port_constraint_map();
  auto& input = constraints["in"];
  input.set_port_name("in");
  input.set_input_delay_max(0.1); input.set_has_input_delay_max(true);
  input.set_input_transition(0.07); input.set_has_input_transition(true);
  for (const auto& [name, pin] : database.get_pin_map())
    if (pin.get_is_port() && pin.get_direction() == PinDirection::kOutput) {
      auto& output = constraints[name];
      output.set_port_name(name);
      output.set_output_delay_max(0.2); output.set_has_output_delay_max(true);
    }
}
struct PhysicalDesign
{
  idb::IdbLayout layout;
  idb::IdbDefService service{&layout};
  idb::IdbDesign& design{*service.get_design()};
  idb::IdbDefService* previous{dmInst->get_idb_def_service()};
  const izh::PreRouteRcTechnology rc{1000, {"M1", 0.2, 1.0, 0.002, 0.001}, {"M2", 0.4, 0.5, 0.004, 0.001}};
  PhysicalDesign()
  {
    layout.set_units(new idb::IdbUnits());
    layout.get_units()->set_microns_dbu(1000);
    design.get_units()->set_microns_dbu(1000);
    auto* site = layout.get_sites()->add_site_list("site");
    site->set_width(1000); site->set_height(2000);
    layout.get_rows()->createRow("row", site, 0, 7000, idb::IdbOrient::kN_R0, 20, 1, 1000, 0);
    for (bool vertical : {false, true}) {
      auto* layer = dynamic_cast<idb::IdbLayerRouting*>(layout.get_layers()->set_layer(vertical ? "M2" : "M1", "ROUTING"));
      require(layer, "physical test layer missing");
      layout.get_layers()->add_routing_layer(layer);
      layer->set_direction(vertical ? idb::IdbLayerDirection::kVertical : idb::IdbLayerDirection::kHorizontal);
      layer->set_width(vertical ? 400 : 200);
      layer->set_resistance(0.2); layer->set_capacitance(0.01); layer->set_edge_capacitance(0.001);
    }
    auto* master = layout.get_cell_master_list()->set_cell_master("BUF_X1");
    master->set_site(site);
    master->set_width(1000); master->set_height(2000); master->set_type(idb::CellMasterType::kCore);
    for (bool output : {false, true}) {
      auto* term = master->add_term(output ? "Y" : "A");
      term->set_direction(output ? idb::IdbConnectDirection::kOutput : idb::IdbConnectDirection::kInput);
      term->set_type(idb::IdbConnectType::kSignal);
      term->set_average_position(output ? 800 : 200, output ? 1400 : 600);
      term->set_bounding_box(0, 0, 1000, 2000);
      auto* shape = term->add_port()->add_layer_shape();
      shape->set_layer(layout.get_layers()->find_layer("M1"));
      shape->add_rect(output ? 750 : 150, output ? 1300 : 500,
                      output ? 850 : 250, output ? 1500 : 700);
    }
    auto* net = design.createOrFindNet("n0", idb::IdbConnectType::kSignal);
    for (bool output : {false, true}) {
      auto* pin = design.createOrFindIoPin(output ? "out" : "in");
      pin->set_term()->set_direction(output ? idb::IdbConnectDirection::kOutput : idb::IdbConnectDirection::kInput);
      pin->set_average_coordinate(output ? 15000 : 0, 0);
      require(design.connectPinToNet(pin, net), "initial physical connection failed");
    }
    dmInst->set_idb_def_service(&service);
  }
  ~PhysicalDesign() { dmInst->set_idb_def_service(previous); }
};
struct RecoveryDirectory
{
  std::filesystem::path path;
  RecoveryDirectory()
  {
    auto pattern = (std::filesystem::temp_directory_path() / "ista-physical-batch-XXXXXX").string();
    auto* made = ::mkdtemp(pattern.data());
    if (!made) throw std::runtime_error("physical recovery test directory failed");
    path = made;
    std::ofstream(path / "fixture.txt") << "native transaction fixture / IDB matching n0, in and out";
  }
  ~RecoveryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
  PhysicalStaConfig config() const
  {
    return {"M1", "M2", {}, {}, (path / "journal").string(), {{"component_fixture", (path / "fixture.txt").string()}}};
  }
};
}
