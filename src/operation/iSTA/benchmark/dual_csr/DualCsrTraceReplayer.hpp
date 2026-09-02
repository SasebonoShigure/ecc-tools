// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>

#include "DualCsrReplayTypes.hpp"

namespace ista::buffer_trace {
struct ReplayProgram;
}

namespace ista::timing_dual_csr {

class DualCsrTraceReplayer final
{
 public:
  [[nodiscard]] static DualCsrTraceRunResult replayPrefix(
      const DualCsrTraceContext& context,
      const buffer_trace::ReplayProgram& program,
      std::size_t prefix_batches);
};

[[nodiscard]] DualCsrTraceRunResult replayDualCsrTracePrefix(
    const DualCsrTraceContext& context,
    const buffer_trace::ReplayProgram& program,
    std::size_t prefix_batches);

}  // namespace ista::timing_dual_csr
