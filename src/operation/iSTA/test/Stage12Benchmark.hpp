// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <sched.h>
#include <stdexcept>
#include "BenchmarkTimingConsumer.hpp"
#include "BufferTraceReplayInput.hpp"
#include "CudaFullTraceReplayer.hpp"
#include "CudaTargetedTraceReplayer.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "DualCsrTraceReplayer.hpp"
#include "IncreGpuStaPerformanceJson.hpp"
#include "NativeEditTransaction.hpp"
#include "NativeFullEditPreparer.hpp"
#include "ProductionFreshSessionLoader.hpp"
#include "StaCsrSessionContext.hpp"
#include "TimingEditResolver.hpp"

namespace ista::test {
using BenchmarkClock = std::chrono::steady_clock;
inline double benchmarkElapsed(BenchmarkClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(BenchmarkClock::now() - start).count();
}
inline void benchmarkRequire(bool valid, const char* message)
{
  if (!valid)
    throw std::runtime_error(message);
}
// Production native full transaction, with no CSR propagation, receipt,
// fresh oracle, or second report analysis.
inline nlohmann::json benchmarkNativeFull(
    const timing_edit::oracle::ProductionFreshSessionView& view,
    const buffer_trace::ReplayProgram& program, std::size_t prefix,
    BenchmarkTimingConsumer& consumer, double& setup_ms)
{
  using namespace timing_edit;
  const auto setup_started = BenchmarkClock::now();
  auto state = NativeFullEditPreparer::bindBaseline(view.database, view.reference_input, program.baseline());
  const auto& plans = std::visit([](const auto& workload) -> const std::vector<EditBatch>& { return workload.batches; }, program.workload);
  benchmarkRequire(prefix > 0 && prefix <= plans.size(), "prefix outside frozen trace");
  auto timings = nlohmann::json::array();
  setup_ms = benchmarkElapsed(setup_started);
  for (std::size_t index = 0; index < prefix; ++index) {
    const auto started = BenchmarkClock::now();
    auto prepared = NativeFullEditPreparer::prepare(view.database, view.authority, plans[index], state);
    NativeCandidateExecutor::executeFull(prepared.transaction());
    benchmarkRequire(prepared.commit(), "native full commit rejected");
    const double commit_ms = benchmarkElapsed(started);
    const auto query_started = BenchmarkClock::now();
    consumer();
    const double read_ms = benchmarkElapsed(query_started);
    const double consumer_ms = benchmarkElapsed(started);
    timings.push_back({{"generation_after", state.generation().value()}, {"commit_ms", commit_ms},
                       {"query_materialize_ms", 0.0}, {"consumer_read_ms", read_ms},
                       {"consumer_step_ms", consumer_ms}, {"fallback", false},
                       {"executed_backend", "cpu"}, {"executed_strategy", "full"}});
  }
  return timings;
}

inline nlohmann::json runStage12Benchmark(const std::map<std::string, std::string>& options)
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
  const auto prefix = integer("--prefix");
  const auto threads = integer("--threads");
  benchmarkRequire(threads <= std::size_t(std::numeric_limits<std::int32_t>::max()), "thread count out of range");
  const auto backend = options.at("--backend");
  const auto strategy = options.at("--strategy");
  benchmarkRequire(backend == "cpu" || backend == "cuda", "unknown backend");
  benchmarkRequire(strategy == "full" || strategy == "incremental", "unknown strategy");
  const auto started = BenchmarkClock::now();
  Json timings;
  Json device = nullptr;
  const auto device_info = [](const csr::CsrCudaTelemetry& info) {
    return Json{{"name", info.device_name}, {"uuid", info.device_uuid},
                {"driver_version", info.driver_version}, {"runtime_version", info.runtime_version}};
  };
  double load_initialize_ms{}, backend_setup_ms{}, workload_ms{};
  auto cleanup_started = BenchmarkClock::now();
  Json result;
  {
    const auto program = buffer_trace::test::readReplayInput(options.at("--replay"), true);
    timing_dual_csr::test::DualCsrTargetedArtifactSupport artifacts(program);
    auto supplied = artifacts.artifacts();
    supplied.threads = static_cast<std::int32_t>(threads);
    BenchmarkTimingConsumer consumer;
    timing_edit::oracle::ProductionFreshSessionLoader::run(supplied, [&](const auto& view) {
      load_initialize_ms = benchmarkElapsed(started);
      const auto workload_started = BenchmarkClock::now();
      if (backend == "cpu" && strategy == "full") {
        timings = benchmarkNativeFull(view, program, prefix, consumer, backend_setup_ms);
      } else if (backend == "cpu") {
        timing_dual_csr::DualCsrTraceContext context{
            view.database, view.reference_input, view.authority, view.session_token,
            view.session_context.libraryModels(view.database), [&] { consumer(); }};
        const auto result = timing_dual_csr::DualCsrTraceReplayer::replayMixedPrefix(context, program, prefix);
        backend_setup_ms = result.envelope.backend_setup_ms;
        timings = Json::array();
        for (const auto& t : result.envelope.batch_phase_timings)
          timings.push_back({{"generation_after", t.generation_after}, {"commit_ms", t.session_step_ms},
                             {"query_materialize_ms", t.query_materialization_ms}, {"consumer_read_ms", t.report_ms},
                             {"consumer_step_ms", t.consumer_step_ms}, {"fallback", result.result.fallback_count != 0},
                             {"executed_backend", "cpu"}, {"executed_strategy", "incremental"}});
      } else if (strategy == "full") {
        const auto result = csr::replayCudaFullTracePrefix(view.session_context, view.database, view.reference_input,
            view.authority, program, prefix, [&] { consumer(); });
        timings = Json::array();
        backend_setup_ms = result.backend_setup_ms;
        device = device_info(result.batches.front().cuda);
        for (const auto& t : result.batches)
          timings.push_back({{"generation_after", t.generation_after}, {"commit_ms", t.commit_ms},
                             {"query_materialize_ms", t.query_materialize_ms}, {"consumer_read_ms", t.report_ms},
                             {"consumer_step_ms", t.consumer_end_to_end_ms}, {"fallback", false},
                             {"executed_backend", "cuda"}, {"executed_strategy", "full"},
                             {"explicit_kernel_launches", t.cuda.kernel_launches},
                             {"device_algorithm_calls", t.cuda.device_algorithm_calls},
                             {"levelization_decision_bytes", t.cuda.levelization_decision_bytes}, {"allocations", t.cuda.allocations},
                             {"h2d_bytes", t.cuda.h2d_bytes}, {"d2h_bytes", t.cuda.d2h_bytes}});
      } else {
        const auto result = csr::CudaTargetedTraceReplayer::run(view.session_context, view.database, view.reference_input,
            view.authority, program, prefix, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError, [&] { consumer(); });
        timings = Json::array();
        backend_setup_ms = result.backend_setup_ms;
        device = device_info(result.batches.front().device.cuda);
        for (const auto& t : result.batches)
          timings.push_back({{"generation_after", t.generation_after}, {"commit_ms", t.commit_ms},
                             {"query_materialize_ms", t.query_materialize_ms}, {"consumer_read_ms", t.report_ms},
                             {"consumer_step_ms", t.consumer_end_to_end_ms}, {"fallback", t.execution.fallback_to_full},
                             {"executed_backend", "cuda"}, {"executed_strategy", toString(t.execution.executed_update_strategy)},
                             {"explicit_kernel_launches", t.device.cuda.kernel_launches},
                             {"device_algorithm_calls", t.device.cuda.device_algorithm_calls},
                             {"levelization_decision_bytes", t.device.cuda.levelization_decision_bytes}, {"allocations", t.device.cuda.allocations},
                             {"h2d_bytes", t.device.cuda.h2d_bytes}, {"d2h_bytes", t.device.cuda.d2h_bytes}});
      }
      workload_ms = benchmarkElapsed(workload_started);
      cleanup_started = BenchmarkClock::now();
    }, backend == "cuda" ? StaBackend::kCuda : StaBackend::kCpu);
    artifacts.close();
    benchmarkRequire(consumer.generations == prefix, "consumer did not read every generation");
    result = {{"generation", prefix}, {"timing_node_count", consumer.values.size()},
              {"public", std::move(consumer.reports)}};
  }  // Session, temporary artifacts, replay and consumer storage are now released.
  const double cleanup_ms = benchmarkElapsed(cleanup_started);
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  benchmarkRequire(sched_getaffinity(0, sizeof(affinity), &affinity) == 0, "cannot read actual CPU affinity");
  Json cpus = Json::array();
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &affinity))
      cpus.push_back(cpu);
  double total{};
  for (const auto& batch : timings)
    total += batch.at("consumer_step_ms").get<double>();
  return {{"schema_version", "ista-consumer-sample/2.0.0"}, {"debug_checks_enabled", false},
          {"correctness_comparison", "not_run"}, {"diagnostics", "not_run"},
          {"build", timing_dual_csr::test::incregpustaBuildInfo()}, {"requested_backend", backend},
          {"requested_strategy", strategy}, {"threads_requested", threads}, {"cpu_affinity", cpus},
          {"thread_activity", "external_profile"}, {"device", device}, {"prefix_batches", prefix},
          {"result_requirements", {{"timing_values", "all_nodes_arrival_required_slack"}, {"qor", "complete"},
                                   {"max_paths", 20}, {"nworst", 20}, {"file_artifacts", false}}},
          {"load_initialize_ms", load_initialize_ms}, {"backend_setup_ms", backend_setup_ms}, {"workload_ms", workload_ms}, {"cleanup_ms", cleanup_ms},
          {"cold_end_to_end_ms", benchmarkElapsed(started)}, {"consumer_step_ms", total},
          {"batch_timings", std::move(timings)}, {"lifecycle_cleanup_completed", true},
          {"result", std::move(result)}};
}
}  // namespace ista::test
