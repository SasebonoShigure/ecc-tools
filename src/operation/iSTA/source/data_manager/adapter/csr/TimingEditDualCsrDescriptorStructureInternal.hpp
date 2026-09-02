// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

namespace ista::timing_dual_csr {

class PendingStructuredBatch;
struct DualCsrBatchDescriptor;

namespace detail {
struct NarrowTimingRecords;

// Completes BaseOut/AuxOut and BaseIn edits. Incoming replacement pairs are
// derived from the semantic reconnect's exact StableArcRefs, never from
// coincident vector positions or destination alone.
void buildStructuredPhysicalDelta(
    const PendingStructuredBatch& pending,
    const NarrowTimingRecords& records,
    DualCsrBatchDescriptor& descriptor);

}  // namespace detail
}  // namespace ista::timing_dual_csr
