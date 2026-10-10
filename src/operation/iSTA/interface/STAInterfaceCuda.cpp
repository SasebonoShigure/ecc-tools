#include "IncreGpuStaBuildConfig.hpp"
#include "STAInterface.hpp"
#include "StaCsrSessionContext.hpp"

#if ISTA_ENABLE_CUDA
#include <stdexcept>
#include <variant>

#include "CsrCudaBackend.hpp"
#include "StaCudaSessionState.hpp"
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
  return runOwnedCudaBaseline(token, context, true, nullptr);
}

StaStatus STAInterface::runOwnedCudaBaseline(std::uint64_t token, StaCsrSessionContext* context, bool include_artifacts,
                                            csr::CsrReferenceInput* baseline_input)
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
      auto built = csr::exportCsrReferenceInputCandidate(STADM.getDatabase(), context->analysisContext(), token, context->libraryModels(STADM.getDatabase()));
      if (const auto* error = std::get_if<csr::CsrReferenceInputExportFailure>(&built))
        throw std::runtime_error("CUDA CSR capture failed: " + std::string(csr::token(error->code)));
      auto input = std::move(std::get<csr::CsrReferenceInput>(built));
      if (!input.unsupported_reasons.empty()) {
        unsupported_input = true;
        std::string message = "CUDA STA does not support analysis features:";
        for (auto reason : input.unsupported_reasons)
          message += " " + std::string(csr::token(reason));
        throw std::runtime_error(message);
      }
      context->_cuda.reset(new StaCudaSessionState(STADM.getDatabase(), input));
      if (baseline_input)
        *baseline_input = std::move(input);
      const auto& telemetry = context->_cuda->device.telemetry().cuda;
      STALOG.info(Loc::current(), "CUDA full H2D bytes=", telemetry.h2d_bytes, " D2H bytes=", telemetry.d2h_bytes,
                  " explicit_kernels=", telemetry.kernel_launches, " device_algorithm_calls=", telemetry.device_algorithm_calls, " synchronized_ms=", telemetry.end_to_end_ms);
    };
    std::vector<StaLifecycleStage> stages{
        {"GraphBuilder", [] { GraphBuilder::initInst(); }, [] { STAGB.buildStructure(); }, [] { GraphBuilder::destroyInst(); }},
        {"CUDA full timing", [] {}, compute, [] {}},
        {"CUDA timing consumer", [] {}, [&] { context->_cuda->materializeBaseline(STADM.getDatabase()); }, [] {}},
    };
    if (include_artifacts)
      stages.push_back({"PowerPropagator", [] { PowerPropagator::initInst(); }, [] { STAPP.propagate(); }, [] { PowerPropagator::destroyInst(); }});
    stages.push_back({"CUDA report preparation", [] {}, [&] { context->_cuda->prepareReport(); }, [] {}});
    if (include_artifacts) {
      stages.push_back({"PowerAnalyzer", [] { PowerAnalyzer::initInst(); }, [] { STAPA.analyze(); }, [] { PowerAnalyzer::destroyInst(); }});
      stages.push_back({"TimingReporter", [] { TimingReporter::initInst(); }, [] { STATR.report(); }, [] { TimingReporter::destroyInst(); }});
      stages.push_back({"PowerReporter", [] { PowerReporter::initInst(); }, [] { STAPR.report(); }, [] { PowerReporter::destroyInst(); }});
      stages.push_back({"SDFWriter", [] { SDFWriter::initInst(); }, [] { STASW.write(); }, [] { SDFWriter::destroyInst(); }});
    }
    const auto result = executeStaLifecyclePipeline(
        stages, StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
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
  static_cast<void>(include_artifacts);
  static_cast<void>(baseline_input);
  return StaStatus::failure(StaSessionStatusCode::kUnsupported, StaSessionState::kInitialized, "CUDA STA backend was not built",
                            StaCapabilityReason::kBackendUnavailable);
#endif
}

}  // namespace ista
