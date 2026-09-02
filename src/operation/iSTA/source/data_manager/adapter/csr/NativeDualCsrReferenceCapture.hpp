#pragma once

#include <optional>
#include <vector>

#include "AffectedCapturePlan.hpp"
#include "NarrowTimingCaptureInternal.hpp"
#include "DualSemanticReadView.hpp"

namespace ista::timing_dual_csr::detail {

// Finalizes only already captured typed records. It performs no native graph
// traversal and leaves every unrequested reference-payload field untouched.
[[nodiscard]] std::optional<NarrowCaptureError>
finalizeNativeDualCsrReferenceCapture(
    const DualSemanticReadView& committed,
    const AffectedCapturePlan& plan,
    const std::vector<std::optional<csr::NodeKey>>& captured_node_keys,
    NarrowTimingRecords& records);

}  // namespace ista::timing_dual_csr::detail
