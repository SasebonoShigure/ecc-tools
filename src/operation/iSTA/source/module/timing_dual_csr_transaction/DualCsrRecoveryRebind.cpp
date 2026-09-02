// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTransactionCoordinator.hpp"

#include <string>
#include <utility>

namespace ista::timing_dual_csr {
namespace {

void reject(DualCsrViolations& violations, std::string field,
            std::string message)
{
  violations.push_back({DualCsrViolationCode::kConflict,
                        std::move(field), std::move(message)});
}

bool descriptorUsesToken(const DualCsrBatchDescriptor& descriptor,
                         timing_edit::TimingEditSessionToken token)
{
  if (descriptor.session_token != token)
    return false;
  const auto& source = descriptor.reference_payload.source_identity;
  return !source
         || (source->expected.session_token == token.value()
             && source->replacement.session_token == token.value());
}

bool fullyBoundTo(const CommittedDualCsrAnalysisState& state,
                  timing_edit::TimingEditSessionToken token)
{
  if (state.graph.session_token != token
      || state.graph.reference_payload.source_identity.session_token
             != token.value()
      || state.timing.values.source_identity.session_token
             != token.value()
      || state.arc_group_index.session_token != token)
    return false;
  for (const auto& descriptor : state.descriptor_trace)
    if (!descriptorUsesToken(descriptor, token))
      return false;
  return true;
}

void rebindDescriptor(DualCsrBatchDescriptor& descriptor,
                      timing_edit::TimingEditSessionToken token)
{
  descriptor.session_token = token;
  auto& source = descriptor.reference_payload.source_identity;
  if (!source)
    return;
  source->expected.session_token = token.value();
  source->replacement.session_token = token.value();
}

}  // namespace

DualCsrRecoveredAggregateRebindAttempt
rebindRecoveredAggregateSession(
    CommittedDualCsrAnalysisState recovered,
    timing_edit::TimingEditSessionToken old_token,
    timing_edit::TimingEditSessionToken new_token)
{
  DualCsrRecoveredAggregateRebindAttempt attempt;
  attempt.violations = validateCommittedDualCsrAnalysisState(recovered);
  if (!attempt.violations.empty())
    return attempt;
  if (old_token == new_token || new_token.value() == 0) {
    reject(attempt.violations, "recovery.session_token",
           "recovery must change to a nonzero session token");
    return attempt;
  }
  if (!fullyBoundTo(recovered, old_token)) {
    reject(attempt.violations, "recovery.old_session_token",
           "aggregate is not completely bound to the declared old token");
    return attempt;
  }

  recovered.graph.session_token = new_token;
  recovered.graph.reference_payload.source_identity.session_token
      = new_token.value();
  recovered.timing.values.source_identity.session_token
      = new_token.value();
  recovered.arc_group_index.session_token = new_token;
  for (auto& descriptor : recovered.descriptor_trace)
    rebindDescriptor(descriptor, new_token);

  attempt.violations = validateCommittedDualCsrAnalysisState(recovered);
  if (!attempt.violations.empty())
    return attempt;
  if (!fullyBoundTo(recovered, new_token)) {
    reject(attempt.violations, "recovery.new_session_token",
           "aggregate was only partially rebound to the new token");
    return attempt;
  }
  attempt.rebound = std::move(recovered);
  return attempt;
}

}  // namespace ista::timing_dual_csr
