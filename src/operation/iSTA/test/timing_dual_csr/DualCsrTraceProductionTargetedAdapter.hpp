// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <cstdint>

#include "BufferTraceProductionFreshAdapter.hpp"
#include "DualCsrReplayTypes.hpp"

namespace ista::buffer_trace {
struct ReplayProgram;
}

namespace ista::timing_dual_csr::test {

struct ProductionTargetedSemanticPrefix
{
  DualCsrTraceRunResult replay;
  buffer_trace::SourceIsolationFunctionalReceipt source_isolation;
  std::uint64_t loader_invocation_count{0};
  std::uint64_t guarded_work_invocation_count{0};
};

[[nodiscard]] ProductionTargetedSemanticPrefix
runProductionTargetedSemanticPrefix(
    const buffer_trace::ReplayProgram& program,
    std::size_t prefix_batches);

[[nodiscard]] buffer_trace::OneShotProcessState
targetedOneShotState() noexcept;
[[nodiscard]] std::uint64_t targetedLoaderInvocationCount() noexcept;
[[nodiscard]] std::uint64_t targetedGuardedWorkInvocationCount() noexcept;

}  // namespace ista::timing_dual_csr::test
