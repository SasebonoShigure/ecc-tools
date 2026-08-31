// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>

#include "IncrementalStaSession.hpp"
#include "StaCsrSessionContext.hpp"
#include "TimingEditTypes.hpp"
#include "TimingEditTransactionTypes.hpp"

namespace ista {

class Database;

namespace csr {
struct CsrCommittedSessionSnapshot;
struct CsrSnapshotObservation;
struct CsrTimingResult;
}  // namespace csr

namespace timing_edit {
class CsrFullExactReceipt;
class NativeExecutionAuthority;
}  // namespace timing_edit

class CsrFullExactMismatch final : public std::runtime_error
{
 public:
  CsrFullExactMismatch()
      : std::runtime_error("CSR CPU/full differs from native timing readback field-by-field")
  {
  }
};

// Test-only bridge. Production headers only forward-declare and friend this
// class; no timing-edit batch API is added to STAInterface in Stage 3.
class StaTimingEditSessionProbeAccess final
{
 public:
  using GuardedMutation = std::function<StaStatus(std::uint64_t, StaCsrSessionContext&, const timing_edit::NativeExecutionAuthority&)>;

  [[nodiscard]] static StaStatus runWithSingleOperationGuard(IncrementalStaSession& session, const GuardedMutation& mutation);
  static void releaseSnapshotForMutation(StaCsrSessionContext& context,
                                         std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) noexcept;
  [[nodiscard]] static timing_edit::SessionStateSignal stateSignal(IncrementalStaSession& session) noexcept;
  [[nodiscard]] static StaCsrCaptureAttempt captureSnapshotForTesting(
      StaCsrSessionContext& context, const Database& database);
  [[nodiscard]] static bool copyObservationForTesting(
      StaCsrSessionContext& context,
      std::unique_ptr<csr::CsrSnapshotObservation>* detached_out);
  [[nodiscard]] static StaCsrReferenceAttempt computeReferenceForTesting(
      StaCsrSessionContext& context);
  [[nodiscard]] static StaCsrScatterAttempt scatterForTesting(
      StaCsrSessionContext& context, Database& database, const csr::CsrTimingResult& result);
  [[nodiscard]] static timing_edit::CsrFullExactReceipt proveCsrFullExact(
      const timing_edit::NativeExecutionAuthority& authority,
      csr::CsrCommittedSessionSnapshot& snapshot, const Database& database,
      timing_edit::TransactionGeneration target_generation);

 private:
  static void writeSessionState(void* opaque_session, timing_edit::SessionTransactionState state) noexcept;
  static void poisonSessionOnScopeViolation(void* opaque_session) noexcept;
};

}  // namespace ista
