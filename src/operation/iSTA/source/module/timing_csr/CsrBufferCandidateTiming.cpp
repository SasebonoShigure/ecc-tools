// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrBufferCandidateTiming.hpp"
#include <algorithm>
#include <cmath>

namespace ista::csr {
BufferCandidateTiming bufferCandidateTiming(const CsrTimingValues& values, NodeId node)
{
  const auto id = node.value();
  BufferCandidateTiming result;
  result.clock_point = values.nodes.is_clock_point.presence.at(id)
                       && values.nodes.is_clock_point.values.at(id);
  for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
    const auto lane = id * kAnalysisCount * kTransitionCount + transition;
    const auto update = [lane](double& target, const OptionalSoa<double>& source) {
      if (source.presence.at(lane) && std::isfinite(source.values.at(lane)) && source.values.at(lane) >= 0.0)
        target = std::max(std::max(0.0, target), source.values.at(lane));
    };
    update(result.data_slew, values.data_slews);
    update(result.clock_slew, values.physical_clock.slews);
  }
  if (result.clock_point || !values.nodes.arrivals.presence.at(id) || !values.nodes.requireds.presence.at(id)
      || !values.nodes.slacks.presence.at(id) || !std::isfinite(values.nodes.arrivals.values.at(id))
      || !std::isfinite(values.nodes.requireds.values.at(id)) || !std::isfinite(values.nodes.slacks.values.at(id)))
    return result;
  const auto row = id * kAnalysisCount * kPathSourceCount * kTransitionCount;
  for (auto slot = values.paths.row_offsets.at(row);
       slot < values.paths.row_offsets.at(row + kPathSourceCount * kTransitionCount); ++slot) {
    if (values.paths.arrivals.presence.at(slot) && std::isfinite(values.paths.arrivals.values.at(slot))) {
      result.setup_slack = values.nodes.slacks.values.at(id);
      break;
    }
  }
  return result;
}
}  // namespace ista::csr
