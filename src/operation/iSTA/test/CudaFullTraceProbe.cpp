#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

#include "BufferTraceReplayInput.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CudaFullTraceReplayer.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "DualCsrFunctionalReceiptJson.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "DualCsrTargetedPhysicalProjection.hpp"
#include "DualCsrTraceProductionFreshAdapter.hpp"
#include "IncreGpuStaPerformanceJson.hpp"
#include "ProductionFreshSessionLoader.hpp"
#include "PublicTimingJson.hpp"
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
Json telemetry(const csr::CudaFullTraceBatch& batch)
{
  const auto& c = batch.cuda;
  return {{"generation_before", batch.generation_before},
          {"generation_after", batch.generation_after},
          {"node_count", batch.node_count},
          {"arc_count", batch.arc_count},
          {"prepare_ms", batch.prepare_ms},
          {"native_graph_ms", batch.native_graph_ms},
          {"export_ms", batch.export_ms},
          {"commit_ms", batch.commit_ms},
          {"query_materialize_ms", batch.query_materialize_ms},
          {"report_ms", batch.report_ms},
          {"consumer_end_to_end_ms", batch.consumer_end_to_end_ms},
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
          {"kernel_launches", c.kernel_launches},
          {"device_name", c.device_name},
          {"driver_version", c.driver_version},
          {"runtime_version", c.runtime_version}};
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
    require(argc == 8, "usage: probe cuda|fresh|performance|public (--trace FILE | --replay FILE) --prefix N --output FILE");
    const std::string mode = argv[1];
    require(mode == "cuda" || mode == "fresh" || mode == "performance" || mode == "rollback" || mode == "public", "unknown execution mode");
    require(mode == "public" || (mode == "performance" ? !incregpusta_debug_checks_enabled : incregpusta_debug_checks_enabled),
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
    Json output{{"schema_version", "ista-cuda-full-trace/3.0.0"},
                {"prefix_batches", prefix},
                {"debug_checks_enabled", incregpusta_debug_checks_enabled},
                {"requested_backend", mode == "fresh" ? "cpu" : "cuda"},
                {"executed_backend", mode == "fresh" ? "cpu" : "cuda"},
                {"requested_strategy", "full"},
                {"executed_strategy", "full"},
                {"fallback_count", 0},
                {"correctness_comparison", "not_run"},
                {"build", timing_dual_csr::test::incregpustaBuildInfo()}};
    if (mode == "fresh") {
      auto fresh = timing_dual_csr::test::runProductionFreshSemanticPrefix(program, prefix);
      output["functional"] = timing_dual_csr::test::functionalSemanticReceiptToJson(fresh.functional);
    } else {
      timing_dual_csr::test::DualCsrTargetedArtifactSupport artifacts(program);
      const auto source_before = incregpusta_debug_checks_enabled ? artifacts.capture() : std::vector<buffer_trace::SourceArtifactFunctionalRecord>{};
      std::optional<csr::CudaFullTraceResult> result;
      if (mode == "rollback") {
        require(prefix >= 2, "rollback test requires two batches");
        auto rejected_program = program;
        std::visit([](auto& workload) {
          require(workload.batches.size() >= 2 && !workload.batches[1].edits.empty(), "missing second edit");
          workload.batches[1].edits.front().library_cell.cell_name = "CUDA_MISSING_SECOND_BATCH_CELL";
        }, rejected_program.workload);
        std::vector<std::uint8_t> restored_input;
        timing_edit::oracle::ProductionFreshSessionLoader::run(artifacts.artifacts(), [&](const auto& view) {
          bool rejected = false;
          try {
            static_cast<void>(csr::replayCudaFullTracePrefix(view.session_context, view.database, view.reference_input.analysis_context,
                                                           view.authority, rejected_program, prefix));
          } catch (const std::exception& error) {
            rejected = std::string(error.what()).find("CUDA_MISSING_SECOND_BATCH_CELL") != std::string::npos;
          }
          require(rejected, "second-batch native cell failure did not occur");
          auto restored = csr::exportCsrReferenceInputCandidate(view.database, view.reference_input.analysis_context, view.session_token);
          require(std::holds_alternative<csr::CsrReferenceInput>(restored), "rollback graph export failed");
          restored_input = csr::canonicalReferenceInputBytes(std::get<csr::CsrReferenceInput>(restored));
        }, StaBackend::kCuda);
        timing_edit::oracle::ProductionFreshSessionLoader::run(artifacts.artifacts(), [&](const auto& view) {
          static_cast<void>(csr::replayCudaFullTracePrefix(view.session_context, view.database, view.reference_input.analysis_context, view.authority, program, 1));
          auto expected = csr::exportCsrReferenceInputCandidate(view.database, view.reference_input.analysis_context, view.session_token);
          require(std::holds_alternative<csr::CsrReferenceInput>(expected)
                      && restored_input == csr::canonicalReferenceInputBytes(std::get<csr::CsrReferenceInput>(expected)),
                  "failed edit changed the previously committed generation");
        }, StaBackend::kCuda);
      }
      timing_edit::oracle::ProductionFreshSessionLoader::run(
          artifacts.artifacts(),
          [&](const auto& view) {
            const auto physical_before
                = incregpusta_debug_checks_enabled ? timing_dual_csr::test::captureTargetedSourcePhysical() : buffer_trace::SourcePhysicalFunctionalReceipt{};
            result.emplace(csr::replayCudaFullTracePrefix(view.session_context, view.database, view.reference_input.analysis_context, view.authority, program, prefix));
            // Explicit cross-build consumer test; ordinary performance does not serialize public results.
            if (mode == "public")
              output["public"] = ista::test::capturePublicTiming();
            if constexpr (incregpusta_debug_checks_enabled)
              require(physical_before == timing_dual_csr::test::captureTargetedSourcePhysical(), "source physical state changed");
          },
          StaBackend::kCuda);
      require(result.has_value(), "CUDA trace did not return a result");
      if constexpr (incregpusta_debug_checks_enabled)
        require(source_before == artifacts.capture(), "source artifacts changed");
      output["lifecycle_cleanup_completed"] = true;
      output["batch_timings"] = Json::array();
      for (const auto& batch : result->batches)
        output["batch_timings"].push_back(telemetry(batch));
      if constexpr (incregpusta_debug_checks_enabled) {
        output["batches"] = Json::array();
        for (const auto& batch : result->batches)
          output["batches"].push_back(functional(batch.diagnostic_input.value(), batch.diagnostic_timing.value()));
      }
    }
    std::ofstream file(options.at("--output"));
    require(bool(file), "cannot open output");
    file << output << '\n';
    file.close();
    require(bool(file), "output write failed");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CUDA trace: " << error.what() << '\n';
    return 1;
  }
}
