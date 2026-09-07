// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrUpdater.hpp"

#include <new>
#include <stdexcept>

#include "DualCsrStateValidation.hpp"
#include "DualCsrUpdaterInternal.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::timing_dual_csr {
namespace {

void inject(DualCsrUpdateFailpoint requested,
            DualCsrUpdateFailpoint current)
{
  if (incregpusta_debug_checks_enabled && requested == current)
    throw std::bad_alloc();
}

}  // namespace

DualCsrUpdateAttempt DualCsrUpdater::stage(
    const DualCsrState& committed,
    const DualCsrBatchDescriptor& descriptor,
    DualCsrUpdateOptions options)
{
  if (!incregpusta_debug_checks_enabled && options.failpoint != DualCsrUpdateFailpoint::kNone)
    throw std::invalid_argument("fault injection requires ECC_INCREGPUSTA_DEBUG_CHECKS=ON");
  DualCsrUpdateAttempt attempt;
  attempt.violations = validateDualCsrDescriptor(committed, descriptor);
  if (!attempt.violations.empty())
    return attempt;

  DualCsrState staged = committed;
  inject(options.failpoint, DualCsrUpdateFailpoint::kAfterDetachedClone);
  detail::initializeUpdateTelemetry(staged, committed, descriptor);
  detail::growDetachedCapacity(staged, descriptor);
  inject(options.failpoint, DualCsrUpdateFailpoint::kAfterCapacityGrow);

  detail::applyLogicalDeactivations(staged, descriptor);
  detail::applyBaseChanges(staged, descriptor);
  detail::appendLogicalRecordsAndRows(staged, descriptor);
  detail::rebuildAffectedAuxRows(staged, descriptor);
  inject(options.failpoint, DualCsrUpdateFailpoint::kAfterStructuralUpdate);

  detail::applyPayloadChanges(staged, descriptor);
  detail::finalizeUpdateState(staged, descriptor);
  inject(options.failpoint, DualCsrUpdateFailpoint::kBeforeValidation);
  if constexpr (incregpusta_debug_checks_enabled)
    attempt.violations = validateDualCsrState(staged);
  if (attempt.violations.empty())
    attempt.state = std::move(staged);
  return attempt;
}

DualCsrIndexedUpdateAttempt DualCsrUpdater::stage(
    const DualCsrState& committed,
    const DualCsrArcGroupIndex& committed_index,
    const DualCsrBatchDescriptor& descriptor,
    DualCsrUpdateOptions options)
{
  DualCsrIndexedUpdateAttempt attempt;
  auto graph_attempt = stage(committed, descriptor, options);
  if (!graph_attempt.success()) {
    attempt.violations = std::move(graph_attempt.violations);
    return attempt;
  }
  auto index_attempt = stageDualCsrArcGroupIndex(
      committed, committed_index, *graph_attempt.state, descriptor);
  if (!index_attempt.success()) {
    attempt.violations = std::move(index_attempt.violations);
    return attempt;
  }
  attempt.state = std::move(graph_attempt.state);
  attempt.arc_group_index = std::move(index_attempt.index);
  return attempt;
}

}  // namespace ista::timing_dual_csr
