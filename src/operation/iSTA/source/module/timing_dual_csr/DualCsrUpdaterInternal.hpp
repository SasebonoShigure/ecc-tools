// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "DualCsrUpdater.hpp"

namespace ista::timing_dual_csr::detail {

void initializeUpdateTelemetry(DualCsrState& staged,
                               const DualCsrState& committed,
                               const DualCsrBatchDescriptor& descriptor);
void growDetachedCapacity(DualCsrState& staged,
                          const DualCsrBatchDescriptor& descriptor);
void applyLogicalDeactivations(DualCsrState& staged,
                               const DualCsrBatchDescriptor& descriptor);
void applyBaseChanges(DualCsrState& staged,
                      const DualCsrBatchDescriptor& descriptor);
void appendLogicalRecordsAndRows(DualCsrState& staged,
                                 const DualCsrBatchDescriptor& descriptor);
void rebuildAffectedAuxRows(DualCsrState& staged,
                            const DualCsrBatchDescriptor& descriptor);
void applyPayloadChanges(DualCsrState& staged,
                         const DualCsrBatchDescriptor& descriptor);
void finalizeUpdateState(DualCsrState& staged,
                         const DualCsrBatchDescriptor& descriptor);

}  // namespace ista::timing_dual_csr::detail
