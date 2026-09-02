// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>

#include "BufferTraceProductionFreshAdapter.hpp"
#include "DualCsrFunctionalProjection.hpp"

namespace ista::buffer_trace {
struct ReplayProgram;
}

namespace ista::timing_dual_csr::test {

struct ProductionFreshSemanticPrefix
{
  FunctionalSemanticReceipt functional;
  buffer_trace::SourceIsolationFunctionalReceipt source_isolation;
  std::uint64_t loader_invocation_count{0};
  std::uint64_t guarded_work_invocation_count{0};
};

[[nodiscard]] ProductionFreshSemanticPrefix runProductionFreshSemanticPrefix(
    const buffer_trace::ReplayProgram& program, std::size_t prefix_batches);

}  // namespace ista::timing_dual_csr::test
