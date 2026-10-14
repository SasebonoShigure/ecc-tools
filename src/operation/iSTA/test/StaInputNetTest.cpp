// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "DataManager.hpp"
#include "Logger.hpp"
#include "STAInterface.hpp"
#include "Utility.hpp"
#include "idm.h"

namespace {
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv)
{
  try {
    require(argc == 3, "expected fixture and isolated output directories");
    const std::filesystem::path fixtures(argv[1]);
    require(dmInst->readLef(std::vector<std::string>{(fixtures / "stage0_tech.lef").string()}, true), "technology load failed");
    require(dmInst->readLef(std::vector<std::string>{(fixtures / "stage0_cells.lef").string()}), "cell load failed");
    require(dmInst->readVerilog((fixtures / "chain.v").string(), "stage0_chain"), "Verilog load failed");
    require(dmInst->readLib(std::vector<std::string>{(fixtures / "stage0.lib").string()}), "Liberty load failed");
    dmInst->get_config().set_sdc_path((fixtures / "chain.sdc").string());

    ista::Logger::initInst();
    ista::Utility::initInst();
    ista::DataManager::initInst();
    auto& database = STADM.getDatabase();
    auto& prior = database.get_net_map()["zz_prior"];
    prior.set_net_name("zz_prior");
    prior.set_pin_name_list({"u2:A", "u0:Y", "u2:A", "u0:Y"});
    prior.set_driver_pin("old_driver");
    prior.set_driver_pin_list({"old_driver"});
    prior.set_load_pin_list({"old_load"});
    database.get_pin_map()["detached"].set_net_name("old_net");

    auto* idb_net = dmInst->get_idb_design()->get_net_list()->find_net("data_in");
    require(idb_net != nullptr, "missing input net");
    auto& io = idb_net->get_io_pins()->get_pin_list();
    auto& pins = idb_net->get_instance_pin_list()->get_pin_list();
    require(io.size() == 1 && pins.size() == 1, "unexpected fixture input connectivity");
    io.push_back(io.front());
    pins.push_back(pins.front());

    std::map<std::string, std::any> config{
        {"-temp_directory_path", std::filesystem::absolute(argv[2]).string()},
        {"-output_timing_reports", std::int32_t{0}},
        {"-output_timing_features", std::int32_t{0}},
        {"-thread_number", std::int32_t{1}}};
    try {
      STADM.input(config);
    } catch (...) {
      io.pop_back();
      pins.pop_back();
      throw;
    }
    io.pop_back();
    pins.pop_back();

    auto& net = database.get_net_map().at("data_in");
    const std::vector<std::string> expected{"data_in", "u0:A"};
    require(net.get_pin_name_list() == expected, "input wrapping changed first-seen IO/instance order");
    require(net.get_load_pin_list() == expected, "input loads lost names or duplicated pins");
    require(prior.get_load_pin_list() == std::vector<std::string>{"u2:A", "u0:Y"},
            "load preparation changed first-seen order or retained duplicates");
    require(prior.get_driver_pin().empty() && prior.get_driver_pin_list().empty(), "stale driver cache survived input");
    require(database.get_pin_map().at("u2:A").get_net_name() == "zz_prior", "pin net binding was not rebuilt");
    require(database.get_pin_map().at("detached").get_net_name().empty(), "detached pin retained an old net");

    ista::DataManager::destroyInst();
    ista::Utility::destroyInst();
    ista::Logger::destroyInst();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
