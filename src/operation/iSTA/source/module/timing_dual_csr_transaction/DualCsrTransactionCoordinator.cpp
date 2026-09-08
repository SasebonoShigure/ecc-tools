// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTransactionCoordinator.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "DualCsrArcGroupResolverValidation.hpp"

namespace ista::timing_dual_csr {
namespace {

bool prefixMatches(const CommittedDualCsrAnalysisState& committed,
                   const CommittedDualCsrAnalysisState& staged)
{
  if (staged.descriptor_trace.size() != committed.descriptor_trace.size() + 1
      || staged.semantic_trace.size() != committed.semantic_trace.size() + 1
      || staged.execution_trace.size()
             != committed.execution_trace.size() + 1)
    return false;
  return std::equal(committed.descriptor_trace.begin(),
                    committed.descriptor_trace.end(),
                    staged.descriptor_trace.begin())
         && std::equal(committed.semantic_trace.begin(),
                       committed.semantic_trace.end(),
                       staged.semantic_trace.begin())
         && std::equal(committed.execution_trace.begin(),
                       committed.execution_trace.end(),
                       staged.execution_trace.begin());
}

}  // namespace

DualCsrTransactionCoordinator::DualCsrTransactionCoordinator(
    CommittedDualCsrAnalysisState initial)
    : _committed(std::move(initial))
{
  const auto errors = validateCommittedDualCsrAnalysisState(_committed);
  const auto binding_errors = validateDualCsrArcGroupResolverFull(
      _committed.arc_group_index, _committed.resolver);
  if (!errors.empty() || !binding_errors.empty())
    throw std::invalid_argument("invalid initial Dual-CSR aggregate");
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::reject(
    DualCsrTransactionCode code) const noexcept
{
  return {code, false, _poisoned};
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::publish(
    DualCsrVerifiedStage verified,
    DualCsrPublishFailpoint failpoint)
{
  if (_poisoned)
    return reject(DualCsrTransactionCode::kPoisoned);
  if (failpoint != DualCsrPublishFailpoint::kNone
      && (!incregpusta_debug_checks_enabled || (failpoint != DualCsrPublishFailpoint::kBeforeSwap && failpoint != DualCsrPublishFailpoint::kAfterSwap)))
    return reject(DualCsrTransactionCode::kUnknownFailpoint);
  if (incregpusta_debug_checks_enabled && !validateCommittedDualCsrAnalysisState(_committed).empty()) {
    _poisoned = true;
    return reject(DualCsrTransactionCode::kInvalidCommittedState);
  }
  if (!(verified._expected_committed.matches(_committed)))
    return reject(DualCsrTransactionCode::kTraceMismatch);

  CommittedDualCsrAnalysisState staged = std::move(verified._state);
  if (staged.graph.session_token != _committed.graph.session_token
      || staged.generation.value() != _committed.generation.value() + 1)
    return reject(DualCsrTransactionCode::kStaleGeneration);
  if (incregpusta_debug_checks_enabled && !prefixMatches(_committed, staged))
    return reject(DualCsrTransactionCode::kTraceMismatch);
  if (failpoint == DualCsrPublishFailpoint::kBeforeSwap)
    return reject(DualCsrTransactionCode::kPreSwapFault);

  _committed.swap(staged);
  if (failpoint == DualCsrPublishFailpoint::kAfterSwap) {
    _poisoned = true;
    return {DualCsrTransactionCode::kPostSwapPoisoned, true, true};
  }
  return {DualCsrTransactionCode::kOk, true, false};
}

}  // namespace ista::timing_dual_csr
