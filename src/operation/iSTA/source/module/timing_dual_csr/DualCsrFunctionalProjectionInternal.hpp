// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <vector>

#include "DualCsrFunctionalProjection.hpp"

namespace ista::timing_dual_csr::detail {

struct CanonicalSemanticIndex
{
  std::vector<std::size_t> node_ids;
  std::vector<std::size_t> active_arc_ids;
};

[[nodiscard]] CanonicalSemanticIndex makeCanonicalSemanticIndex(
    const DualCsrState& state);

[[nodiscard]] const csr::NodeKey& semanticNodeKey(
    const DualCsrState& state, csr::NodeId id);

[[nodiscard]] const csr::ArcKey& semanticArcKey(
    const DualCsrState& state, csr::ArcId id);

[[nodiscard]] DualCsrFunctionalProjection projectValidatedDualCsrState(
    const DualCsrState& state, const CanonicalSemanticIndex& index);

[[nodiscard]] DualCsrTimingSemanticProjection
projectValidatedDualCsrTimingResult(
    const DualCsrState& state, const DualCsrTimingResult& timing,
    const CanonicalSemanticIndex& index);

}  // namespace ista::timing_dual_csr::detail
