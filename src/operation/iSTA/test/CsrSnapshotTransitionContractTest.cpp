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
#include <array>
#include <charconv>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "CsrSnapshotTestFixture.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::snapshot_test;

static_assert(std::is_nothrow_move_constructible_v<ReadAction>);
static_assert(std::is_nothrow_move_constructible_v<ReferenceMeasurementAction>);
static_assert(std::is_nothrow_move_constructible_v<ScatterRejectionAction>);
static_assert(std::is_nothrow_move_constructible_v<ScatterSuccessAction>);
static_assert(std::is_nothrow_move_constructible_v<InvalidationAction>);
static_assert(std::is_nothrow_move_constructible_v<ReleaseAction>);
static_assert(std::is_nothrow_move_constructible_v<CsrObservationAction>);

CsrOptionalObservation reduced(const CsrOptionalObservation& before, const CsrObservationAction& action)
{
  auto result = reduceSnapshotObservation(before, action);
  if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result))
    throw std::runtime_error(errors->front().field + ": " + errors->front().message);
  return std::get<CsrOptionalObservation>(std::move(result));
}

void expectInvalid(const CsrOptionalObservation& before, const CsrObservationAction& action, std::string_view field, std::string_view message,
                   std::string_view name)
{
  const auto original = before;
  const auto result = reduceSnapshotObservation(before, action);
  const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result);
  require(errors != nullptr, std::string(name) + " unexpectedly reduced");
  requireExactViolation(*errors, field, message, name);
  requireExactViolation(validateSnapshotObservationTransition(before, before, action), field, message, std::string(name) + " transition");
  require(before == original, std::string(name) + " modified the input observation on failure");
}

void checkSequencesAndPayloadFailures()
{
  CsrOptionalObservation state = readyObservation();
  state = reduced(state, ReferenceMeasurementAction{0.0});
  require(state->timing_ms.reference_ms == 0.0, "reference null-to-zero transition failed");
  state = reduced(state, ReferenceMeasurementAction{3.5});
  require(state->timing_ms.reference_ms == 3.5, "reference overwrite transition failed");

  state = reduced(state, ScatterRejectionAction{CsrScatterCode::kSchemaMismatch, std::nullopt, 1.0});
  require(state->last_rejection.has_value(), "first rejection did not set the event");
  state = reduced(state, ScatterSuccessAction{2.0});
  require(!state->last_rejection && state->timing_ms.scatter_ms == 2.0, "success did not atomically clear rejection");
  state = reduced(state, ScatterRejectionAction{CsrScatterCode::kSchemaMismatch, std::nullopt, 3.0});
  require(state->last_rejection && state->timing_ms.scatter_ms == 3.0, "second rejection did not replace scatter history");

  const auto current = topologyDriftFingerprints(state->identity);
  state = reduced(state, InvalidationAction{current, 4.0});
  require(state->state == CsrSnapshotState::kInvalidated && !state->last_rejection && state->last_invalidation,
          "invalidation did not atomically replace rejection history");
  const auto invalidated = state;
  state = reduced(state, ReleaseAction{});
  require(state->state == CsrSnapshotState::kReleased, "invalidated release did not change state");
  auto expected = *invalidated;
  expected.state = CsrSnapshotState::kReleased;
  require(*state == expected, "release changed fields other than state");

  const auto ready = readyObservation();
  expectInvalid(ready, InvalidationAction{matchingFingerprints(ready.identity), 1.0}, "action.current_fingerprints",
                "invalidation requires at least one absent or changed fingerprint", "equal-fingerprint invalidation");
  expectInvalid(ready, ScatterRejectionAction{CsrScatterCode::kSessionMismatch, std::nullopt, 1.0}, "observation.last_rejection",
                "rejection reason and source identity disagree", "malformed rejection payload");
}

void checkUnknownKindAndValuelessDefense()
{
  bool threw = false;
  try {
    static_cast<void>(token(static_cast<CsrObservationActionKind>(255)));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "unknown action kind token was accepted");

  // All six alternatives are nothrow-movable in this ABI, so a stable
  // valueless_by_exception fixture cannot be constructed. The static_asserts
  // above prove that fact while production keeps explicit guards at all three
  // action entry points.
  const CsrObservationAction action = ReadAction{};
  require(!action.valueless_by_exception(), "nothrow action unexpectedly became valueless");
}

constexpr std::array<CompiledContractDescriptor, 17> kCases{
    CompiledContractDescriptor{"transition_absent_read", "transition.absent", "replace_value", "after", "null", "read", "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_absent_release", "transition.absent", "replace_value", "after", "null", "release", "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_ready_read", "transition.ready", "replace_value", "after", "exact_copy", "read", "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_invalidated_read", "transition.invalidated", "replace_value", "after", "exact_copy", "read", "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_released_read", "transition.released", "replace_value", "after", "exact_copy", "read", "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_ready_reference", "transition.ready", "replace_value", "action.duration_ms", "1.25", "reference_measurement",
                               "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_ready_rejection", "transition.ready", "replace_value", "action.reason", "schema_mismatch", "scatter_rejection",
                               "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_ready_success", "transition.ready.rejected", "replace_value", "action.duration_ms", "2.5", "scatter_success",
                               "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_ready_invalidation", "transition.ready.rejected", "replace_value",
                               "action.current_fingerprints.topology_fingerprint", "null", "invalidation", "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_ready_release", "transition.ready", "replace_value", "after.state", "released", "release", "valid", "-", "-", "-"},
    CompiledContractDescriptor{"transition_invalidated_release", "transition.invalidated", "replace_value", "after.state", "released", "release", "valid", "-",
                               "-", "-"},
    CompiledContractDescriptor{"transition_released_second_release", "transition.released", "replace_value", "after.state", "released", "release", "invalid",
                               "observation.state", "-", "release is not allowed for released observation"},
    CompiledContractDescriptor{"transition_absent_reference", "transition.absent", "replace_value", "action.duration_ms", "1", "reference_measurement",
                               "invalid", "observation.state", "-", "reference_measurement requires ready observation"},
    CompiledContractDescriptor{"transition_invalidated_success", "transition.invalidated", "replace_value", "action.duration_ms", "1", "scatter_success",
                               "invalid", "observation.state", "-", "scatter_success requires ready observation"},
    CompiledContractDescriptor{"transition_released_invalidation", "transition.released", "replace_value", "action.current_fingerprints.topology_fingerprint",
                               "null", "invalidation", "invalid", "observation.state", "-", "invalidation requires ready observation"},
    CompiledContractDescriptor{"transition_negative_duration", "transition.ready", "replace_value", "action.duration_ms", "-1", "reference_measurement",
                               "invalid", "action.duration_ms", "-", "must be finite and nonnegative"},
    CompiledContractDescriptor{"transition_after_mismatch", "transition.ready", "replace_value", "after.timing_ms.reference", "9", "reference_measurement",
                               "invalid", "transition", "-", "after observation must equal reducer result"},
};

double parseDouble(std::string_view text, std::string_view name)
{
  double value = 0.0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  require(error == std::errc{} && end == text.data() + text.size(), std::string(name) + " is not a canonical floating operand");
  return value;
}

CsrOptionalObservation transitionBaseline(std::string_view baseline)
{
  if (baseline == "transition.absent")
    return std::nullopt;
  if (baseline == "transition.ready")
    return readyObservation();
  if (baseline == "transition.ready.rejected")
    return rejectedObservation();
  if (baseline == "transition.invalidated")
    return invalidatedObservation();
  require(baseline == "transition.released", std::string(baseline) + " is an unknown transition baseline");
  return releasedObservation();
}

CsrObservationAction descriptorAction(const CompiledContractDescriptor& descriptor, const CsrOptionalObservation& before)
{
  const auto action = descriptor.action_token_or_dash;
  if (action == "read") {
    require(descriptor.target == "after" && (descriptor.operand == "null" || descriptor.operand == "exact_copy"),
            std::string(descriptor.case_id) + " has an unknown read recipe");
    return ReadAction{};
  }
  if (action == "release") {
    require((descriptor.target == "after" && descriptor.operand == "null") || (descriptor.target == "after.state" && descriptor.operand == "released"),
            std::string(descriptor.case_id) + " has an unknown release recipe");
    return ReleaseAction{};
  }
  if (action == "reference_measurement") {
    if (descriptor.target == "after.timing_ms.reference") {
      require(descriptor.operand == "9", std::string(descriptor.case_id) + " has an unknown after-mismatch operand");
      return ReferenceMeasurementAction{1.25};
    }
    require(descriptor.target == "action.duration_ms", std::string(descriptor.case_id) + " has an unknown reference target");
    return ReferenceMeasurementAction{parseDouble(descriptor.operand, descriptor.case_id)};
  }
  if (action == "scatter_rejection") {
    require(descriptor.target == "action.reason" && descriptor.operand == "schema_mismatch",
            std::string(descriptor.case_id) + " has an unknown rejection recipe");
    return ScatterRejectionAction{CsrScatterCode::kSchemaMismatch, std::nullopt, 1.0};
  }
  if (action == "scatter_success") {
    require(descriptor.target == "action.duration_ms", std::string(descriptor.case_id) + " has an unknown success target");
    return ScatterSuccessAction{parseDouble(descriptor.operand, descriptor.case_id)};
  }
  require(action == "invalidation" && descriptor.target == "action.current_fingerprints.topology_fingerprint" && descriptor.operand == "null",
          std::string(descriptor.case_id) + " has an unknown invalidation recipe");
  const auto current = before ? topologyDriftFingerprints(before->identity) : CsrCurrentFingerprints{};
  return InvalidationAction{current, 1.0};
}

CsrOptionalObservation descriptorAfter(const CompiledContractDescriptor& descriptor, const CsrOptionalObservation& before, const CsrObservationAction& action)
{
  if (std::holds_alternative<ReadAction>(action))
    return before;
  if (std::holds_alternative<ReleaseAction>(action)) {
    if (!before)
      return std::nullopt;
    auto after = before;
    after->state = CsrSnapshotState::kReleased;
    return after;
  }
  require(before.has_value(), std::string(descriptor.case_id) + " cannot construct an after observation without a baseline");
  auto after = before;
  if (const auto* value = std::get_if<ReferenceMeasurementAction>(&action)) {
    after->timing_ms.reference_ms = descriptor.target == "after.timing_ms.reference" ? parseDouble(descriptor.operand, descriptor.case_id) : value->duration_ms;
  } else if (const auto* value = std::get_if<ScatterRejectionAction>(&action)) {
    after->last_rejection = CsrScatterRejectionEvent{value->reason, 0, value->source, after->identity};
    after->timing_ms.scatter_ms = value->duration_ms;
  } else if (const auto* value = std::get_if<ScatterSuccessAction>(&action)) {
    after->last_rejection.reset();
    after->timing_ms.scatter_ms = value->duration_ms;
  } else {
    const auto& invalidation = std::get<InvalidationAction>(action);
    after->state = CsrSnapshotState::kInvalidated;
    after->last_rejection.reset();
    after->last_invalidation = CsrInvalidationEvent{CsrInvalidationReason::kTopologyChanged, 0, after->identity, invalidation.current_fingerprints};
    after->timing_ms.scatter_ms = invalidation.duration_ms;
  }
  return after;
}

void evaluateCompiledCase(const CompiledContractDescriptor& descriptor)
{
  require(descriptor.recipe_op == "replace_value", std::string(descriptor.case_id) + " has an unknown recipe operation");
  const auto before = transitionBaseline(descriptor.baseline);
  if (before)
    requireValid(validateSnapshotObservation(*before), std::string(descriptor.case_id) + " baseline");
  else
    require(descriptor.baseline == "transition.absent", std::string(descriptor.case_id) + " unexpectedly has no baseline");
  const auto action = descriptorAction(descriptor, before);
  const auto kind_result = observationActionKind(action);
  require(std::holds_alternative<CsrObservationActionKind>(kind_result), std::string(descriptor.case_id) + " action kind failed");
  require(token(std::get<CsrObservationActionKind>(kind_result)) == descriptor.action_token_or_dash,
          std::string(descriptor.case_id) + " action token differs from its descriptor");

  if (descriptor.expected_validity == "valid") {
    const auto after = descriptorAfter(descriptor, before, action);
    requireCompiledOutcome(descriptor, validateSnapshotObservationTransition(before, after, action));
    auto result = reduceSnapshotObservation(before, action);
    const auto* reduced_after = std::get_if<CsrOptionalObservation>(&result);
    require(reduced_after && *reduced_after == after, std::string(descriptor.case_id) + " reducer differs from descriptor recipe");
    return;
  }

  if (descriptor.target == "after.timing_ms.reference") {
    const auto after = descriptorAfter(descriptor, before, action);
    auto result = reduceSnapshotObservation(before, action);
    require(std::holds_alternative<CsrOptionalObservation>(result), std::string(descriptor.case_id) + " action unexpectedly failed to reduce");
    requireCompiledOutcome(descriptor, validateSnapshotObservationTransition(before, after, action));
    return;
  }

  const auto original = before;
  auto result = reduceSnapshotObservation(before, action);
  const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result);
  require(errors != nullptr, std::string(descriptor.case_id) + " unexpectedly reduced");
  requireCompiledOutcome(descriptor, *errors);
  requireCompiledOutcome(descriptor, validateSnapshotObservationTransition(before, before, action));
  require(before == original, std::string(descriptor.case_id) + " modified the input observation on failure");
}

struct DisallowedActionCase
{
  std::string_view baseline;
  std::string_view action;
  std::string_view message;
};

constexpr std::array kDisallowedActions{
    DisallowedActionCase{"transition.absent", "reference_measurement", "reference_measurement requires ready observation"},
    DisallowedActionCase{"transition.absent", "scatter_rejection", "scatter_rejection requires ready observation"},
    DisallowedActionCase{"transition.absent", "scatter_success", "scatter_success requires ready observation"},
    DisallowedActionCase{"transition.absent", "invalidation", "invalidation requires ready observation"},
    DisallowedActionCase{"transition.invalidated", "reference_measurement", "reference_measurement requires ready observation"},
    DisallowedActionCase{"transition.invalidated", "scatter_rejection", "scatter_rejection requires ready observation"},
    DisallowedActionCase{"transition.invalidated", "scatter_success", "scatter_success requires ready observation"},
    DisallowedActionCase{"transition.invalidated", "invalidation", "invalidation requires ready observation"},
    DisallowedActionCase{"transition.released", "reference_measurement", "reference_measurement requires ready observation"},
    DisallowedActionCase{"transition.released", "scatter_rejection", "scatter_rejection requires ready observation"},
    DisallowedActionCase{"transition.released", "scatter_success", "scatter_success requires ready observation"},
    DisallowedActionCase{"transition.released", "invalidation", "invalidation requires ready observation"},
    DisallowedActionCase{"transition.released", "release", "release is not allowed for released observation"},
};

CsrObservationAction matrixAction(std::string_view token_value, const CsrOptionalObservation& before)
{
  if (token_value == "reference_measurement")
    return ReferenceMeasurementAction{1.0};
  if (token_value == "scatter_rejection")
    return ScatterRejectionAction{CsrScatterCode::kSchemaMismatch, std::nullopt, 1.0};
  if (token_value == "scatter_success")
    return ScatterSuccessAction{1.0};
  if (token_value == "invalidation")
    return InvalidationAction{before ? topologyDriftFingerprints(before->identity) : CsrCurrentFingerprints{}, 1.0};
  require(token_value == "release", std::string(token_value) + " is an unknown matrix action");
  return ReleaseAction{};
}

void checkDisallowedActionMatrix()
{
  for (const auto& test_case : kDisallowedActions) {
    const auto before = transitionBaseline(test_case.baseline);
    if (before)
      requireValid(validateSnapshotObservation(*before), std::string(test_case.baseline) + " matrix baseline");
    const auto name = std::string(test_case.baseline) + " " + std::string(test_case.action);
    expectInvalid(before, matrixAction(test_case.action, before), "observation.state", test_case.message, name);
  }
}

void checkTokensAndKinds()
{
  constexpr std::array expected{"read", "reference_measurement", "scatter_rejection", "scatter_success", "invalidation", "release"};
  const std::array<CsrObservationAction, 6> actions{ReadAction{},           ReferenceMeasurementAction{}, ScatterRejectionAction{},
                                                    ScatterSuccessAction{}, InvalidationAction{},         ReleaseAction{}};
  for (std::size_t index = 0; index < actions.size(); ++index) {
    auto result = observationActionKind(actions[index]);
    require(std::holds_alternative<CsrObservationActionKind>(result), "non-valueless action kind failed");
    require(token(std::get<CsrObservationActionKind>(result)) == expected[index], "action kind token parity failed");
  }
}

}  // namespace

int main(int argc, char** argv)
{
  if (listCompiledContractsIfRequested(argc, argv, kCases))
    return 0;
  for (const auto& test_case : kCases)
    evaluateCompiledCase(test_case);
  checkTokensAndKinds();
  checkDisallowedActionMatrix();
  checkSequencesAndPayloadFailures();
  checkUnknownKindAndValuelessDefense();
  return 0;
}
