// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace ista::csr::value_detail {

using Index = std::uint64_t;
inline constexpr Index absent = std::numeric_limits<Index>::max();

template <class T> struct Value
{
  T value{};
  bool present{false};
};

struct ClockLane
{
  Value<double> arrival, slew;
  Index predecessor{absent};
  double predecessor_delay{0.0};
  std::uint8_t predecessor_transition{2};
};

struct NodeTimingScalars
{
  double arrival{-std::numeric_limits<double>::infinity()};
  double required{std::numeric_limits<double>::infinity()};
  double slack{0.0};
};

struct NodeDetails
{
  double launch{0.0};
  Index clock_name{absent};
  Index predecessor_node{absent};
  Index predecessor_arc{absent};
  Value<double> slew[4];
  bool clock_point{false};
  bool propagated_clock{false};
  ClockLane physical_clock[4];
};

struct NodeValue : NodeTimingScalars, NodeDetails {};

struct PathState
{
  double arrival{0.0};
  double slew{0.0};
  double launch{0.0};
  double predecessor_delay{0.0};
  Index start{0};
  Index predecessor_node{absent};
  Index predecessor_arc{absent};
  std::uint8_t predecessor_transition{2};
  Index crpr_node{absent};
  std::uint8_t crpr_transition{2};
  bool present{false};
};

static_assert(std::is_trivially_copyable_v<NodeTimingScalars> && std::is_trivially_copyable_v<NodeDetails>
              && std::is_trivially_copyable_v<NodeValue> && std::is_trivially_copyable_v<PathState>);

}  // namespace ista::csr::value_detail
