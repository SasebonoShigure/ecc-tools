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
#include "DualCsrStructuredBatchPreparer.hpp"

namespace ista::timing_dual_csr {
namespace {

template<typename T>
void reserveNext(std::vector<T>& records)
{
  if (records.size() == records.max_size())
    throw std::length_error("Dual-CSR history capacity exhausted");
  if (records.size() == records.capacity()) {
    const auto capacity = records.capacity();
    records.reserve(capacity > records.max_size() / 2 ? records.max_size() : std::max<std::size_t>(1, capacity * 2));
  }
}

}  // namespace

DualCsrTransactionCoordinator::DualCsrTransactionCoordinator(
    CommittedDualCsrAnalysisState initial)
    : _committed(std::make_shared<CommittedDualCsrAnalysisState>(std::move(initial)))
{
  if (!_committed->baseline)
    throw std::invalid_argument("initial Dual-CSR aggregate has no baseline");
  _committed->baseline = std::make_shared<const timing_edit::BaselineSemanticIdentity>(*_committed->baseline);
  const auto errors = validateCommittedDualCsrAnalysisState(*_committed);
  const auto binding_errors = validateDualCsrArcGroupResolverFull(
      _committed->arc_group_index, _committed->resolver);
  if (!errors.empty() || !binding_errors.empty())
    throw std::invalid_argument("invalid initial Dual-CSR aggregate");
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::reject(
    DualCsrTransactionCode code) const noexcept
{
  return {code, false, _poisoned};
}

const CommittedDualCsrAnalysisState& DualCsrTransactionCoordinator::committed() const
{
  if (!_pending.expired())
    throw std::logic_error("Dual-CSR query is unavailable during a pending transaction");
  return *_committed;
}

PendingStructuredBatch DualCsrTransactionCoordinator::prepareBatch(timing_edit::EditBatch edits) const
{
  if (_poisoned)
    throw std::logic_error("Dual-CSR prepare is unavailable for a poisoned session");
  const auto& current = committed();
  return DualCsrStructuredBatchPreparer::prepareBound(
      std::move(edits), current.baseline, current.resolver, DualSemanticReadView(current.graph),
      current.graph, current.identity, current.arc_group_index);
}

void DualCsrTransactionCoordinator::reserveHistory()
{
  reserveNext(_committed->semantic_trace);
  reserveNext(_committed->execution_trace);
  if constexpr (incregpusta_debug_checks_enabled)
    reserveNext(_committed->descriptor_trace);
}

}  // namespace ista::timing_dual_csr
