// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <string>

#include "BufferTraceTypes.hpp"

namespace ista::buffer_trace::detail {

[[nodiscard]] BufferTraceContractViolations validateReplayBatches(
    const ReplayHeader& header, const std::vector<timing_edit::EditBatch>& batches);

// Cross-field replay rules live outside canonical encoding so the public
// parser cannot bypass profile/workload limits while the encoder stays small.
void appendReplayProfileViolations(
    const ReplayHeader& header, const std::vector<timing_edit::EditBatch>& batches,
    BufferTraceContractViolations& errors);

// Decision and physical numeric evidence must describe a state the greedy
// selector can actually emit. Arithmetic reproducibility remains at the
// artifact validator call site next to the canonical receipt calculation.
void appendEvaluationContractViolations(
    const CandidateEvaluationReceipt& evaluation,
    const SelectionProfile& profile, const std::string& field,
    BufferTraceContractViolations& errors);

}  // namespace ista::buffer_trace::detail
