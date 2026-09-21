#include "CanonicalTimingRecordProjectorInternal.hpp"
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "DualCsrModeRecoveryReplay.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "NarrowTimingRecordReader.hpp"

namespace ista::timing_dual_csr::test::recovery_support {
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
      const timing_edit::NativeExecutionAuthority& authority)
      : _authority(authority)
  {
  }

  [[nodiscard]] VerifiedStructuredBatch mint(
      DualCsrTransactionCoordinator& coordinator,
      timing_edit::EditBatch plan)
  {
    const auto& committed = coordinator.committed();
    commitPending(committed.generation);
    DualSemanticReadView committed_view(committed.graph);
    PendingStructuredBatch pending
        = coordinator.prepareBatch(std::move(plan));

    _installed.emplace(timing_edit::NativeEditTransaction::prepareFull(
        _authority, pending.preparedBatch(), pending.stagedResolver()));
    _pending_generation = pending.preparedBatch().targetGeneration();
    timing_edit::NativeCandidateExecutor::executeFull(*_installed);

    csr::detail::NativeLibraryModels models(_installed->database());
    CandidateBoundCapture capture = NarrowTimingRecordReader{models}.read(
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
          request.coordinator,
          request.persisted_semantic_batch);
      if (request.diagnostic_descriptor && verified.descriptor() != *request.diagnostic_descriptor) {
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

  // Recovery's coordinator is local to replay; retain only the pending
  // generation, then confirm publication through the next call or its result.
  void commitPending(timing_edit::TransactionGeneration generation)
  {
    if (!_installed)
      return;
    require(_installed->active() && generation == _pending_generation,
            "native batch has no matching committed CSR generation");
    _installed->commit();
    _installed.reset();
  }

  [[nodiscard]] bool rollback() noexcept
  {
    if (!_installed)
      return true;
    const bool restored = _installed->rollback();
    _installed.reset();
    return restored;
  }

 private:
  const timing_edit::NativeExecutionAuthority& _authority;
  std::optional<timing_edit::NativeEditTransaction> _installed;
  timing_edit::TransactionGeneration _pending_generation;
  std::size_t _mint_count{0};
};

inline DualCsrVerifiedStage stage(
    DualCsrTransactionCoordinator& coordinator,
    VerifiedStructuredBatch verified, LevelizationExecutionMode mode)
{
  if (mode == LevelizationExecutionMode::kFullStage5) {
    auto attempt = makeStagedDualCsrAnalysisState(
        coordinator, std::move(verified));
    if (!attempt.success()) {
      std::string detail;
      for (const auto& violation : attempt.violations)
        detail += violation.field + ": " + violation.message + "; ";
      throw std::runtime_error("focused full stage was rejected: " + detail);
    }
    return std::move(*attempt.staged);
  }
  auto attempt = makeMixedLevelizedDualCsrAnalysisState(
      coordinator, std::move(verified));
  auto* staged = std::get_if<DualCsrVerifiedStage>(&attempt);
  require(staged != nullptr, "focused mixed stage was rejected");
  return std::move(*staged);
}

}  // namespace ista::timing_dual_csr::test::recovery_support

namespace ista::timing_dual_csr::test {
void realReplayRecovery(const char* replay, const char* output);
void realReplayConsumer(const char* replay, const char* output, const char* mode, const char* prefix);
}
