#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "CudaTraceObservation.hpp"
#include "CudaDualCsrSession.hpp"
#include "NativeCandidateExecutor.hpp"
#include "StaExecutionContract.hpp"

namespace ista { class StaCsrSessionContext; }
namespace ista::buffer_trace {
struct ReplayProgram;
}
namespace ista::csr {
struct CudaTargetedTraceBatch
{
  std::uint64_t generation_before{}, generation_after{}, node_count{}, arc_slots{};
  bool results_consumed{false};
  double direct_prepare_ms{}, commit_ms{}, query_materialize_ms{}, report_ms{}, consumer_end_to_end_ms{};
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
  std::optional<CudaTraceObservation> diagnostic;
};
struct CudaTargetedTraceResult
{
  double backend_setup_ms{}, edit_sta_loop_ms{};
  CudaDualCsrTelemetry initialization;
  std::vector<CudaTargetedTraceBatch> batches;
};
// The operation guard owns the CSR/device transaction for the complete prefix.
class CudaTargetedTraceReplayer final
{
 public:
  [[nodiscard]] static CudaTargetedTraceResult run(StaCsrSessionContext& session, Database& baseline, const CsrReferenceInput& baseline_input,
                                                   const timing_edit::NativeExecutionAuthority& authority, const buffer_trace::ReplayProgram& program,
                                                   std::size_t prefix, StaUpdateStrategy strategy = StaUpdateStrategy::kFull,
                                                   StaFallbackPolicy fallback = StaFallbackPolicy::kError,
                                                   const std::function<void()>& consume_results = {});
};
}  // namespace ista::csr
