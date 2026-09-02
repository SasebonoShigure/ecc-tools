// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>

#include "NativeCandidateExecutor.hpp"
#include "NativeDesignEditTranslator.hpp"

namespace ista::timing_dual_csr::detail {

struct CandidateLifecycleSnapshot
{
  std::uint64_t creates{0};
  std::uint64_t clones{0};
  std::uint64_t applies{0};
  std::uint64_t executes{0};
  std::uint64_t installs{0};
  std::uint64_t rollbacks{0};
};

[[nodiscard]] CandidateLifecycleSnapshot
currentCandidateLifecycleSnapshot() noexcept;

[[nodiscard]] CandidateLifecycleSnapshot candidateLifecycleDelta(
    CandidateLifecycleSnapshot before,
    CandidateLifecycleSnapshot after);

class DualCsrNativeCandidateCarrier final
{
 public:
  [[nodiscard]] static timing_edit::NativeDatabaseCandidate create(
      const Database& baseline);

  [[nodiscard]] static timing_edit::NativeDatabaseCandidate clone(
      const timing_edit::InstalledNativeDatabase& installed);

  [[nodiscard]] static timing_edit::NativeTranslationResult apply(
      timing_edit::NativeDatabaseCandidate& candidate,
      const timing_edit::PreparedBatch& prepared,
      const timing_edit::TimingEditResolverState& staged_resolver);

  [[nodiscard]] static timing_edit::InstalledNativeDatabase
  installAndExecute(
      const timing_edit::NativeExecutionAuthority& authority,
      timing_edit::NativeDatabaseCandidate candidate);

  [[nodiscard]] static bool rollback(
      timing_edit::InstalledNativeDatabase& installed) noexcept;
};

}  // namespace ista::timing_dual_csr::detail
