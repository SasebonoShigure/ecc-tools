#pragma once
#include <optional>
#include <string>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr::test {
// Diagnostic-only ID projection. The caller first compares the actual device
// graph/payload with canonical_input; this function performs no computation.
[[nodiscard]] CsrTimingResult projectCudaTargetedTiming(const CsrReferenceInput& canonical_input, const CsrTimingValues& stable_values,
                                                        const std::vector<std::string>& stable_node_names,
                                                        const std::vector<std::optional<std::size_t>>& stable_arc_native_ranks);
}  // namespace ista::csr::test
