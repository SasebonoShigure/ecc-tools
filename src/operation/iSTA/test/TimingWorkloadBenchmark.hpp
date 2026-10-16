// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <sched.h>
#include "BenchmarkTimingConsumer.hpp"
#include "CompactWorkloadInput.hpp"
#include "IncreGpuStaBuildInfo.hpp"
#include "NativeFullEditPreparer.hpp"
#include "NativeCandidateExecutor.hpp"
#include "PublicTimingJson.hpp"
#if ISTA_ENABLE_CUDA
#include "CudaTargetedTraceReplayer.hpp"
#include "CudaTelemetryJson.hpp"
#endif

namespace ista::test {
using BenchmarkClock = std::chrono::steady_clock;
inline double benchmarkElapsed(BenchmarkClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(BenchmarkClock::now() - start).count();
}
inline void benchmarkRequire(bool valid, const char* message)
{
  if (!valid) throw std::runtime_error(message);
}
inline nlohmann::json benchmarkNativeFull(
    const timing_edit::oracle::ProductionNativeSessionView& view,
    const std::vector<timing_edit::EditBatch>& plans, std::size_t prefix,
    BenchmarkTimingConsumer& consumer, double& setup_ms, double& edit_sta_loop_ms, double& consumer_loop_ms)
{
  using namespace timing_edit;
  const auto setup_started = BenchmarkClock::now();
  auto state = NativeFullEditPreparer::bindBaseline(view.database, view.authority);
  benchmarkRequire(prefix > 0 && prefix <= plans.size(), "prefix outside frozen trace");
  auto timings = nlohmann::json::array();
  setup_ms = benchmarkElapsed(setup_started);
  const auto loop_started = BenchmarkClock::now();
  for (std::size_t index = 0; index < prefix; ++index) {
    const auto started = BenchmarkClock::now();
    auto prepared = NativeFullEditPreparer::prepare(view.database, view.authority, plans[index], state);
    const double native_edit_prepare_ms = benchmarkElapsed(started);
    const auto execute_started = BenchmarkClock::now();
    NativeCandidateExecutor::executeFullTiming(prepared.transaction());
    const double native_execution_ms = benchmarkElapsed(execute_started);
    const auto publication_started = BenchmarkClock::now();
    benchmarkRequire(prepared.commit(), "native full commit rejected");
    const double publication_ms = benchmarkElapsed(publication_started);
    const double commit_ms = benchmarkElapsed(started);
    if (index + 1 == prefix) edit_sta_loop_ms = benchmarkElapsed(loop_started);
    double report_prepare_ms{}, read_ms{};
    if (index + 1 == prefix) {
      const auto report_started = BenchmarkClock::now();
      const auto status = executeStaLifecyclePipeline(
          {{"TimingAnalyzer", [] { TimingAnalyzer::initInst(); }, [] { STATA.analyzePropagatedTiming(); },
            [] { TimingAnalyzer::destroyInst(); }}},
          StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
      if (status.cleanup_failed) view.authority.poison();
      if (!status.succeeded) throw std::runtime_error("final report preparation failed: " + status.message);
      report_prepare_ms = benchmarkElapsed(report_started);
      const auto query_started = BenchmarkClock::now();
      consumer();
      read_ms = benchmarkElapsed(query_started);
    }
    const double consumer_ms = benchmarkElapsed(started);
    timings.push_back({{"generation_after", state.generation().value()}, {"commit_ms", commit_ms},
                       {"native_edit_prepare_ms", native_edit_prepare_ms}, {"native_execution_ms", native_execution_ms},
                       {"publication_ms", publication_ms}, {"results_consumed", index + 1 == prefix},
                       {"query_materialize_ms", index + 1 == prefix ? nlohmann::json(0.0) : nlohmann::json(nullptr)},
                       {"report_prepare_ms", index + 1 == prefix ? nlohmann::json(report_prepare_ms) : nlohmann::json(nullptr)},
                       {"consumer_read_ms", index + 1 == prefix ? nlohmann::json(read_ms) : nlohmann::json(nullptr)},
                       {"consumer_step_ms", consumer_ms}, {"fallback", false},
                       {"executed_backend", "cpu"}, {"executed_strategy", "full"}});
  }
  consumer_loop_ms = benchmarkElapsed(loop_started);
  return timings;
}

inline nlohmann::json runTimingWorkloadBenchmark(const std::map<std::string, std::string>& options)
{
  using Json = nlohmann::json;
  benchmarkRequire(!incregpusta_debug_checks_enabled, "benchmark requires checks OFF before sampling");
  for (const char* flag : {"--replay", "--prefix", "--backend", "--strategy", "--threads", "--output"})
    benchmarkRequire(options.contains(flag), "missing benchmark option");
  benchmarkRequire(options.size() == 6, "unknown benchmark option");
  const auto integer = [&](const char* flag) {
    std::size_t value{};
    const auto& text = options.at(flag);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    benchmarkRequire(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0, "invalid positive integer");
    return value;
  };
  const auto prefix = integer("--prefix"), threads = integer("--threads");
  benchmarkRequire(threads <= std::size_t(std::numeric_limits<std::int32_t>::max()), "thread count out of range");
  const auto backend = options.at("--backend"), strategy = options.at("--strategy");
  benchmarkRequire((backend == "cpu" && strategy == "full") || (backend == "cuda" && strategy == "incremental"),
                   "benchmark supports native CPU/full and CUDA/incremental only");
#if !ISTA_ENABLE_CUDA
  benchmarkRequire(backend == "cpu", "CUDA workload requires ISTA_ENABLE_CUDA");
#endif
  const auto started = BenchmarkClock::now();
  Json timings, result, device = nullptr;
  double load_initialize_ms{}, backend_setup_ms{}, workload_ms{}, edit_sta_loop_ms{}, consumer_loop_ms{};
  auto cleanup_started = started;
  {
    auto input = readCompactWorkload(options.at("--replay"));
    benchmarkRequire(prefix == input.batches.size(), "benchmark requires the complete frozen trace");
    timing_dual_csr::test::DualCsrTargetedArtifactSupport artifacts(input.sources);
    requireWorkloadFiles(input, artifacts);
    auto supplied = artifacts.artifacts();
    supplied.threads = static_cast<std::int32_t>(threads);
    BenchmarkTimingConsumer consumer;
    if (backend == "cpu") {
      timing_edit::oracle::ProductionFreshSessionLoader::runNative(supplied, [&](const auto& view) {
        requireWorkloadScale(input, view.database);
        load_initialize_ms = benchmarkElapsed(started);
        const auto work_started = BenchmarkClock::now();
        timings = benchmarkNativeFull(view, input.batches, prefix, consumer, backend_setup_ms,
                                      edit_sta_loop_ms, consumer_loop_ms);
        workload_ms = benchmarkElapsed(work_started);
        cleanup_started = BenchmarkClock::now();
      });
    }
#if ISTA_ENABLE_CUDA
    else {
      timing_edit::oracle::ProductionFreshSessionLoader::run(supplied, [&](const auto& view) {
        requireWorkloadScale(input, view.database);
        auto program = cudaWorkloadReplay(std::move(input), view);
        load_initialize_ms = benchmarkElapsed(started);
        const auto work_started = BenchmarkClock::now();
        double read_ms{};
        const auto measured = csr::CudaTargetedTraceReplayer::run(view.session_context, view.database, view.reference_input,
            view.authority, program, prefix, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError, [&] {
              const auto read_started = BenchmarkClock::now();
              consumer.consumeCuda(view.session_context, view.database, view.authority);
              read_ms = benchmarkElapsed(read_started);
            });
        backend_setup_ms = measured.backend_setup_ms;
        edit_sta_loop_ms = measured.edit_sta_loop_ms;
        consumer_loop_ms = measured.consumer_loop_ms;
        const auto& info = measured.initialization.cuda;
        device = {{"name", info.device_name}, {"uuid", info.device_uuid},
                  {"driver_version", info.driver_version}, {"runtime_version", info.runtime_version}};
        timings = Json::array();
        for (const auto& t : measured.batches) {
          const auto& cuda = t.device.cuda;
          timings.push_back({{"generation_after", t.generation_after}, {"commit_ms", t.commit_ms},
              {"query_materialize_ms", t.results_consumed ? Json(t.query_materialize_ms) : nlohmann::json(nullptr)},
              {"report_prepare_ms", t.results_consumed ? Json(t.report_ms - read_ms) : nlohmann::json(nullptr)},
              {"consumer_read_ms", t.results_consumed ? Json(read_ms) : nlohmann::json(nullptr)},
              {"results_consumed", t.results_consumed}, {"consumer_step_ms", t.consumer_end_to_end_ms},
              {"fallback", t.execution.fallback_to_full}, {"executed_backend", "cuda"},
              {"executed_strategy", toString(t.execution.executed_update_strategy)},
              {"direct_prepare_ms", t.direct_prepare_ms}, {"csr_update_ms", t.device.update_ms},
              {"mixed_build_ms", t.device.mixed.mixed_build_ms}, {"mixed_kahn_ms", t.device.mixed.mixed_kahn_ms},
              {"mixed_prepare_ms", t.device.mixed_prepare_ms},
              {"cuda_phases", {{"encode_ms", cuda.encode_ms}, {"h2d_ms", cuda.h2d_ms},
                  {"levelization_ms", cuda.levelization_ms}, {"propagation_ms", cuda.propagation_ms},
                  {"delivery_d2h_ms", cuda.delivery_d2h_ms}, {"device_end_to_end_ms", cuda.end_to_end_ms}}},
              {"explicit_kernel_launches", cuda.kernel_launches}, {"device_algorithm_calls", cuda.device_algorithm_calls},
              {"levelization_decision_bytes", cuda.levelization_decision_bytes}, {"allocations", cuda.allocations},
              {"h2d_bytes", cuda.h2d_bytes}, {"d2h_bytes", cuda.d2h_bytes}, {"readback", cudaReadbackJson(cuda)}});
        }
        workload_ms = benchmarkElapsed(work_started);
        cleanup_started = BenchmarkClock::now();
      }, StaBackend::kCuda);
    }
#endif
    artifacts.close();
    benchmarkRequire(consumer.generations == 1, "final-only consumer count differs");
    result = {{"generation", prefix}, {"report_consumptions", consumer.generations},
              {"timing_node_count", consumer.values.size()}, {"baseline_instance_count", input.instances},
              {"baseline_timing_node_count", input.nodes}, {"public", std::move(consumer.reports)}};
  }
  const double cleanup_ms = benchmarkElapsed(cleanup_started);
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  benchmarkRequire(sched_getaffinity(0, sizeof(affinity), &affinity) == 0, "cannot read CPU affinity");
  Json cpus = Json::array();
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &affinity)) cpus.push_back(cpu);
  double total{};
  for (const auto& batch : timings) total += batch.at("consumer_step_ms").get<double>();
  return {{"schema_version", "ista-consumer-sample/3.0.0"}, {"report_schedule", "final_only"},
          {"debug_checks_enabled", false}, {"correctness_comparison", "not_run"}, {"diagnostics", "not_run"},
          {"build", timing_dual_csr::test::incregpustaBuildInfo()}, {"requested_backend", backend},
          {"requested_strategy", strategy}, {"threads_requested", threads}, {"cpu_affinity", cpus},
          {"thread_activity", "external_profile"}, {"device", device}, {"prefix_batches", prefix},
          {"result_requirements", {{"timing_values", "all_nodes_arrival_required_slack"}, {"qor", "complete"},
                                   {"max_paths", 20}, {"nworst", 20}, {"file_artifacts", false}}},
          {"load_initialize_ms", load_initialize_ms}, {"backend_setup_ms", backend_setup_ms},
          {"workload_ms", workload_ms}, {"edit_sta_loop_ms", edit_sta_loop_ms}, {"consumer_loop_ms", consumer_loop_ms},
          {"cleanup_ms", cleanup_ms}, {"cold_end_to_end_ms", benchmarkElapsed(started)},
          {"consumer_step_ms", total}, {"batch_timings", std::move(timings)},
          {"lifecycle_cleanup_completed", true}, {"result", std::move(result)}};
}
}  // namespace ista::test
