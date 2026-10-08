// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "CommittedDualCsrAnalysisState.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "MixedLevelizationTypes.hpp"

namespace ista::timing_dual_csr {

enum class DualCsrTransactionCode : std::uint8_t
{
  kOk,
  kInvalidCommittedState,
  kInvalidStagedState,
  kStaleGeneration,
  kTraceMismatch,
  kPreSwapFault,
  kPostSwapPoisoned,
  kPoisoned,
  kRecoveryNotPoisoned,
  kUnknownFailpoint,
  kPending
};

enum class DualCsrPublishFailpoint : std::uint8_t
{
  kNone,
  kBeforeSwap,
  kAfterSwap
};

struct DualCsrTransactionStatus
{
  DualCsrTransactionCode code{DualCsrTransactionCode::kOk};
  bool published{false};
  bool poisoned{false};

  [[nodiscard]] bool ok() const noexcept
  {
    return code == DualCsrTransactionCode::kOk;
  }

  friend bool operator==(const DualCsrTransactionStatus&,
                         const DualCsrTransactionStatus&) = default;
};

enum class DualCsrStageFailureOrigin : std::uint8_t
{
  kCapability,
  kIndexDelta,
  kUpdater,
  kStateValidation,
  kTimingIntrinsic
};

struct DualCsrStageFailure
{
  DualCsrStageFailureOrigin origin{DualCsrStageFailureOrigin::kCapability};
  DualCsrViolations violations;

  friend bool operator==(const DualCsrStageFailure&,
                         const DualCsrStageFailure&) = default;
};

struct DualCsrStageTimings
{
  double dual_csr_update_ms{0.0};
  double full_levelization_ms{0.0};
  double mixed_build_ms{0.0};
  double mixed_kahn_ms{0.0};
  double reconstruct_ms{0.0};
  double full_propagation_ms{0.0};
  double invariant_ms{0.0};
  double verify_ms{0.0};
};

class DualCsrTransactionCoordinator;
class DualCsrStageFactoryAccess;
struct DualCsrStageAttempt;
class VerifiedStructuredBatch;
class PendingStructuredBatch;

// A stage belongs to one stable committed owner and generation.
class DualCsrCommitBinding final
{
 public:
  explicit DualCsrCommitBinding(const CommittedDualCsrAnalysisState& state)
      : _owner(&state), _session(state.graph.session_token), _generation(state.generation)
  {
  }
  [[nodiscard]] bool matches(const CommittedDualCsrAnalysisState& state) const noexcept
  {
    if (_owner != &state || _session != state.graph.session_token || _generation != state.generation)
      return false;
    return true;
  }

 private:
  const CommittedDualCsrAnalysisState* _owner;
  timing_edit::TimingEditSessionToken _session;
  timing_edit::TransactionGeneration _generation;
};

class DualCsrVerifiedStage final
{
 public:
  DualCsrVerifiedStage(const DualCsrVerifiedStage&) = delete;
  DualCsrVerifiedStage& operator=(const DualCsrVerifiedStage&) = delete;
  DualCsrVerifiedStage(DualCsrVerifiedStage&&) noexcept = default;
  DualCsrVerifiedStage& operator=(DualCsrVerifiedStage&&) noexcept = default;

  [[nodiscard]] const DualCsrPendingBatch& batch() const noexcept;
  [[nodiscard]] const DualCsrStageTimings& timings() const noexcept;

 private:
  struct Data;
  explicit DualCsrVerifiedStage(std::shared_ptr<Data> data) noexcept : _data(std::move(data)) {}
  std::shared_ptr<Data> _data;
  friend class DualCsrStageFactoryAccess;
  friend class DualCsrTransactionCoordinator;
};

class DualCsrVerifiedModeRecovery;
class DualCsrModeRecoveryBatchSource;
struct DualCsrModeRecoveryReplayAttempt;

struct DualCsrStageAttempt
{
  std::optional<DualCsrVerifiedStage> staged;
  DualCsrViolations violations;
  DualCsrStageTimings timings;
  std::optional<DualCsrStageFailureOrigin> failure_origin;

  [[nodiscard]] bool success() const noexcept
  {
    return staged.has_value() && violations.empty();
  }
};

[[nodiscard]] DualCsrStageAttempt makeStagedDualCsrAnalysisState(
    DualCsrTransactionCoordinator& coordinator,
    VerifiedStructuredBatch&& verified_batch);

using MixedDualCsrStageAttempt = std::variant<
    DualCsrVerifiedStage, DualCsrStageFailure,
    timing_incremental_levelization::MixedLevelizationFailure>;

[[nodiscard]] MixedDualCsrStageAttempt
makeMixedLevelizedDualCsrAnalysisState(
    DualCsrTransactionCoordinator& coordinator,
    VerifiedStructuredBatch&& verified_batch);

struct DualCsrRecoveredAggregateRebindAttempt
{
  std::optional<CommittedDualCsrAnalysisState> rebound;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return rebound.has_value() && violations.empty();
  }
};

// The only recovery boundary allowed to change a committed aggregate's
// session identity. It validates the complete old binding before changing
// every graph/timing/index/descriptor occurrence as one detached value.
[[nodiscard]] DualCsrRecoveredAggregateRebindAttempt
rebindRecoveredAggregateSession(
    CommittedDualCsrAnalysisState recovered,
    timing_edit::TimingEditSessionToken old_token,
    timing_edit::TimingEditSessionToken new_token);

class DualCsrTransactionCoordinator final
{
 public:
  explicit DualCsrTransactionCoordinator(
      CommittedDualCsrAnalysisState initial);
  explicit DualCsrTransactionCoordinator(DualCsrVerifiedModeRecovery recovered);

  DualCsrTransactionCoordinator(const DualCsrTransactionCoordinator&) = delete;
  DualCsrTransactionCoordinator& operator=(
      const DualCsrTransactionCoordinator&) = delete;
  DualCsrTransactionCoordinator(DualCsrTransactionCoordinator&&) = delete;
  DualCsrTransactionCoordinator& operator=(
      DualCsrTransactionCoordinator&&) = delete;

  // Borrow only until the next stage/publish. A pending transaction owns
  // exclusive access to graph/index and must finish before any query.
  [[nodiscard]] const CommittedDualCsrAnalysisState& committed() const;
  [[nodiscard]] PendingStructuredBatch prepareBatch(timing_edit::EditBatch edits) const;
  [[nodiscard]] bool poisoned() const noexcept { return _poisoned; }

  [[nodiscard]] bool canPublish(const DualCsrVerifiedStage& staged) const noexcept;
  [[nodiscard]] DualCsrTransactionStatus publish(
      DualCsrVerifiedStage staged,
      DualCsrPublishFailpoint failpoint = DualCsrPublishFailpoint::kNone) noexcept;
  [[nodiscard]] DualCsrTransactionStatus recover(
      DualCsrVerifiedModeRecovery rebuilt);

 private:
  [[nodiscard]] DualCsrTransactionCode publicationError(const DualCsrVerifiedStage& staged) const noexcept;
  [[nodiscard]] DualCsrTransactionStatus reject(
      DualCsrTransactionCode code) const noexcept;

  friend DualCsrModeRecoveryReplayAttempt replayDualCsrModesForRecovery(const CommittedDualCsrAnalysisState&, CommittedDualCsrAnalysisState,
                                                                        DualCsrModeRecoveryBatchSource&);

  void reserveHistory();
  std::shared_ptr<CommittedDualCsrAnalysisState> _committed;
  std::weak_ptr<DualCsrVerifiedStage::Data> _pending;
  friend class DualCsrStageFactoryAccess;
  bool _poisoned{false};
};

}  // namespace ista::timing_dual_csr
