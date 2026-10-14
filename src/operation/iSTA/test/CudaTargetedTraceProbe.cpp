#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

#include "BufferTraceReplayInput.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CudaTargetedDeviceComparison.hpp"
#include "CudaTargetedTimingProjection.hpp"
#include "CudaTargetedTraceReplayer.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "DualCsrFunctionalReceiptJson.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "DualCsrTargetedPhysicalProjection.hpp"
#include "DualCsrTraceProductionFreshAdapter.hpp"
#include "IncreGpuStaPerformanceJson.hpp"
#include "ProductionFreshSessionLoader.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace {
using namespace ista;
using Json = nlohmann::json;
void require(bool value, const char* message)
{
  if (!value)
    throw std::runtime_error(message);
}
Json functional(const csr::CsrReferenceInput& input, const csr::CsrTimingResult& timing)
{
  auto baseline = timing_dual_csr::DualCsrBaselineFactory::create(input, timing_edit::TimingEditSessionToken{input.source_identity.session_token},
                                                                  timing_edit::AppendOnlyHighWater{input.nodes.size(), input.nodes.size(), input.arcs.size()});
  if (!baseline.success()) {
    std::string message = "diagnostic canonical projection failed";
    for (const auto& error : baseline.violations)
      message += "\n" + error.field + ": " + error.message;
    throw std::runtime_error(message);
  }
  timing_dual_csr::DualCsrTimingResult values{timing};
  auto projected = timing_dual_csr::projectDualCsrFunctionalSemanticReceipt(*baseline.state, values);
  if (!projected.success()) {
    std::string message = "diagnostic timing projection failed";
    for (const auto& error : projected.violations)
      message += "\n" + error.field + ": " + error.message;
    throw std::runtime_error(message);
  }
  return timing_dual_csr::test::functionalSemanticReceiptToJson(*projected.receipt);
}
Json deviceTelemetry(const csr::CudaDualCsrTelemetry& device)
{
  const auto& c = device.cuda;
  return {{"update_ms", device.update_ms},
          {"grown_buffers", device.grown_buffers},
          {"mixed_build_ms", device.mixed.mixed_build_ms},
          {"mixed_kahn_ms", device.mixed.mixed_kahn_ms},
          {"mixed_prepare_ms", device.mixed_prepare_ms},
          {"levelization_total_ms", device.mixed.mixed_build_ms + device.mixed.mixed_kahn_ms + device.mixed_prepare_ms + c.levelization_ms},
          {"encode_ms", c.encode_ms},
          {"h2d_ms", c.h2d_ms},
          {"levelization_ms", c.levelization_ms},
          {"propagation_ms", c.propagation_ms},
          {"d2h_ms", c.d2h_ms},
          {"cuda_end_to_end_ms", c.end_to_end_ms},
          {"h2d_bytes", c.h2d_bytes},
          {"d2h_bytes", c.d2h_bytes},
          {"d2d_bytes", c.d2d_bytes},
          {"allocations", c.allocations},
          {"explicit_kernel_launches", c.kernel_launches},
          {"device_algorithm_calls", c.device_algorithm_calls},
          {"levelization_decision_bytes", c.levelization_decision_bytes},
          {"device_name", c.device_name},
          {"driver_version", c.driver_version},
          {"runtime_version", c.runtime_version}};
}
Json telemetry(const csr::CudaTargetedTraceBatch& batch)
{
  auto result = deviceTelemetry(batch.device);
  const auto& record = batch.execution;
  result.update({{"generation_before", batch.generation_before},
                 {"generation_after", batch.generation_after},
                 {"node_count", batch.node_count},
                 {"arc_slots", batch.arc_slots},
                 {"direct_prepare_ms", batch.direct_prepare_ms},
                 {"commit_ms", batch.commit_ms},
                 {"result_consumption", batch.results_consumed ? "completed" : "not_run"},
                 {"query_materialize_ms", batch.results_consumed ? Json(batch.query_materialize_ms) : Json(nullptr)},
                 {"report_ms", batch.results_consumed ? Json(batch.report_ms) : Json(nullptr)},
                 {"consumer_end_to_end_ms", batch.consumer_end_to_end_ms},
                 {"execution",
                  {{"requested_backend", toString(record.requested_backend)},
                   {"requested_update_strategy", toString(record.requested_update_strategy)},
                   {"fallback_policy", toString(record.fallback_policy)},
                   {"executed_backend", toString(record.executed_backend)},
                   {"executed_update_strategy", toString(record.executed_update_strategy)},
                   {"execution_kind", toString(record.execution_kind)},
                   {"propagation_scope", toString(record.propagation_scope)},
                   {"incremental_batch_executed", record.incremental_batch_executed},
                   {"fallback_to_full", record.fallback_to_full},
                   {"fallback_reason", record.fallback_reason ? Json(toString(*record.fallback_reason)) : Json(nullptr)}}}});
  if (batch.rejected_mixed_attempt)
    result["rejected_mixed_attempt"] = deviceTelemetry(*batch.rejected_mixed_attempt);
  return result;
}
}  // namespace
int main(int argc, char** argv)
{
  try {
    using namespace ista;
    if (argc == 2 && std::string(argv[1]) == "build-info") {
      std::cout << timing_dual_csr::test::incregpustaBuildInfo().dump() << '\n';
      return 0;
    }
    require(argc == 8,
            "usage: probe targeted|performance|rollback|incremental|incremental-performance|incremental-rollback (--trace FILE | --replay FILE) --prefix N --output FILE");
    const std::string mode = argv[1];
    const bool incremental = mode == "incremental" || mode == "incremental-performance" || mode == "incremental-rollback";
    const bool performance = mode == "performance" || mode == "incremental-performance";
    const bool rollback = mode == "rollback" || mode == "incremental-rollback";
    require(incremental || mode == "targeted" || performance || rollback, "unknown execution mode");
    const auto strategy = incremental ? StaUpdateStrategy::kIncremental : StaUpdateStrategy::kFull;
    require(rollback || (performance ? !incregpusta_debug_checks_enabled : incregpusta_debug_checks_enabled),
            "performance requires checks OFF; diagnostics require checks ON");
    std::map<std::string, std::string> options;
    for (int i = 2; i < argc; i += 2)
      require(options.emplace(argv[i], argv[i + 1]).second, "duplicate option");
    require((options.contains("--trace") != options.contains("--replay")) && options.contains("--prefix") && options.contains("--output"),
            "expected exactly one --trace/--replay plus --prefix and --output");
    std::size_t prefix = 0;
    const auto& text = options.at("--prefix");
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), prefix);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && prefix > 0, "invalid prefix");
    const bool replay_only = options.contains("--replay");
    const auto program = buffer_trace::test::readReplayInput(options.at(replay_only ? "--replay" : "--trace"), replay_only);
    Json output{{"schema_version", incremental ? "ista-cuda-incremental-trace/7.0.0" : "ista-cuda-targeted-trace/7.0.0"},
                {"prefix_batches", prefix},
                {"debug_checks_enabled", incregpusta_debug_checks_enabled},
                {"requested_backend", "cuda"},
                {"executed_backend", "cuda"},
                {"requested_strategy", incremental ? "incremental" : "targeted-full-levelization"},
                {"executed_strategy", incremental ? "incremental" : "targeted-full-levelization"},
                {"fallback_count", 0},
                {"correctness_comparison", "not_run"},
                {"build", timing_dual_csr::test::incregpustaBuildInfo()}};
    {
      timing_dual_csr::test::DualCsrTargetedArtifactSupport artifacts(program);
      const auto source_before = incregpusta_debug_checks_enabled ? artifacts.capture() : std::vector<buffer_trace::SourceArtifactFunctionalRecord>{};
      std::optional<csr::CudaTargetedTraceResult> result;
      if (rollback) {
        require(prefix >= 2, "rollback test requires two batches");
        auto rejected_program = program;
        std::visit([](auto& workload) {
          require(workload.batches.size() >= 2 && !workload.batches[1].edits.empty(), "missing second edit");
          workload.batches[1].edits.front().library_cell.cell_name = "CUDA_MISSING_SECOND_BATCH_CELL";
        }, rejected_program.workload);
        std::vector<std::uint8_t> after_failure;
        bool rejected = false;
        timing_edit::oracle::ProductionFreshSessionLoader::run(
            artifacts.artifacts(), [&](const auto& view) {
              try {
                static_cast<void>(csr::CudaTargetedTraceReplayer::run(view.session_context, view.database, view.reference_input, view.authority,
                                                                    rejected_program, prefix, strategy));
              } catch (const std::exception& error) {
                rejected = std::string(error.what()).find("CUDA_MISSING_SECOND_BATCH_CELL") != std::string::npos;
              }
              require(rejected, "second-batch native cell failure did not occur");
              if constexpr (incregpusta_debug_checks_enabled) {
                view.session_context.materializeCudaTiming(view.database, view.authority);
                auto exported = csr::exportCsrReferenceInputCandidate(view.database, view.reference_input.analysis_context, view.session_token);
                require(std::holds_alternative<csr::CsrReferenceInput>(exported), "post-failure graph export failed");
                after_failure = csr::canonicalReferenceInputBytes(std::get<csr::CsrReferenceInput>(exported));
              }
            }, StaBackend::kCuda);
        if constexpr (incregpusta_debug_checks_enabled) {
          timing_edit::oracle::ProductionFreshSessionLoader::run(
              artifacts.artifacts(), [&](const auto& view) {
                static_cast<void>(csr::CudaTargetedTraceReplayer::run(view.session_context, view.database, view.reference_input, view.authority, program, 1, strategy));
                view.session_context.materializeCudaTiming(view.database, view.authority);
                auto exported = csr::exportCsrReferenceInputCandidate(view.database, view.reference_input.analysis_context, view.session_token);
                require(std::holds_alternative<csr::CsrReferenceInput>(exported), "committed prefix graph export failed");
                require(after_failure == csr::canonicalReferenceInputBytes(std::get<csr::CsrReferenceInput>(exported)),
                        "failed second batch changed the first committed batch");
              }, StaBackend::kCuda);
        }
        output["second_batch_failure_observed"] = rejected;
      }
      timing_edit::oracle::ProductionFreshSessionLoader::run(
          artifacts.artifacts(),
          [&](const auto& view) {
            const auto physical_before
                = incregpusta_debug_checks_enabled ? timing_dual_csr::test::captureTargetedSourcePhysical() : buffer_trace::SourcePhysicalFunctionalReceipt{};
            result.emplace(csr::CudaTargetedTraceReplayer::run(view.session_context, view.database, view.reference_input, view.authority, program, prefix, strategy));
            if constexpr (incregpusta_debug_checks_enabled)
              require(physical_before == timing_dual_csr::test::captureTargetedSourcePhysical(), "source physical state changed");
          },
          StaBackend::kCuda);
      require(result.has_value(), "CUDA trace did not complete its workload");
      if constexpr (incregpusta_debug_checks_enabled)
        require(source_before == artifacts.capture(), "source artifacts changed");
      // run() returned only after closing and destroying the fresh session.
      output["lifecycle_cleanup_completed"] = true;
      const auto& initialization = result->initialization.cuda;
      output["initialization"] = {{"scope", "persistent_device_session_only"},
                                  {"h2d_bytes", initialization.h2d_bytes},
                                  {"d2h_bytes", initialization.d2h_bytes},
                                  {"d2d_bytes", initialization.d2d_bytes},
                                  {"allocations", initialization.allocations},
                                  {"explicit_kernel_launches", initialization.kernel_launches},
          {"device_algorithm_calls", initialization.device_algorithm_calls},
          {"levelization_decision_bytes", initialization.levelization_decision_bytes},
                                  {"end_to_end_ms", initialization.end_to_end_ms}};
      output["batch_timings"] = Json::array();
      for (const auto& batch : result->batches) {
        output["batch_timings"].push_back(telemetry(batch));
        if (batch.execution.fallback_to_full)
          output["fallback_count"] = output["fallback_count"].get<unsigned>() + 1;
      }
      if constexpr (incregpusta_debug_checks_enabled) {
        output["batches"] = Json::array();
        output["device_checks"] = Json::array();
        for (const auto& batch : result->batches) {
          const auto& observation = batch.diagnostic.value();
          csr::test::requireCudaDeviceMatchesCanonical(observation);
          const auto values
              = csr::test::projectCudaTargetedTiming(observation.canonical_input, observation.values, observation.node_names, observation.arc_native_ranks);
          output["batches"].push_back(functional(observation.canonical_input, values));
          output["device_checks"].push_back({{"graph_equivalent", true}, {"storage_visits", observation.device.storage_visits}});
        }
      }
    }
    std::ofstream file(options.at("--output"));
    require(bool(file), "cannot open output");
    file << output << '\n';
    file.close();
    require(bool(file), "output write failed");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CUDA targeted trace: " << error.what() << '\n';
    return 1;
  }
}
