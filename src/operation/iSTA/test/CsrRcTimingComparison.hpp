#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "CsrReferenceTypes.hpp"

namespace ista::csr::test {
inline void rcTimingRequire(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
// Full component comparison: floating lanes use the existing RC tolerance;
// identity, presence, levels, winners, paths and all other fields remain exact.
inline void compareRcTiming(const CsrTimingValues& actual, const CsrTimingValues& expected)
{
  auto normalized = actual;
  const auto numbers = [](OptionalSoa<double>& a, const OptionalSoa<double>& b) {
    rcTimingRequire(a.presence == b.presence && a.values.size() == b.values.size(), "RC timing numeric presence differs");
    for (std::size_t i = 0; i < a.values.size(); ++i)
      rcTimingRequire(a.values[i] == b.values[i] || (std::isfinite(a.values[i]) && std::isfinite(b.values[i])
          && std::abs(a.values[i] - b.values[i]) <= 1e-6), "RC CPU/CUDA numeric lane differs");
    a.values = b.values;
  };
  numbers(normalized.nodes.arrivals, expected.nodes.arrivals);
  numbers(normalized.nodes.requireds, expected.nodes.requireds);
  numbers(normalized.nodes.slacks, expected.nodes.slacks);
  numbers(normalized.nodes.launches, expected.nodes.launches);
  for (auto pair : {std::pair{&normalized.physical_clock, &expected.physical_clock},
                    std::pair{&normalized.effective_clock, &expected.effective_clock}}) {
    numbers(pair.first->arrivals, pair.second->arrivals);
    numbers(pair.first->slews, pair.second->slews);
    numbers(pair.first->predecessor_arc_delays, pair.second->predecessor_arc_delays);
  }
  numbers(normalized.data_slews, expected.data_slews);
  numbers(normalized.paths.arrivals, expected.paths.arrivals);
  numbers(normalized.paths.slews, expected.paths.slews);
  numbers(normalized.paths.launches, expected.paths.launches);
  numbers(normalized.paths.predecessor_arc_delays, expected.paths.predecessor_arc_delays);
  numbers(normalized.arcs.delays, expected.arcs.delays);
  numbers(normalized.arcs.min_delays, expected.arcs.min_delays);
  numbers(normalized.arcs.max_delays, expected.arcs.max_delays);
  numbers(normalized.arcs.transition_delays, expected.arcs.transition_delays);
  numbers(normalized.arcs.input_output_delays, expected.arcs.input_output_delays);
  numbers(normalized.arcs.graph_delays, expected.arcs.graph_delays);
  numbers(normalized.arcs.timing_arc_delays, expected.arcs.timing_arc_delays);
  rcTimingRequire(normalized == expected, "RC CPU/CUDA topology/winner/path identity differs");
}

}  // namespace ista::csr::test
