// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
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
  kUnknownFailpoint
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
  kExactBefore,
  kCapability,
  kIndexDelta,
  kUpdater,
  kStateValidation,
  kTimingIntrinsic
};

struct DualCsrStageFailure
{
  DualCsrStageFailureOrigin origin{DualCsrStageFailureOrigin::kExactBefore};
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

// Session, generation and committed identity bind a stage. Aggregate comparison is
// supplementary diagnostics, with no graph copy in the default build.
class DualCsrCommitBinding final
{
 public:
  explicit DualCsrCommitBinding(const CommittedDualCsrAnalysisState& state)
      : _session(state.graph.session_token), _generation(state.generation), _identity(state.identity)
  {
    if constexpr (incregpusta_debug_checks_enabled)
      _exact.emplace(state);
  }
  [[nodiscard]] bool matches(const CommittedDualCsrAnalysisState& state) const
  {
    if (_session != state.graph.session_token || _generation != state.generation || _identity != state.identity)
      return false;
    if constexpr (incregpusta_debug_checks_enabled)
      return *_exact == state;
    return true;
  }

 private:
  timing_edit::TimingEditSessionToken _session;
  timing_edit::TransactionGeneration _generation;
  timing_edit::CommittedEditIdentity _identity;
  std::optional<CommittedDualCsrAnalysisState> _exact;
};

class DualCsrVerifiedStage final
{
 public:
  DualCsrVerifiedStage(const DualCsrVerifiedStage&) = delete;
  DualCsrVerifiedStage& operator=(const DualCsrVerifiedStage&) = delete;
  DualCsrVerifiedStage(DualCsrVerifiedStage&&) noexcept = default;
  DualCsrVerifiedStage& operator=(DualCsrVerifiedStage&&) noexcept = default;

  [[nodiscard]] const CommittedDualCsrAnalysisState& state() const noexcept
  {
    return _state;
  }
  [[nodiscard]] const DualCsrStageTimings& timings() const noexcept
  {
    return _timings;
  }

 private:
  DualCsrVerifiedStage(const CommittedDualCsrAnalysisState& expected_committed, CommittedDualCsrAnalysisState state, DualCsrStageTimings timings)
      : _expected_committed(expected_committed), _state(std::move(state)), _timings(timings)
  {
  }

  DualCsrCommitBinding _expected_committed;
  CommittedDualCsrAnalysisState _state;
  DualCsrStageTimings _timings;

  friend DualCsrStageAttempt makeStagedDualCsrAnalysisState(
      const CommittedDualCsrAnalysisState&, VerifiedStructuredBatch&&);
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
    const CommittedDualCsrAnalysisState& committed,
    VerifiedStructuredBatch&& verified_batch);

using MixedDualCsrStageAttempt = std::variant<
    DualCsrVerifiedStage, DualCsrStageFailure,
    timing_incremental_levelization::MixedLevelizationFailure>;

[[nodiscard]] MixedDualCsrStageAttempt
makeMixedLevelizedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& exact_committed_before,
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

  DualCsrTransactionCoordinator(const DualCsrTransactionCoordinator&) = delete;
  DualCsrTransactionCoordinator& operator=(
      const DualCsrTransactionCoordinator&) = delete;
  DualCsrTransactionCoordinator(DualCsrTransactionCoordinator&&) = delete;
  DualCsrTransactionCoordinator& operator=(
      DualCsrTransactionCoordinator&&) = delete;

  [[nodiscard]] const CommittedDualCsrAnalysisState& committed() const
      noexcept
  {
    return _committed;
  }
  [[nodiscard]] bool poisoned() const noexcept { return _poisoned; }

  [[nodiscard]] DualCsrTransactionStatus publish(
      DualCsrVerifiedStage staged,
      DualCsrPublishFailpoint failpoint = DualCsrPublishFailpoint::kNone);
  [[nodiscard]] DualCsrTransactionStatus recover(
      DualCsrVerifiedModeRecovery rebuilt);

 private:
  [[nodiscard]] DualCsrTransactionStatus reject(
      DualCsrTransactionCode code) const noexcept;

  friend DualCsrModeRecoveryReplayAttempt replayDualCsrModesForRecovery(const CommittedDualCsrAnalysisState&, CommittedDualCsrAnalysisState,
                                                                        DualCsrModeRecoveryBatchSource&);

  CommittedDualCsrAnalysisState _committed;
  bool _poisoned{false};
};

}  // namespace ista::timing_dual_csr
