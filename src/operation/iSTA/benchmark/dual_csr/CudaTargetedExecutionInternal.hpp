#pragma once

#include "CudaTargetedTraceReplayer.hpp"

namespace ista::csr::detail {

// Shared production policy boundary; native installation/publication remains
// in the replayer. The returned detached candidate still requires owner publish.
[[nodiscard]] CudaDualCsrSession stageCudaTargetedBatch(CudaDualCsrSession& owner, const CudaDualCsrPatch& patch, StaUpdateStrategy strategy,
                                                        StaFallbackPolicy fallback, CudaTargetedTraceBatch& batch);

}  // namespace ista::csr::detail
