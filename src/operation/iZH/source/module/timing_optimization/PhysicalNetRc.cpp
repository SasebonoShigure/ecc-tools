// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PreRouteRc.hpp"
#include "IdbTimingPinName.hpp"
#include "IdbNet.h"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace izh {
ista::ParasiticNet buildPreRouteRcNet(idb::IdbNet& net, const PreRouteRcTechnology& technology)
{
  std::vector<PreRouteTerminal> terminals;
  std::size_t driver = 0, drivers = 0;
  for (auto* pins : {net.get_io_pins(), net.get_instance_pin_list()}) {
    for (auto* pin : pins->get_pin_list()) {
      if (!pin || !pin->get_term() || !pin->get_average_coordinate())
        throw std::invalid_argument("pre-route terminal is missing direction or geometry");
      const auto direction = pin->get_term()->get_direction();
      if (direction != idb::IdbConnectDirection::kInput && direction != idb::IdbConnectDirection::kOutput)
        throw std::invalid_argument("pre-route tree requires unidirectional terminals");
      if (direction == (pin->is_io_pin() ? idb::IdbConnectDirection::kInput : idb::IdbConnectDirection::kOutput)) {
        driver = terminals.size();
        ++drivers;
      }
      auto* coordinate = pin->get_average_coordinate();
      terminals.push_back({ista::idbTimingPinName(*pin), coordinate->get_x(), coordinate->get_y()});
    }
  }
  if (drivers != 1)
    throw std::invalid_argument("pre-route tree for net " + net.get_net_name() + " has " + std::to_string(drivers) + " drivers");
  std::swap(terminals.front(), terminals[driver]);
  return buildPreRouteRcNet(net.get_net_name(), terminals, technology);
}
}  // namespace izh
