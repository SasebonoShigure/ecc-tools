// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "AffectedCapturePlan.hpp"

namespace ista::timing_dual_csr::detail {

struct NativeArcBijection
{
  // Deactivation requests remain null. Every active request has exactly one
  // candidate index, and no candidate is assigned twice.
  std::vector<std::optional<std::size_t>> candidate_by_request;
  std::uint64_t identity_tie_breaks{0};
};

using NativeArcCompatibility
    = std::function<bool(std::size_t request_index,
                         std::size_t candidate_index)>;
using NativeArcSemanticEquivalence
    = std::function<bool(std::size_t lhs_candidate,
                         std::size_t rhs_candidate)>;

// Candidate indices and keys are in the candidate graph's local native
// traversal order. Keys must already have been assigned once by
// csr::assignParallelOrdinals. This routine never predicts or rewrites an
// ordinal.
[[nodiscard]] std::variant<NativeArcBijection, NarrowCaptureError>
matchNativeAffectedArcs(
    std::span<const AffectedArcCapture> requests,
    std::span<const csr::ArcKey> candidate_keys,
    std::span<const csr::ArcId> request_ids,
    const NativeArcCompatibility& compatible,
    const NativeArcSemanticEquivalence& semantically_equivalent);

}  // namespace ista::timing_dual_csr::detail
