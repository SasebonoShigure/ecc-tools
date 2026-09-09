#pragma once

#include <optional>
#include <vector>

#include "CsrCudaBackend.hpp"
#include "NativeCandidateExecutor.hpp"
#include "TimingEditTypes.hpp"

namespace ista::buffer_trace {
struct ReplayProgram;
}
namespace ista::csr {
struct CudaFullTraceBatch
{
  std::uint64_t generation_before{}, generation_after{}, node_count{}, arc_count{};
  double prepare_ms{}, native_graph_ms{}, export_ms{}, scatter_ms{}, end_to_end_ms{};
  CsrCudaTelemetry cuda;
  std::optional<CsrReferenceInput> diagnostic_input;
  std::optional<CsrTimingResult> diagnostic_timing;
};
struct CudaFullTraceResult
{
  std::vector<CudaFullTraceBatch> batches;
  bool native_final_rollback_completed{false};
};
// The caller's existing operation guard owns the authority and baseline.
// Each generation installs a complete canonical candidate and uses new device
// storage. No CPU levelization/propagation or CPU Dual-CSR updater is called.
[[nodiscard]] CudaFullTraceResult replayCudaFullTracePrefix(Database& baseline, const StaAnalysisContext& analysis,
                                                            const timing_edit::NativeExecutionAuthority& authority, const buffer_trace::ReplayProgram& program,
                                                            std::size_t prefix);
}  // namespace ista::csr
