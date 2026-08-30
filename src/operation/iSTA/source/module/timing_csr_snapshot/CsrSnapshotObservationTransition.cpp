// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "CsrSnapshotObservationTransition.hpp"

#include <cmath>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "CsrSnapshotInvalidationPolicyInternal.hpp"

namespace ista::csr {
namespace {

using Violations = std::vector<CsrContractViolation>;

Violations valuelessActionViolation()
{
  return {{"action", "action variant is valueless"}};
}

void append(Violations& errors, Violations additions)
{
  errors.insert(errors.end(), std::make_move_iterator(additions.begin()), std::make_move_iterator(additions.end()));
}

bool validDuration(double value)
{
  return std::isfinite(value) && value >= 0.0;
}

Violations validateActionPayload(const CsrObservationAction& action)
{
  Violations errors;
  const auto rejectDuration = [&](double duration) {
    if (!validDuration(duration))
      errors.push_back({"action.duration_ms", "must be finite and nonnegative"});
  };
  if (const auto* value = std::get_if<ReferenceMeasurementAction>(&action))
    rejectDuration(value->duration_ms);
  else if (const auto* value = std::get_if<ScatterRejectionAction>(&action))
    rejectDuration(value->duration_ms);
  else if (const auto* value = std::get_if<ScatterSuccessAction>(&action))
    rejectDuration(value->duration_ms);
  else if (const auto* value = std::get_if<InvalidationAction>(&action))
    rejectDuration(value->duration_ms);
  return errors;
}

bool actionAllowed(const CsrOptionalObservation& before, CsrObservationActionKind kind)
{
  if (kind == CsrObservationActionKind::kRead)
    return true;
  if (!before)
    return kind == CsrObservationActionKind::kRelease;
  if (before->state == CsrSnapshotState::kReady)
    return true;
  if (before->state == CsrSnapshotState::kInvalidated)
    return kind == CsrObservationActionKind::kRelease;
  return false;
}

Violations disallowedActionViolation(const CsrOptionalObservation& before, CsrObservationActionKind kind)
{
  if (before && before->state == CsrSnapshotState::kReleased && kind == CsrObservationActionKind::kRelease)
    return {{"observation.state", "release is not allowed for released observation"}};
  return {{"observation.state", std::string(token(kind)) + " requires ready observation"}};
}

}  // namespace

std::string_view token(CsrObservationActionKind kind)
{
  switch (kind) {
    case CsrObservationActionKind::kRead:
      return "read";
    case CsrObservationActionKind::kReferenceMeasurement:
      return "reference_measurement";
    case CsrObservationActionKind::kScatterRejection:
      return "scatter_rejection";
    case CsrObservationActionKind::kScatterSuccess:
      return "scatter_success";
    case CsrObservationActionKind::kInvalidation:
      return "invalidation";
    case CsrObservationActionKind::kRelease:
      return "release";
  }
  throw std::invalid_argument("unknown CsrObservationActionKind");
}

CsrObservationActionKindResult observationActionKind(const CsrObservationAction& action)
{
  if (action.valueless_by_exception())
    return valuelessActionViolation();
  return std::visit(
      [](const auto& value) -> CsrObservationActionKind {
        using Action = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Action, ReadAction>)
          return CsrObservationActionKind::kRead;
        else if constexpr (std::is_same_v<Action, ReferenceMeasurementAction>)
          return CsrObservationActionKind::kReferenceMeasurement;
        else if constexpr (std::is_same_v<Action, ScatterRejectionAction>)
          return CsrObservationActionKind::kScatterRejection;
        else if constexpr (std::is_same_v<Action, ScatterSuccessAction>)
          return CsrObservationActionKind::kScatterSuccess;
        else if constexpr (std::is_same_v<Action, InvalidationAction>)
          return CsrObservationActionKind::kInvalidation;
        else
          return CsrObservationActionKind::kRelease;
      },
      action);
}

CsrOptionalObservationResult reduceSnapshotObservation(const CsrOptionalObservation& before, const CsrObservationAction& action)
{
  if (action.valueless_by_exception())
    return valuelessActionViolation();
  const auto kind_result = observationActionKind(action);
  if (const auto* errors = std::get_if<Violations>(&kind_result))
    return *errors;
  const auto kind = std::get<CsrObservationActionKind>(kind_result);

  Violations errors;
  if (before)
    append(errors, validateSnapshotObservation(*before));
  append(errors, validateActionPayload(action));
  if (!errors.empty())
    return errors;
  if (!actionAllowed(before, kind))
    return disallowedActionViolation(before, kind);
  if (kind == CsrObservationActionKind::kRead || !before)
    return before;

  CsrOptionalObservation after = before;
  auto& observation = *after;
  if (const auto* value = std::get_if<ReferenceMeasurementAction>(&action)) {
    observation.timing_ms.reference_ms = value->duration_ms;
  } else if (const auto* value = std::get_if<ScatterRejectionAction>(&action)) {
    observation.last_rejection = CsrScatterRejectionEvent{value->reason, 0, value->source, observation.identity};
    observation.timing_ms.scatter_ms = value->duration_ms;
  } else if (const auto* value = std::get_if<ScatterSuccessAction>(&action)) {
    observation.last_rejection.reset();
    observation.timing_ms.scatter_ms = value->duration_ms;
  } else if (const auto* value = std::get_if<InvalidationAction>(&action)) {
    const auto reason = snapshot_internal::firstInvalidationReason(observation.identity, value->current_fingerprints);
    if (!reason)
      return Violations{{"action.current_fingerprints", "invalidation requires at least one absent or changed fingerprint"}};
    observation.state = CsrSnapshotState::kInvalidated;
    observation.last_rejection.reset();
    observation.last_invalidation = CsrInvalidationEvent{*reason, 0, observation.identity, value->current_fingerprints};
    observation.timing_ms.scatter_ms = value->duration_ms;
  } else if (std::holds_alternative<ReleaseAction>(action)) {
    observation.state = CsrSnapshotState::kReleased;
  }

  errors = validateSnapshotObservation(observation);
  if (!errors.empty())
    return errors;
  return after;
}

std::vector<CsrContractViolation> validateSnapshotObservationTransition(const CsrOptionalObservation& before, const CsrOptionalObservation& after,
                                                                        const CsrObservationAction& action)
{
  if (action.valueless_by_exception())
    return valuelessActionViolation();
  Violations errors;
  if (before)
    append(errors, validateSnapshotObservation(*before));
  if (after)
    append(errors, validateSnapshotObservation(*after));
  if (!errors.empty())
    return errors;
  CsrOptionalObservationResult expected = reduceSnapshotObservation(before, action);
  if (const auto* reduce_errors = std::get_if<Violations>(&expected))
    return *reduce_errors;
  if (after != std::get<CsrOptionalObservation>(expected))
    return {{"transition", "after observation must equal reducer result"}};
  return {};
}

}  // namespace ista::csr
