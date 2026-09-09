// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDesignEditTranslator.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

#include "Database.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "NativeDatabaseCandidate.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_edit {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("native buffer insertion: " + std::move(message));
}

NativeTranslationResult preflightPolicy(const PreparedBatch& batch)
{
  if (batch.policyReceipt().disposition == EditDisposition::kFullWithRecord) {
    return {EditDisposition::kFullWithRecord, batch.policyReceipt().reasons};
  }
  for (const auto& edit : batch.normalizedEdits()) {
    if (const auto* parasitic = std::get_if<ParasiticRcInputs>(&edit.rc_inputs)) {
      if (parasitic->parasitic_policy == ParasiticEditPolicy::kFullWithRecord) {
        return {EditDisposition::kFullWithRecord, {"source net parasitics require a fresh full rebuild"}};
      }
      invalid("source net parasitics are unsupported by native buffer insertion");
    }
  }
  return {};
}

}  // namespace

NativeTranslationResult NativeDesignEditTranslator::applyNormalizedBatch(Database& database, const PreparedBatch& batch,
                                                                          const TimingEditResolverState& staged_resolver)
{
  NativeTranslationResult policy = preflightPolicy(batch);
  if (!policy.applied()) {
    return policy;
  }

  Database staged(database);
  NativeDatabaseCandidate::resetDerivedState(staged);
  std::uint64_t instance_id = detail::nextNativeInstanceId(staged);
  for (const auto& edit : batch.normalizedEdits()) {
    detail::applyNativeBufferInsertion(staged, edit, staged_resolver, instance_id);
    if (instance_id == std::numeric_limits<std::uint64_t>::max() && &edit != &batch.normalizedEdits().back()) {
      invalid("native instance ID capacity is exhausted within the batch");
    }
    ++instance_id;
  }
  NativeDatabaseCandidate::resetDerivedState(staged);
  database = std::move(staged);
  return policy;
}

}  // namespace ista::timing_edit
