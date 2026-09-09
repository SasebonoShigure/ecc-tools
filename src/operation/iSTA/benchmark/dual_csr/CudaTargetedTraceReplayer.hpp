#pragma once
#include <optional>
#include <string>
#include <vector>

#include "CudaDualCsrDebugState.hpp"
#include "CudaDualCsrSession.hpp"
#include "NativeCandidateExecutor.hpp"
#include "StaExecutionContract.hpp"

namespace ista::buffer_trace {
struct ReplayProgram;
}
namespace ista::csr {
struct CudaTargetedTraceObservation
{
  CsrReferenceInput canonical_input;
  CsrTimingValues values;
  std::vector<std::string> node_names;
  std::vector<std::optional<std::size_t>> arc_native_ranks;
  CudaDualCsrDebugState device;
};
struct CudaTargetedTraceBatch
{
  std::uint64_t generation_before{}, generation_after{}, node_count{}, arc_count{};
  double native_prepare_ms{}, native_install_ms{}, scatter_ms{}, end_to_end_ms{};
  CudaDualCsrTelemetry device;
  std::optional<CudaDualCsrTelemetry> rejected_mixed_attempt;
  StaExecutionRecord execution{StaBackend::kCuda,
                               StaUpdateStrategy::kFull,
                               StaFallbackPolicy::kError,
                               StaBackend::kCuda,
                               StaUpdateStrategy::kFull,
                               StaExecutionKind::kFullUpdate,
                               StaPropagationScope::kFullGraph,
                               false,
                               false,
                               std::nullopt};
  std::optional<CudaTargetedTraceObservation> diagnostic;
};
struct CudaTargetedTraceResult
{
  CudaDualCsrTelemetry initialization;
  std::vector<CudaTargetedTraceBatch> batches;
  bool native_final_rollback_completed{false};
};
// The operation guard owns the baseline for the complete prefix. Native and
// device candidates are paired here; no caller can commit arbitrary targets.
class CudaTargetedTraceReplayer final
{
 public:
  [[nodiscard]] static CudaTargetedTraceResult run(Database& baseline, const CsrReferenceInput& baseline_input,
                                                   const timing_edit::NativeExecutionAuthority& authority, const buffer_trace::ReplayProgram& program,
                                                   std::size_t prefix, StaUpdateStrategy strategy = StaUpdateStrategy::kFull,
                                                   StaFallbackPolicy fallback = StaFallbackPolicy::kError);
};
}  // namespace ista::csr
