// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <array>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <vector>
#include "IncreGpuStaBuildConfig.hpp"
#include "DataManager.hpp"
#include "PublicTimingJson.hpp"
#include "json.hpp"
#if ISTA_ENABLE_CUDA
#include "StaCsrSessionContext.hpp"
#include "StaCudaSessionState.hpp"
#endif

namespace ista {
namespace test {
// The common consumer reads every node's scalar timing and the complete public
// QOR/limited paths. It never exports a graph, recomputes timing, or writes reports.
struct BenchmarkTimingConsumer
{
  std::vector<std::array<double, 3>> values;
  nlohmann::json reports;
  std::size_t generations{};
#if ISTA_ENABLE_CUDA
  void consumeCuda(StaCsrSessionContext& context, Database& database, const timing_edit::NativeExecutionAuthority& authority)
  {
    const auto& report = context.readCudaReport(database, authority);
    auto& state = context.cudaState(authority);
    const auto& nodes = state.host->graph.nodes;
    const auto& timing = state.device.timingValues().nodes;
    std::vector<std::size_t> order(nodes.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order, {}, [&](std::size_t id) -> const std::string& { return nodes[id].key.full_name; });
    values.clear();
    values.reserve(nodes.size());
    for (const auto id : order)
      values.push_back({timing.arrivals.values[id], timing.requireds.values[id], timing.slacks.values[id]});
    reports = {{"qor", nlohmann::json::parse(report.first)}, {"paths", nlohmann::json::parse(report.second)}};
    ++generations;
  }
#endif
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
