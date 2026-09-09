#include "IncreGpuStaBuildConfig.hpp"
#include "STAInterface.hpp"
#include "StaCsrSessionContext.hpp"

#if ISTA_ENABLE_CUDA
#include <stdexcept>
#include <variant>

#include "CsrCudaBackend.hpp"
#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "GraphBuilder.hpp"
#include "Logger.hpp"
#include "PowerAnalyzer.hpp"
#include "PowerPropagator.hpp"
#include "PowerReporter.hpp"
#include "SDFWriter.hpp"
#include "StaProductionLifecycle.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingReporter.hpp"
#include "TimingResultScatterInternal.hpp"
#include "TimingSemanticKey.hpp"
#endif

namespace ista {

StaStatus STAInterface::runSessionCudaBaseline(std::uint64_t token, StaCsrSessionContext* context)
{
  StaStatus failure;
  auto authority = acquireUse(StaOwner::session(token), context, true, StaSessionState::kInitialized, &failure);
  if (!authority)
    return failure;
  if (!_is_initialized)
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kCreated, "init_sta must be called before CUDA STA");
#if ISTA_ENABLE_CUDA
  try {
    csr::CsrCudaBackend::requireDevice();
    bool unsupported_input = false;
    const auto compute = [&] {
      auto built = csr::exportCsrReferenceInputCandidate(STADM.getDatabase(), context->analysisContext(), token);
      if (const auto* error = std::get_if<csr::CsrReferenceInputExportFailure>(&built))
        throw std::runtime_error("CUDA CSR capture failed: " + std::string(csr::token(error->code)));
      auto input = std::move(std::get<csr::CsrReferenceInput>(built));
      const auto output = csr::CsrCudaBackend{}.computeWithTelemetry(input);
      const auto* timing = std::get_if<csr::CsrTimingResult>(&output.computation.outcome);
      if (timing == nullptr) {
        if (const auto* support = std::get_if<csr::CsrReferenceSupportResult>(&output.computation.outcome)) {
          unsupported_input = true;
          std::string message = "CUDA STA does not support analysis features:";
          for (auto reason : support->unsupported_reasons)
            message += " " + std::string(csr::token(reason));
          throw std::runtime_error(message);
        }
        throw std::runtime_error("CUDA STA computation failed: " + std::string(csr::token(std::get<csr::CsrComputationCode>(output.computation.outcome))));
      }
      if (const auto code = csr::detail::publishComputedTimingResult(STADM.getDatabase(), input, *timing, token); code != csr::CsrScatterCode::kSuccess)
        throw std::runtime_error("CUDA timing scatter failed: " + std::string(csr::token(code)));
      STALOG.info(Loc::current(), "CUDA full H2D bytes=", output.telemetry.h2d_bytes, " D2H bytes=", output.telemetry.d2h_bytes,
                  " kernels=", output.telemetry.kernel_launches, " synchronized_ms=", output.telemetry.end_to_end_ms);
    };
    const auto result = executeStaLifecyclePipeline(
        {{"GraphBuilder", [] { GraphBuilder::initInst(); }, [] { STAGB.buildStructure(); }, [] { GraphBuilder::destroyInst(); }},
         {"CUDA full timing", [] {}, compute, [] {}},
         {"PowerPropagator", [] { PowerPropagator::initInst(); }, [] { STAPP.propagate(); }, [] { PowerPropagator::destroyInst(); }},
         {"TimingAnalyzer", [] { TimingAnalyzer::initInst(); }, [] { STATA.analyzePropagatedTiming(); }, [] { TimingAnalyzer::destroyInst(); }},
         {"PowerAnalyzer", [] { PowerAnalyzer::initInst(); }, [] { STAPA.analyze(); }, [] { PowerAnalyzer::destroyInst(); }},
         {"TimingReporter", [] { TimingReporter::initInst(); }, [] { STATR.report(); }, [] { TimingReporter::destroyInst(); }},
         {"PowerReporter", [] { PowerReporter::initInst(); }, [] { STAPR.report(); }, [] { PowerReporter::destroyInst(); }},
         {"SDFWriter", [] { SDFWriter::initInst(); }, [] { STASW.write(); }, [] { SDFWriter::destroyInst(); }}},
        StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
    if (!result.succeeded) {
      if (result.cleanup_failed) {
        std::scoped_lock lifecycle_lock(_lifecycle_mutex);
        _lifecycle.quarantineCurrent();
        return StaStatus::failure(StaSessionStatusCode::kCleanupFailed, StaSessionState::kPoisoned, result.message);
      }
      if (unsupported_input)
        return StaStatus::failure(StaSessionStatusCode::kUnsupported, StaSessionState::kPoisoned, result.message,
                                  StaCapabilityReason::kUnsupportedAnalysisFeatures);
      return StaStatus::failure(StaSessionStatusCode::kRunFailed, StaSessionState::kPoisoned, result.message);
    }
    _is_timing_updated = true;
    return StaStatus::success(StaSessionState::kBaselineExecuted);
  } catch (const std::exception& error) {
    return StaStatus::failure(StaSessionStatusCode::kRunFailed, StaSessionState::kPoisoned, error.what());
  }
#else
  return StaStatus::failure(StaSessionStatusCode::kUnsupported, StaSessionState::kInitialized, "CUDA STA backend was not built",
                            StaCapabilityReason::kBackendUnavailable);
#endif
}

}  // namespace ista
