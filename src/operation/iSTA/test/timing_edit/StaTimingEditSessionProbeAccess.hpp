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

namespace ista {

class Database;

namespace csr {
struct CsrCommittedSessionSnapshot;
struct CsrSnapshotObservation;
struct CsrTimingResult;
}  // namespace csr

namespace timing_edit {
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
  [[nodiscard]] static bool snapshotReleasedForTesting(const StaCsrSessionContext& context) noexcept;
  static void releaseSnapshotForMutation(StaCsrSessionContext& context,
                                         std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) noexcept;
  [[nodiscard]] static StaCsrCaptureAttempt captureSnapshotForTesting(
      StaCsrSessionContext& context, const Database& database);
  [[nodiscard]] static bool copyObservationForTesting(
      StaCsrSessionContext& context,
      std::unique_ptr<csr::CsrSnapshotObservation>* detached_out);
  [[nodiscard]] static StaCsrReferenceAttempt computeReferenceForTesting(
      StaCsrSessionContext& context);
  [[nodiscard]] static StaCsrScatterAttempt scatterForTesting(
      StaCsrSessionContext& context, Database& database, const csr::CsrTimingResult& result);
  static void requireCsrFullExact(csr::CsrCommittedSessionSnapshot& snapshot, const Database& database);
  static void requireNativeTimingEqual(const csr::CsrReferenceInput& input,
                                      const csr::CsrTimingResult& reference, const Database& database);

 private:
  static void poisonSessionOnScopeViolation(void* opaque_session) noexcept;
};

}  // namespace ista
