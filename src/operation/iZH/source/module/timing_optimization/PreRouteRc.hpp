// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include "ParasiticNet.hpp"

namespace idb { class IdbLayout; class IdbNet; }
namespace izh {
struct PreRouteTerminal
{
  std::string name;
  std::int32_t x_dbu{}, y_dbu{};
};
struct PreRouteLayerRc
{
  std::string name;
  double width_um{}, resistance_per_um{}, area_capacitance_per_um{}, edge_capacitance_per_um{};
};
struct PreRouteRcTechnology
{
  std::int32_t dbu_per_um{};
  PreRouteLayerRc horizontal, vertical;
};
// Resolve only the configured routing interval. Empty H/V selects its lowest
// layer with that direction. DEF and LEF coordinates must use the same DBU.
[[nodiscard]] PreRouteRcTechnology preparePreRouteRcTechnology(idb::IdbLayout& layout, std::int32_t design_dbu,
    const std::string& bottom, const std::string& top, const std::string& horizontal = {}, const std::string& vertical = {});
// Driver is first; all other terminal order is immaterial. Output units are
// OHM/PF, coordinates are microns, and no Liberty pin capacitance is included.
[[nodiscard]] ista::ParasiticNet buildPreRouteRcNet(const std::string& name,
    std::span<const PreRouteTerminal> terminals, const PreRouteRcTechnology& technology);
// Uses real oriented pin coordinates and the same terminal identity as STA input.
[[nodiscard]] ista::ParasiticNet buildPreRouteRcNet(idb::IdbNet& net, const PreRouteRcTechnology& technology);
}  // namespace izh
