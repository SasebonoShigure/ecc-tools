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

#include "CommittedDualCsrAnalysisState.hpp"

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
class DualCsrTransactionCoordinator;
struct DualCsrStageAttempt;
class VerifiedStructuredBatch;

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

 private:
  DualCsrVerifiedStage(CommittedDualCsrAnalysisState expected_committed,
                       CommittedDualCsrAnalysisState state)
      : _expected_committed(std::move(expected_committed)),
        _state(std::move(state))
  {
  }

  CommittedDualCsrAnalysisState _expected_committed;
  CommittedDualCsrAnalysisState _state;

  friend DualCsrStageAttempt makeStagedDualCsrAnalysisState(
      const CommittedDualCsrAnalysisState&, VerifiedStructuredBatch&&);
  friend class DualCsrTransactionCoordinator;
};

struct DualCsrRecoveryAttempt;
class DualCsrVerifiedRecovery final
{
 public:
  DualCsrVerifiedRecovery(const DualCsrVerifiedRecovery&) = delete;
  DualCsrVerifiedRecovery& operator=(const DualCsrVerifiedRecovery&) = delete;
  DualCsrVerifiedRecovery(DualCsrVerifiedRecovery&&) noexcept = default;
  DualCsrVerifiedRecovery& operator=(DualCsrVerifiedRecovery&&) noexcept
      = default;

  [[nodiscard]] const CommittedDualCsrAnalysisState& state() const noexcept
  {
    return _state;
  }

 private:
  DualCsrVerifiedRecovery(CommittedDualCsrAnalysisState expected_poisoned,
                          CommittedDualCsrAnalysisState state)
      : _expected_poisoned(std::move(expected_poisoned)),
        _state(std::move(state))
  {
  }

  CommittedDualCsrAnalysisState _expected_poisoned;
  CommittedDualCsrAnalysisState _state;

  friend DualCsrRecoveryAttempt makeDualCsrVerifiedRecovery(
      const CommittedDualCsrAnalysisState&,
      CommittedDualCsrAnalysisState);
  friend class DualCsrTransactionCoordinator;
};

struct DualCsrRecoveryAttempt
{
  std::optional<DualCsrVerifiedRecovery> recovery;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return recovery.has_value() && violations.empty();
  }
};

struct DualCsrStageTimings
{
  double dual_csr_update_ms{0.0};
  double full_levelization_ms{0.0};
  double full_propagation_ms{0.0};
  double invariant_ms{0.0};
  double verify_ms{0.0};
};

struct DualCsrStageAttempt
{
  std::optional<DualCsrVerifiedStage> staged;
  DualCsrViolations violations;
  DualCsrStageTimings timings;

  [[nodiscard]] bool success() const noexcept
  {
    return staged.has_value() && violations.empty();
  }
};

[[nodiscard]] DualCsrStageAttempt makeStagedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& committed,
    VerifiedStructuredBatch&& verified_batch);

[[nodiscard]] DualCsrRecoveryAttempt makeDualCsrVerifiedRecovery(
    const CommittedDualCsrAnalysisState& poisoned_committed,
    CommittedDualCsrAnalysisState rebuilt);

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
      DualCsrVerifiedRecovery rebuilt);

 private:
  [[nodiscard]] DualCsrTransactionStatus reject(
      DualCsrTransactionCode code) const noexcept;

  CommittedDualCsrAnalysisState _committed;
  bool _poisoned{false};
};

}  // namespace ista::timing_dual_csr
