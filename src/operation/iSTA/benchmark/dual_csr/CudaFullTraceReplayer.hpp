#pragma once

#include <optional>
#include <vector>

#include "CsrCudaBackend.hpp"
#include "NativeCandidateExecutor.hpp"
#include "TimingEditTypes.hpp"

namespace ista { class StaCsrSessionContext; }
namespace ista::buffer_trace {
struct ReplayProgram;
}
namespace ista::csr {
struct CudaFullTraceBatch
{
  std::uint64_t generation_before{}, generation_after{}, node_count{}, arc_count{};
  double prepare_ms{}, native_graph_ms{}, export_ms{}, commit_ms{}, query_materialize_ms{}, report_ms{}, consumer_end_to_end_ms{};
  CsrCudaTelemetry cuda;
  std::optional<CsrReferenceInput> diagnostic_input;
  std::optional<CsrTimingResult> diagnostic_timing;
};
struct CudaFullTraceResult
{
  std::vector<CudaFullTraceBatch> batches;
};
// The caller's existing operation guard owns the authority and baseline.
// Each generation rebuilds and uploads the complete graph, reusing session models
// and capacity. No CPU levelization/propagation or CPU Dual-CSR updater is called.
[[nodiscard]] CudaFullTraceResult replayCudaFullTracePrefix(StaCsrSessionContext& session, Database& baseline, const StaAnalysisContext& analysis,
                                                            const timing_edit::NativeExecutionAuthority& authority, const buffer_trace::ReplayProgram& program,
                                                            std::size_t prefix);
}  // namespace ista::csr
