// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "DualCsrModeRecoveryReplay.hpp"
#include "DualCsrNativeCandidateCarrierInternal.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "NarrowTimingRecordReader.hpp"

namespace ista::timing_dual_csr::test::recovery_support {
using ::ista::timing_dual_csr::detail::DualCsrNativeCandidateCarrier;
inline void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

class CumulativeBatchSource final
    : public DualCsrModeRecoveryBatchSource
{
 public:
  CumulativeBatchSource(
      Database& baseline,
      const timing_edit::NativeExecutionAuthority& authority)
      : _baseline(baseline), _authority(authority)
  {
  }

  [[nodiscard]] VerifiedStructuredBatch mint(
      const CommittedDualCsrAnalysisState& committed,
      timing_edit::DesignEditPlan plan)
  {
    DualSemanticReadView committed_view(committed.graph);
    PendingStructuredBatch pending
        = DualCsrStructuredBatchPreparer::prepare(
            std::move(plan), committed.resolver, committed_view,
            committed.graph, committed.identity,
            committed.arc_group_index);

    timing_edit::NativeDatabaseCandidate candidate
        = _installed ? DualCsrNativeCandidateCarrier::clone(*_installed) : DualCsrNativeCandidateCarrier::create(_baseline);
    if (_installed) {
      require(DualCsrNativeCandidateCarrier::rollback(*_installed), "cumulative source could not roll back prior candidate");
      _installed.reset();
    }
    const auto translated = DualCsrNativeCandidateCarrier::apply(candidate, pending.preparedBatch(), pending.stagedResolver());
    require(translated.applied(),
            "cumulative source native apply was rejected");
    _installed.emplace(DualCsrNativeCandidateCarrier::installAndExecute(_authority, std::move(candidate)));
    CandidateBoundCapture capture = NarrowTimingRecordReader{}.read(
        _installed->database(), committed_view,
        committed.arc_group_index, pending.stagedResolver(),
        pending.preparedBatch(), pending.capturePlan());
    if (!capture.success()) {
      const auto error = capture.error();
      throw std::runtime_error(
          "cumulative source narrow capture was rejected at mint "
          + std::to_string(_mint_count) + " with error "
          + (error ? std::to_string(static_cast<unsigned>(*error))
                   : std::string{"missing"}));
    }
    ++_mint_count;
    return TimingEditDualCsrDescriptorBuilder::build(
        std::move(pending), std::move(capture));
  }

  [[nodiscard]] DualCsrModeRecoveryBatchAttempt rebuild(
      const DualCsrModeRecoveryBatchRequest& request) override
  {
    DualCsrModeRecoveryBatchAttempt attempt;
    try {
      if (request.persisted_execution.generation().value()
              != request.batch_index + 1
          || request.persisted_semantic_batch.expected_generation.value()
                 != request.batch_index) {
        attempt.detail
            = "persisted recovery request is not generation-contiguous";
        return attempt;
      }
      VerifiedStructuredBatch verified = mint(
          request.exact_recovered_before,
          request.persisted_semantic_batch);
      if (verified.descriptor() != request.persisted_descriptor) {
        attempt.detail
            = "rebuilt descriptor differs from typed-rebound persistence";
        return attempt;
      }
      attempt.verified_batch.emplace(std::move(verified));
      return attempt;
    } catch (const std::exception& error) {
      attempt.detail = error.what();
      return attempt;
    }
  }

  [[nodiscard]] bool finish() noexcept
  {
    if (!_installed)
      return true;
    const bool rolled_back = DualCsrNativeCandidateCarrier::rollback(*_installed);
    _installed.reset();
    return rolled_back;
  }

 private:
  Database& _baseline;
  const timing_edit::NativeExecutionAuthority& _authority;
  std::optional<timing_edit::InstalledNativeDatabase> _installed;
  std::size_t _mint_count{0};
};

inline DualCsrVerifiedStage stage(
    const CommittedDualCsrAnalysisState& committed,
    VerifiedStructuredBatch verified, LevelizationExecutionMode mode)
{
  if (mode == LevelizationExecutionMode::kFullStage5) {
    auto attempt = makeStagedDualCsrAnalysisState(
        committed, std::move(verified));
    if (!attempt.success()) {
      std::string detail;
      for (const auto& violation : attempt.violations)
        detail += violation.field + ": " + violation.message + "; ";
      throw std::runtime_error("focused full stage was rejected: " + detail);
    }
    return std::move(*attempt.staged);
  }
  auto attempt = makeMixedLevelizedDualCsrAnalysisState(
      committed, std::move(verified));
  auto* staged = std::get_if<DualCsrVerifiedStage>(&attempt);
  require(staged != nullptr, "focused mixed stage was rejected");
  return std::move(*staged);
}

}  // namespace ista::timing_dual_csr::test::recovery_support

namespace ista::timing_dual_csr::test {
void realReplayRecovery(const char* replay, const char* output);
}
