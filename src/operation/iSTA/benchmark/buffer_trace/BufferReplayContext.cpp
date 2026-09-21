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
#include "NativeCandidateExecutor.hpp"

namespace ista::buffer_trace {
BufferReplayContext::BufferReplayContext(
    Database& database, const csr::StaAnalysisContext& analysis,
    const timing_edit::NativeExecutionAuthority& authority,
    timing_edit::BaselineSemanticIdentity baseline,
    timing_edit::CommittedEditIdentity identity,
    timing_edit::TimingEditResolverState resolver,
    void (*compare_timing)(const csr::CsrReferenceInput&, const csr::CsrTimingResult&, const Database&))
    : _database(database), _analysis(analysis), _authority(authority),
      _baseline(std::make_shared<const timing_edit::BaselineSemanticIdentity>(std::move(baseline))),
      _identity(std::move(identity)), _resolver(std::move(resolver)),
      _compare_timing(compare_timing), _owner_thread(std::this_thread::get_id())
{
}
BufferReplayContext::~BufferReplayContext() noexcept
{
  _active = false;
}
void BufferReplayContext::requireActive() const
{
  if (!_active || _owner_thread != std::this_thread::get_id())
    throw std::logic_error("buffer replay context is outside its guarded callback/thread");
  if (!_authority.active() || _authority.sessionToken().value() == 0)
    throw std::logic_error("buffer replay context lost its operation-guard authority");
}
Database& BufferReplayContext::database() const
{
  requireActive();
  return _database;
}
const csr::StaAnalysisContext& BufferReplayContext::analysisContext() const
{
  requireActive();
  return _analysis;
}
const timing_edit::NativeExecutionAuthority& BufferReplayContext::authority() const
{
  requireActive();
  return _authority;
}
}  // namespace ista::buffer_trace
