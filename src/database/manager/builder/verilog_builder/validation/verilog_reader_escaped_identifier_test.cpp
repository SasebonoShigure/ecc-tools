// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2. This file is a regression test.
// ***************************************************************************************

#include "verilog_read.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void testEscapedConcatIdentifiersRemainDistinctScalarNets()
{
  const auto verilog_path = std::filesystem::temp_directory_path() / "verilog_reader_escaped_identifier_test.v";
  {
    std::ofstream stream(verilog_path);
    require(stream.good(), "failed to create test Verilog file");
    stream << "module top;\n"
           << "  wire \\hierarchy[0].wdata_i[0] ;\n"
           << "  wire \\hierarchy[0].wdata_i[1] ;\n"
           << "  wire \\hierarchy[0].wdata_i[2] ;\n"
           << "  TEST_CELL \\hierarchy[0].u_mem (\n"
           << "      .D({ \\hierarchy[0].wdata_i[2] , \\hierarchy[0].wdata_i[1] , \\hierarchy[0].wdata_i[0] })\n"
           << "  );\n"
           << "endmodule\n";
  }

  idb::IdbLayout layout;
  idb::IdbDefService service(&layout);
  auto* master = layout.get_cell_master_list()->set_cell_master("TEST_CELL");
  require(master != nullptr, "failed to create test cell master");
  for (int index = 0; index != 3; ++index) {
    require(master->add_term("D[" + std::to_string(index) + "]") != nullptr, "failed to create test cell pin");
  }

  {
    idb::VerilogRead reader(&service);
    require(reader.createDb(verilog_path.string(), "top"), "failed to import test Verilog");
  }
  std::filesystem::remove(verilog_path);

  auto* net_list = service.get_design()->get_net_list();
  require(net_list != nullptr, "net list was not created");
  require(net_list->find_net("hierarchy[0].wdata_i") == nullptr, "concat must not create an unindexed base net");

  for (int index = 0; index != 3; ++index) {
    const std::string net_name = "hierarchy[0].wdata_i[" + std::to_string(index) + "]";
    auto* net = net_list->find_net(net_name);
    require(net != nullptr, "escaped scalar net is missing: " + net_name);
    require(net->get_instance_pin_list()->get_pin_num() == 1, "each escaped scalar net must connect one cell pin");
    require(net->get_instance_pin_list()->get_pin_list().front()->get_pin_name() == "D[" + std::to_string(index) + "]",
            "escaped scalar net connected to the wrong cell pin");
  }
}


void testIoConnections(bool auto_top)
{
  const auto verilog_path = std::filesystem::temp_directory_path() / "verilog_reader_io_connection_test.v";
  {
    std::ofstream stream(verilog_path);
    require(stream.good(), "failed to create IO test Verilog file");
    stream << "module top(source, bus, sink, \\hierarchy[0].port[0] );\n"
           << "input source;\ninput [1:0] bus;\noutput sink;\noutput \\hierarchy[0].port[0] ;\n"
           << "wire source, sink, internal;\nwire \\hierarchy[0].port[0] ;\n"
           << "TEST_BUF first (.A(source), .Z(internal));\n"
           << "TEST_BUF second (.A(internal), .Z(sink));\n"
           << "TEST_BUF third (.A(bus[1]), .Z(\\hierarchy[0].port[0] ));\n"
           << "endmodule\n";
  }
  idb::IdbLayout layout;
  idb::IdbDefService service(&layout);
  auto* existing_source = service.get_design()->createOrFindIoPin("source");
  require(existing_source != nullptr, "failed to create existing IO pin");
  auto* master = layout.get_cell_master_list()->set_cell_master("TEST_BUF");
  require(master != nullptr, "failed to create IO test master");
  require(master->add_term("A") != nullptr && master->add_term("Z") != nullptr, "failed to create IO test terms");
  {
    idb::VerilogRead reader(&service);
    require(auto_top ? reader.createDbAutoTop(verilog_path.string()) : reader.createDb(verilog_path.string(), "top"),
            "failed to import IO test Verilog");
  }
  std::filesystem::remove(verilog_path);
  auto* design = service.get_design();
  require(design->get_io_pin_list()->find_pin("source") == existing_source, "existing IO identity changed");
  require(design->get_io_pin_list()->get_pin_list().size() == 5, "IO import duplicated or dropped a port");
  for (const auto* name : {"source", "bus[1]", "sink", "hierarchy[0].port[0]"}) {
    auto* pin = design->get_io_pin_list()->find_pin(name);
    auto* net = design->get_net_list()->find_net(name);
    require(pin != nullptr && net != nullptr, std::string("missing IO or net: ") + name);
    require(pin->get_net() == net, std::string("IO connected to wrong net: ") + name);
    require(net->get_io_pins()->get_pin_list().size() == 1 && net->get_io_pins()->get_pin_list().front() == pin,
            std::string("IO membership missing or duplicated: ") + name);
    require(net->get_instance_pin_list()->get_pin_num() == 1, std::string("missing instance connection: ") + name);
  }
  auto* internal = design->get_net_list()->find_net("internal");
  require(internal != nullptr && internal->get_io_pins()->get_pin_list().empty(), "internal net gained an IO pin");
  require(internal->get_instance_pin_list()->get_pin_num() == 2, "internal net lost an instance connection");
}

void testManyScalarPorts()
{
  const auto verilog_path = std::filesystem::temp_directory_path() / "verilog_reader_many_ports_test.v";
  constexpr int count = 4096;
  {
    std::ofstream stream(verilog_path);
    require(stream.good(), "failed to create many-port Verilog");
    stream << "module top(";
    for (int i = 0; i < count; ++i) stream << (i ? "," : "") << "port_" << i;
    stream << ");\n";
    for (int i = 0; i < count; ++i) stream << "input port_" << i << ";\n";
    stream << "endmodule\n";
  }
  idb::IdbLayout layout;
  idb::IdbDefService service(&layout);
  {
    idb::VerilogRead reader(&service);
    require(reader.createDb(verilog_path.string(), "top"), "failed to import many-port Verilog");
  }
  std::filesystem::remove(verilog_path);
  const auto& pins = service.get_design()->get_io_pin_list()->get_pin_list();
  require(pins.size() == count, "many-port count differs");
  for (int i = 0; i < count; ++i) {
    auto* pin = pins[i];
    require(pin->get_pin_name() == "port_" + std::to_string(i), "port order or name changed");
    require(pin->get_term() != nullptr && pin->get_term()->get_direction() == idb::IdbConnectDirection::kInput,
            "port direction was not initialized");
    require(pin->get_net() != nullptr && pin->get_net()->get_net_name() == pin->get_pin_name(), "port net differs");
  }
}

void testDesignUnitsOwnership()
{
  idb::IdbDesign design;
  auto* units = new idb::IdbUnits();
  units->set_microns_dbu(1000);
  design.set_units(units);
  design.set_units(units);
  require(design.get_units()->get_micron_dbu() == 1000, "unit self-assignment lost the value");
  design.set_units(new idb::IdbUnits());
  design.set_units(nullptr);
  require(design.get_units() == nullptr, "clearing units retained a stale pointer");
  design.set_units(new idb::IdbUnits());
}

}  // namespace

int main()
{
  try {
    testEscapedConcatIdentifiersRemainDistinctScalarNets();
    testIoConnections(false);
    testIoConnections(true);
    testManyScalarPorts();
    testDesignUnitsOwnership();
  } catch (const std::exception& error) {
    std::cout << error.what() << '\n';
    return 1;
  }
  return 0;
}
