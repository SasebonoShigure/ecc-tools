// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrNativeCandidateCarrierInternal.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace ista::timing_dual_csr::detail {
namespace {

thread_local CandidateLifecycleSnapshot g_lifecycle;

void increment(std::uint64_t& value) noexcept
{
  if (value != std::numeric_limits<std::uint64_t>::max())
    ++value;
}

std::uint64_t delta(std::uint64_t before, std::uint64_t after)
{
  if (after < before)
    throw std::logic_error("candidate lifecycle counter moved backwards");
  return after - before;
}

}  // namespace

CandidateLifecycleSnapshot currentCandidateLifecycleSnapshot() noexcept
{
  return g_lifecycle;
}

CandidateLifecycleSnapshot candidateLifecycleDelta(
    CandidateLifecycleSnapshot before,
    CandidateLifecycleSnapshot after)
{
  return {.creates = delta(before.creates, after.creates),
          .clones = delta(before.clones, after.clones),
          .applies = delta(before.applies, after.applies),
          .executes = delta(before.executes, after.executes),
          .installs = delta(before.installs, after.installs),
          .rollbacks = delta(before.rollbacks, after.rollbacks)};
}

timing_edit::NativeDatabaseCandidate
DualCsrNativeCandidateCarrier::create(const Database& baseline)
{
  increment(g_lifecycle.creates);
  return timing_edit::NativeDatabaseCandidate::create(baseline);
}

timing_edit::NativeDatabaseCandidate
DualCsrNativeCandidateCarrier::clone(
    const timing_edit::InstalledNativeDatabase& installed)
{
  increment(g_lifecycle.clones);
  return timing_edit::NativeDatabaseCandidate::create(
      installed.database());
}

timing_edit::NativeTranslationResult
DualCsrNativeCandidateCarrier::apply(
    timing_edit::NativeDatabaseCandidate& candidate,
    const timing_edit::PreparedBatch& prepared,
    const timing_edit::TimingEditResolverState& staged_resolver)
{
  increment(g_lifecycle.applies);
  return timing_edit::NativeDesignEditTranslator::applyNormalizedBatch(
      candidate.database(), prepared, staged_resolver);
}

timing_edit::InstalledNativeDatabase
DualCsrNativeCandidateCarrier::installAndExecute(
    const timing_edit::NativeExecutionAuthority& authority,
    timing_edit::NativeDatabaseCandidate candidate)
{
  increment(g_lifecycle.installs);
  increment(g_lifecycle.executes);
  return timing_edit::NativeCandidateExecutor::installAndExecute(
      authority, std::move(candidate));
}

bool DualCsrNativeCandidateCarrier::rollback(
    timing_edit::InstalledNativeDatabase& installed) noexcept
{
  increment(g_lifecycle.rollbacks);
  return installed.rollback();
}

}  // namespace ista::timing_dual_csr::detail
