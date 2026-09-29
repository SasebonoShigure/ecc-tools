// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <array>
#include <sstream>
#include <vector>
#include "DataManager.hpp"
#include "TimingReporter.hpp"
#include "json.hpp"

namespace ista {
class TimingReporterProbeAccess final
{
 public:
  static nlohmann::json read()
  {
    TimingReporter reporter;
    std::ostringstream qor, paths;
    reporter.outputQorSummaryReport(&qor);
    reporter.outputTimingPathsJson(&paths);
    return {{"qor", nlohmann::json::parse(qor.str())}, {"paths", nlohmann::json::parse(paths.str())}};
  }
};
namespace test {
// The common consumer reads every node's scalar timing and the complete public
// QOR/limited paths. It never exports a graph, recomputes timing, or writes reports.
struct BenchmarkTimingConsumer
{
  std::vector<std::array<double, 3>> values;
  nlohmann::json reports;
  std::size_t generations{};
  void operator()()
  {
    const auto& points = STADM.getDatabase().get_timing_point_map();
    values.clear();
    values.reserve(points.size());
    for (const auto& [name, point] : points) {
      static_cast<void>(name);
      values.push_back({point.get_arrival(), point.get_required(), point.get_slack()});
    }
    reports = TimingReporterProbeAccess::read();
    ++generations;
  }
};
}  // namespace test
}  // namespace ista
