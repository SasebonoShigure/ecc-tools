// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "PhysicalPlacementRows.hpp"
#include <cstdint>
#include <map>

namespace ista {
struct PhysicalStaPlacement {
  std::uint64_t generation{};
  std::int32_t dbu_per_um{};
  izh::PhysicalPlacementRows::Geometry geometry;
  std::map<std::string, std::array<std::int32_t, 2>> cells;
};
}  // namespace ista
