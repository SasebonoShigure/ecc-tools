// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <memory>
#include <thread>

#include "NativeDatabaseCandidate.hpp"
#include "TimingEditCoordinator.hpp"

namespace ista {
class Database;

namespace csr {
struct CsrCommittedSessionSnapshot;
struct StaAnalysisContext;
}  // namespace csr

namespace timing_edit {
class NativeExecutionAuthority;
}

namespace buffer_trace {

class BufferFixedTraceGenerator;
class BufferTraceProductionFreshAdapter;
class NativeBufferObservationExtractor;
class BufferReplayContext;
struct FunctionalBatchReceipt;
struct ReplayProgram;
struct ReplayReceipt;

class PendingVerifiedBatch final
{
 public:
  PendingVerifiedBatch(const PendingVerifiedBatch&) = delete;
  PendingVerifiedBatch& operator=(const PendingVerifiedBatch&) = delete;
  PendingVerifiedBatch(PendingVerifiedBatch&&) noexcept;
  PendingVerifiedBatch& operator=(PendingVerifiedBatch&&) = delete;
  ~PendingVerifiedBatch() noexcept;

 private:
  PendingVerifiedBatch(
      timing_edit::InstalledNativeDatabase&& installed,
      std::unique_ptr<csr::CsrCommittedSessionSnapshot> host_snapshot,
      timing_edit::CommittedEditIdentity post_identity,
      timing_edit::PreparedBatch&& prepared,
      timing_edit::CumulativeEditTrace cumulative,
      timing_edit::TimingEditResolverState staged_resolver) noexcept;

  timing_edit::InstalledNativeDatabase _installed;
  std::unique_ptr<csr::CsrCommittedSessionSnapshot> _host_snapshot;
  timing_edit::CommittedEditIdentity _post_identity;
  timing_edit::PreparedBatch _prepared;
  timing_edit::CumulativeEditTrace _cumulative;
  timing_edit::TimingEditResolverState _staged_resolver;

  friend class BufferTraceProductionFreshAdapter;
  friend class GuardedReplayServices;
  friend class BufferTraceReplayer;
};

// Stage 4 runtime's only private bridge to the test executable. Implementations
// must consume the opaque Stage 3 proof and publish synchronously in this call.
class GuardedReplayServices
{
 public:
  GuardedReplayServices() = default;
  virtual ~GuardedReplayServices() = default;

  GuardedReplayServices(const GuardedReplayServices&) = delete;
  GuardedReplayServices& operator=(const GuardedReplayServices&) = delete;
  GuardedReplayServices(GuardedReplayServices&&) = delete;
  GuardedReplayServices& operator=(GuardedReplayServices&&) = delete;

 private:
  [[nodiscard]] virtual Database& database() noexcept = 0;
  [[nodiscard]] virtual const csr::StaAnalysisContext& analysisContext()
      const noexcept = 0;
  [[nodiscard]] virtual const timing_edit::NativeExecutionAuthority& authority()
      const noexcept = 0;
  [[nodiscard]] virtual timing_edit::TimingEditCoordinator& coordinator()
      noexcept = 0;
  virtual void proveSealAndPublish(PendingVerifiedBatch&& pending) = 0;

  friend class BufferReplayContext;
};

// Constructed and destroyed only by the production-fresh adapter on the
// loader callback stack. Its API is private so a retained pointer cannot be
// used to reach any guarded borrow.
class BufferReplayContext final
{
 public:
  BufferReplayContext(const BufferReplayContext&) = delete;
  BufferReplayContext& operator=(const BufferReplayContext&) = delete;
  BufferReplayContext(BufferReplayContext&&) = delete;
  BufferReplayContext& operator=(BufferReplayContext&&) = delete;

 private:
  explicit BufferReplayContext(GuardedReplayServices& services) noexcept;
  ~BufferReplayContext() noexcept;

  void requireActive() const;
  [[nodiscard]] Database& database() const;
  [[nodiscard]] const csr::StaAnalysisContext& analysisContext() const;
  [[nodiscard]] const timing_edit::NativeExecutionAuthority& authority() const;
  [[nodiscard]] timing_edit::TimingEditCoordinator& coordinator() const;
  void proveSealAndPublish(PendingVerifiedBatch&& pending);

  GuardedReplayServices* _services{nullptr};
  std::thread::id _owner_thread;
  bool _active{true};

  friend class BufferFixedTraceGenerator;
  friend class BufferTraceProductionFreshAdapter;
  friend class BufferTraceReplayer;
  friend class NativeBufferObservationExtractor;
};

}  // namespace buffer_trace
}  // namespace ista
