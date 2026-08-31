// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferReplayContext.hpp"

#include <stdexcept>
#include <utility>

#include "CsrSessionSnapshotAdapter.hpp"
#include "NativeCandidateExecutor.hpp"

namespace ista::buffer_trace {

PendingVerifiedBatch::PendingVerifiedBatch(
    timing_edit::InstalledNativeDatabase&& installed,
    std::unique_ptr<csr::CsrCommittedSessionSnapshot> host_snapshot,
    timing_edit::CommittedEditIdentity post_identity,
    timing_edit::PreparedBatch&& prepared,
    timing_edit::CumulativeEditTrace cumulative,
    timing_edit::TimingEditResolverState staged_resolver) noexcept
    : _installed(std::move(installed)),
      _host_snapshot(std::move(host_snapshot)),
      _post_identity(std::move(post_identity)),
      _prepared(std::move(prepared)),
      _cumulative(std::move(cumulative)),
      _staged_resolver(std::move(staged_resolver))
{
}

PendingVerifiedBatch::PendingVerifiedBatch(PendingVerifiedBatch&&) noexcept = default;
PendingVerifiedBatch::~PendingVerifiedBatch() noexcept = default;

BufferReplayContext::BufferReplayContext(GuardedReplayServices& services) noexcept
    : _services(&services), _owner_thread(std::this_thread::get_id())
{
}

BufferReplayContext::~BufferReplayContext() noexcept
{
  _active = false;
  _services = nullptr;
}

void BufferReplayContext::requireActive() const
{
  if (!_active || _services == nullptr) {
    throw std::logic_error("buffer replay context is outside its guarded callback");
  }
  if (_owner_thread != std::this_thread::get_id()) {
    throw std::logic_error("buffer replay context was used from a different thread");
  }
  const auto& authority = _services->authority();
  const auto& coordinator = _services->coordinator();
  if (!authority.active() || authority.sessionToken().value() == 0
      || coordinator.sessionToken() != authority.sessionToken()) {
    throw std::logic_error("buffer replay context lost its operation-guard authority");
  }
}

Database& BufferReplayContext::database() const
{
  requireActive();
  return _services->database();
}

const csr::StaAnalysisContext& BufferReplayContext::analysisContext() const
{
  requireActive();
  return _services->analysisContext();
}

const timing_edit::NativeExecutionAuthority& BufferReplayContext::authority() const
{
  requireActive();
  return _services->authority();
}

timing_edit::TimingEditCoordinator& BufferReplayContext::coordinator() const
{
  requireActive();
  return _services->coordinator();
}

void BufferReplayContext::proveSealAndPublish(PendingVerifiedBatch&& pending)
{
  requireActive();
  _services->proveSealAndPublish(std::move(pending));
  requireActive();
}

}  // namespace ista::buffer_trace
