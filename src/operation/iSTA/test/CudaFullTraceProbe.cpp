#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

#include "BufferTraceJson.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CudaFullTraceReplayer.hpp"
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
  require(baseline.success(), "diagnostic canonical projection failed");
  timing_dual_csr::DualCsrTimingResult values{timing, std::vector<std::uint8_t>(input.arcs.size(), 1U)};
  auto projected = timing_dual_csr::projectDualCsrFunctionalSemanticReceipt(*baseline.state, values);
  require(projected.success(), "diagnostic timing projection failed");
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
          {"scatter_ms", batch.scatter_ms},
          {"end_to_end_ms", batch.end_to_end_ms},
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
    require(argc == 8, "usage: probe cuda|fresh|performance --trace FILE --prefix N --output FILE");
    const std::string mode = argv[1];
    require(mode == "cuda" || mode == "fresh" || mode == "performance" || mode == "rollback", "unknown execution mode");
    require(mode == "performance" ? !incregpusta_debug_checks_enabled : incregpusta_debug_checks_enabled,
            "performance requires checks OFF; diagnostics require checks ON");
    std::map<std::string, std::string> options;
    for (int i = 2; i < argc; i += 2)
      require(options.emplace(argv[i], argv[i + 1]).second, "duplicate option");
    require(options.contains("--trace") && options.contains("--prefix") && options.contains("--output"), "unknown or missing option");
    std::size_t prefix = 0;
    const auto& text = options.at("--prefix");
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), prefix);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && prefix > 0, "invalid prefix");
    std::ifstream trace(options.at("--trace"));
    require(bool(trace), "cannot open trace");
    auto decoded = buffer_trace::parseBufferTraceArtifactJson(std::string(std::istreambuf_iterator<char>(trace), {}));
    require(std::holds_alternative<buffer_trace::BufferTraceArtifact>(decoded), "invalid frozen trace");
    const auto& program = std::get<buffer_trace::BufferTraceArtifact>(decoded).replay;
    Json output{{"schema_version", "ista-cuda-full-trace/1.0.0"},
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
      timing_edit::oracle::ProductionFreshSessionLoader::run(
          artifacts.artifacts(),
          [&](const auto& view) {
            const auto physical_before
                = incregpusta_debug_checks_enabled ? timing_dual_csr::test::captureTargetedSourcePhysical() : buffer_trace::SourcePhysicalFunctionalReceipt{};
            if (mode == "rollback") {
              require(prefix >= 2, "rollback test requires two batches");
              auto rejected_program = program;
              std::visit(
                  [](auto& workload) {
                    require(workload.batches.size() >= 2 && !workload.batches[1].edits.empty(), "missing second edit");
                    workload.batches[1].edits.front().library_cell.cell_name = "CUDA_MISSING_SECOND_BATCH_CELL";
                  },
                  rejected_program.workload);
              bool rejected = false;
              try {
                static_cast<void>(
                    csr::replayCudaFullTracePrefix(view.database, view.reference_input.analysis_context, view.authority, rejected_program, prefix));
              } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("CUDA_MISSING_SECOND_BATCH_CELL") != std::string::npos;
              } catch (const std::invalid_argument& error) {
                rejected = std::string(error.what()).find("CUDA_MISSING_SECOND_BATCH_CELL") != std::string::npos;
              }
              require(rejected, "second-batch native cell failure did not occur");
              auto restored = csr::exportCsrReferenceInputCandidate(view.database, view.reference_input.analysis_context, view.session_token);
              require(std::holds_alternative<csr::CsrReferenceInput>(restored)
                          && std::get<csr::CsrReferenceInput>(restored).source_identity == view.reference_input.source_identity
                          && csr::canonicalReferenceInputBytes(std::get<csr::CsrReferenceInput>(restored))
                                 == csr::canonicalReferenceInputBytes(view.reference_input),
                      "failed prefix did not restore exact native baseline input");
            }
            result.emplace(csr::replayCudaFullTracePrefix(view.database, view.reference_input.analysis_context, view.authority, program, prefix));
            if constexpr (incregpusta_debug_checks_enabled)
              require(physical_before == timing_dual_csr::test::captureTargetedSourcePhysical(), "source physical state changed");
          },
          StaBackend::kCuda);
      require(result && result->native_final_rollback_completed, "CUDA trace did not finish rollback");
      if constexpr (incregpusta_debug_checks_enabled)
        require(source_before == artifacts.capture(), "source artifacts changed");
      output["native_final_rollback_completed"] = result->native_final_rollback_completed;
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
    file << output.dump(2) << '\n';
    require(bool(file), "output write failed");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CUDA trace: " << error.what() << '\n';
    return 1;
  }
}
