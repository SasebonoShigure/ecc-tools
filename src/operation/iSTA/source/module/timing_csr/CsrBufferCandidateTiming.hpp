// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <cstdint>
#include <limits>
#include "CsrReferenceTypes.hpp"

namespace ista::csr {
// Only the values consumed by physical buffer selection. Infinite slack means
// no finite max-analysis path; negative slew means no usable lane.
struct BufferCandidateTiming {
  double setup_slack{std::numeric_limits<double>::infinity()};
  double data_slew{-1.0};
  double clock_slew{-1.0};
  // A full-width flag avoids unwritten padding in the device-to-host record.
  std::uint64_t clock_point{0};
};
static_assert(sizeof(BufferCandidateTiming) == 3 * sizeof(double) + sizeof(std::uint64_t));
BufferCandidateTiming bufferCandidateTiming(const CsrTimingValues& values, NodeId node);
}  // namespace ista::csr
