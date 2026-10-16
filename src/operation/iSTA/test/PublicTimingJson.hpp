// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sstream>

#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "StaProductionLifecycle.hpp"
#include "TimingAnalyzer.hpp"
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
}  // namespace ista

namespace ista::test {
inline void requireTimingOnlyFiles(const std::filesystem::path& directory)
{
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
    const auto ext = entry.path().extension();
    const auto name = entry.path().filename();
    if (entry.is_regular_file()
        && (ext == ".rpt" || ext == ".sdf" || ext == ".bin"
            || name == "qor_summary.json" || name == "timing_paths.json"))
      throw std::runtime_error("timing workload generated a disk artifact: " + entry.path().string());
  }
}
// Explicit acceptance output only. Fresh native and targeted computations
// share the public reporter, never their timing calculation.
inline nlohmann::json capturePublicTiming()
{
  auto& features = STADM.getConfig().output_timing_features;
  struct RestoreFeatures {
    std::int32_t& target;
    std::int32_t saved;
    ~RestoreFeatures() { target = saved; }
  } restore{features, features};
  features = 1;
  const std::filesystem::path directory = STADM.getConfig().tr_temp_directory_path;
  for (const char* name : {"qor_summary.json", "timing_paths.json"})
    std::filesystem::remove(directory / name);
  const auto status = executeStaLifecyclePipeline(
      {{"TimingReporter", [] { TimingReporter::initInst(); }, [] { STATR.report(); }, [] { TimingReporter::destroyInst(); }}});
  if (!status.succeeded)
    throw std::runtime_error("public timing report failed: " + status.message);
  nlohmann::json result;
  for (const auto& [key, name] : {std::pair{"qor", "qor_summary.json"}, std::pair{"paths", "timing_paths.json"}}) {
    std::ifstream input(directory / name);
    if (!input)
      throw std::runtime_error("public timing report is missing: " + std::string(name));
    result[key] = nlohmann::json::parse(input);
  }
  return result;
}

inline nlohmann::json analyzeAndCapturePublicTiming()
{
  // Native candidate preparation may already have analyzed its own values.
  // Keep propagated point/arc state, but replace the old report groups.
  const auto status = executeStaLifecyclePipeline(
      {{"TimingAnalyzer", [] { TimingAnalyzer::initInst(); }, [] { STATA.analyzePropagatedTiming(); }, [] { TimingAnalyzer::destroyInst(); }}},
      StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
  if (!status.succeeded)
    throw std::runtime_error("propagated timing analysis failed: " + status.message);
  return capturePublicTiming();
}
}  // namespace ista::test
