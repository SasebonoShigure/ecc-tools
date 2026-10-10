#pragma once
#include <functional>

#include <optional>
#include <vector>

#include "CsrCudaBackend.hpp"
#include "CudaTraceObservation.hpp"
#include "NativeCandidateExecutor.hpp"
#include "TimingEditTypes.hpp"

namespace ista { class StaCsrSessionContext; }
namespace ista::buffer_trace {
struct ReplayProgram;
}
namespace ista::csr {
struct CudaFullTraceBatch
{
  std::uint64_t generation_before{}, generation_after{}, node_count{}, arc_slots{};
  bool results_consumed{false};
  double direct_prepare_ms{}, host_apply_ms{}, commit_ms{}, query_materialize_ms{}, report_ms{}, consumer_end_to_end_ms{};
  CsrCudaTelemetry cuda;
  std::optional<CudaTraceObservation> diagnostic;
};
struct CudaFullTraceResult
{
  double backend_setup_ms{};
  std::vector<CudaFullTraceBatch> batches;
};
// The caller's existing operation guard owns the authority and baseline.
// Each generation rebuilds and uploads the complete graph, reusing session models
// and capacity. Host CSR owns edits; no CPU timing propagation is called.
class CudaFullTraceReplayer final
{
 public:
  [[nodiscard]] static CudaFullTraceResult run(StaCsrSessionContext& session, Database& baseline, const CsrReferenceInput& baseline_input,
                                                            const timing_edit::NativeExecutionAuthority& authority, const buffer_trace::ReplayProgram& program,
                                                            std::size_t prefix, const std::function<void()>& consume_results = {});
};
}  // namespace ista::csr
