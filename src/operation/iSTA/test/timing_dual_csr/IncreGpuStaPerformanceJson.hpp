// IncreGPUSTA workload output; contains no diagnostic receipt.
#pragma once

#include <unistd.h>

#include "DualCsrReplayTypes.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "json.hpp"

namespace ista::timing_dual_csr::test {

inline nlohmann::json incregpustaBuildInfo()
{
  return {{"debug_checks_enabled", incregpusta_debug_checks_enabled},
          {"build_config", incregpusta_build_config},
          {"compiler_id", incregpusta_compiler_id},
          {"compiler_version", incregpusta_compiler_version}};
}

inline nlohmann::json performanceResultToJson(const DualCsrTraceRunResult& replay, std::uint64_t prefix, nlohmann::json phase_timings)
{
  for (auto& phase : phase_timings) {
    phase["invariant_ms"] = "not_run";
    phase["verify_ms"] = "not_run";
  }
  const auto& actual = replay.result;
  const char* mode = actual.executed_mode == LevelizationExecutionMode::kMixedStage6 ? "mixed-stage6" : "full-stage5";
  return {{"schema_version", "ista-incregpusta-performance-process/3.0.0"},
          {"build", incregpustaBuildInfo()},
          {"debug_checks_enabled", incregpusta_debug_checks_enabled},
          {"correctness_comparison", "not_run"},
          {"diagnostics", "not_run"},
          {"result_requirements", {{"timing_values", true}, {"summary", true}, {"timing_paths", true}, {"file_artifacts", false}}},
          {"prefix_batches", prefix},
          {"requested_mode", actual.requested_mode == LevelizationExecutionMode::kMixedStage6 ? "mixed-stage6" : "full-stage5"},
          {"input", {{"node_slots", actual.input_node_slots}, {"arc_slots", actual.input_arc_slots}, {"batches", prefix}}},
          {"executed_mode", mode},
          {"result",
           {{"generation", actual.generation},
            {"node_slots", actual.node_slots},
            {"arc_slots", actual.arc_slots},
            {"fallback_count", actual.fallback_count},
            {"lifecycle_cleanup_completed", actual.lifecycle_cleanup_completed}}},
          {"run", {{"child_pid", static_cast<std::uint64_t>(::getpid())}, {"batch_phase_timings", std::move(phase_timings)}}}};
}

}  // namespace ista::timing_dual_csr::test
