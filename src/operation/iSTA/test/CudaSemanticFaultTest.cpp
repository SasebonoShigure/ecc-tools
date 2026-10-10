// Test-only sticky context failure; each mode runs in an independent process.
#include <filesystem>
#include <string>
#include "BufferTraceReplayInput.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CudaDualCsrTestReader.hpp"
#include "CudaFullTraceReplayer.hpp"
#include "CudaTargetedTraceReplayer.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "ProductionFreshSessionLoader.hpp"
#include "IncrementalStaSession.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::csr::test {
void checkCudaSemanticFault(const std::filesystem::path& path, const std::string& mode)
{
  const auto require = [](bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
  };
  require(mode == "full" || mode == "targeted" || mode == "mixed", "unknown semantic fault mode");
  const auto program = buffer_trace::test::readReplayInput(path, false);
  timing_dual_csr::test::DualCsrTargetedArtifactSupport artifacts(program);
  bool verified = false;
  try {
    timing_edit::oracle::ProductionFreshSessionLoader::run(artifacts.artifacts(), [&](const auto& view) {
      auto& state = view.session_context.cudaState(view.authority);
      const auto before = canonicalReferenceInputBytes(view.reference_input);
      CudaDualCsrTestReader::corruptContext();
      bool failed = false;
      try {
        if (mode == "full")
          static_cast<void>(CudaFullTraceReplayer::run(
              view.session_context, view.database, view.reference_input, view.authority, program, 1));
        else
          static_cast<void>(CudaTargetedTraceReplayer::run(
              view.session_context, view.database, view.reference_input, view.authority, program, 1,
              mode == "mixed" ? StaUpdateStrategy::kIncremental : StaUpdateStrategy::kFull));
      } catch (const std::exception&) {
        failed = true;
      }
      // Do not query first: a query must not repair a missing replayer poison.
      require(failed && state.device.poisoned() && view.session.state() == StaSessionState::kPoisoned,
              "device failure did not immediately poison the outer semantic session");
      require(state.device.generation().value() == 0 && state.host
                  && state.host->graph.generation.value() == 0 && state.history.empty(),
              "failed semantic device stage committed host state or history");
      auto restored = exportCsrReferenceInputCandidate(
          view.database, view.reference_input.analysis_context, view.session_token);
      const auto* input = std::get_if<CsrReferenceInput>(&restored);
      require(input && canonicalReferenceInputBytes(*input) == before,
              "device failure changed the unmaterialized native view");
      verified = true;
    }, StaBackend::kCuda);
  } catch (const StaSessionException& error) {
    require(verified && error.status().code == StaSessionStatusCode::kCleanupFailed,
            "unexpected loader failure during semantic device fault");
  }
  require(verified, "semantic device failure checks were not reached");
}
}  // namespace ista::csr::test
