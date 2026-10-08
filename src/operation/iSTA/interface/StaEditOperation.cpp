// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "StaEditOperation.hpp"
#include "StaCsrSessionContext.hpp"
#include <utility>

namespace ista {
STAInterface::OperationGuard StaEditOperation::acquire(IncrementalStaSession& session, bool api_acquired)
{
  const auto state = session.state();
  if (!api_acquired || state != StaSessionState::kBaselineExecuted || !session._token.load() || !session._context)
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kInvalidState, state,
        "timing edit requires a non-reentrant baseline-executed incremental session"), session.request());
  StaStatus failure;
  auto guard = STAInterface::getInst().acquireUse(StaOwner::session(session._token.load()),
      session._context.get(), true, state, &failure);
  if (!guard) throw StaSessionException(std::move(failure), session.request());
  return std::move(*guard);
}
StaEditOperation::StaEditOperation(IncrementalStaSession& session)
    : _api_guard(&session, session._api_mutex), _session(session),
      _operation_guard(acquire(session, static_cast<bool>(_api_guard))),
      _authority(timing_edit::TimingEditSessionToken{session._token.load()}, {&session, &StaEditOperation::poison})
{
}
StaCsrSessionContext& StaEditOperation::context() const noexcept { return *_session._context; }
void StaEditOperation::poison(void* opaque) noexcept
{
  static_cast<IncrementalStaSession*>(opaque)->_state.store(StaSessionState::kPoisoned, std::memory_order_release);
}
}  // namespace ista
