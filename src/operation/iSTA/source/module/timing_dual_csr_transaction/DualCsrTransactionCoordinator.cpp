// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTransactionCoordinator.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "DualCsrArcGroupResolverValidation.hpp"

namespace ista::timing_dual_csr {
namespace {

void rejectViolation(DualCsrViolations& errors, DualCsrViolationCode code,
                     std::string field, std::string message)
{
  errors.push_back({code, std::move(field), std::move(message)});
}

bool prefixMatches(const CommittedDualCsrAnalysisState& committed,
                   const CommittedDualCsrAnalysisState& staged)
{
  if (staged.descriptor_trace.size() != committed.descriptor_trace.size() + 1
      || staged.semantic_trace.size() != committed.semantic_trace.size() + 1
      || staged.execution_trace.size()
             != committed.execution_trace.size() + 1)
    return false;
  return std::equal(committed.descriptor_trace.begin(),
                    committed.descriptor_trace.end(),
                    staged.descriptor_trace.begin())
         && std::equal(committed.semantic_trace.begin(),
                       committed.semantic_trace.end(),
                       staged.semantic_trace.begin())
         && std::equal(committed.execution_trace.begin(),
                       committed.execution_trace.end(),
                       staged.execution_trace.begin());
}

void clearSession(csr::CsrSnapshotIdentity& identity) noexcept
{
  identity.session_token = 0;
}

std::vector<DualCsrBatchDescriptor> sessionNeutralDescriptors(
    std::vector<DualCsrBatchDescriptor> descriptors)
{
  for (auto& descriptor : descriptors) {
    descriptor.session_token = timing_edit::TimingEditSessionToken{};
    if (descriptor.reference_payload.source_identity) {
      clearSession(
          descriptor.reference_payload.source_identity->expected);
      clearSession(
          descriptor.reference_payload.source_identity->replacement);
    }
  }
  return descriptors;
}

bool recoveryTelemetryMatches(const DualCsrTelemetry& current,
                              const DualCsrTelemetry& rebuilt)
{
  if (current.full_rebuild_count
      == std::numeric_limits<std::uint64_t>::max())
    return false;
  DualCsrTelemetry expected = current;
  ++expected.full_rebuild_count;
  return expected == rebuilt;
}

bool recoverySemanticsMatch(
    const CommittedDualCsrAnalysisState& current,
    const CommittedDualCsrAnalysisState& rebuilt)
{
  auto current_reference = current.graph.reference_payload;
  auto rebuilt_reference = rebuilt.graph.reference_payload;
  clearSession(current_reference.source_identity);
  clearSession(rebuilt_reference.source_identity);

  auto current_timing = current.timing;
  auto rebuilt_timing = rebuilt.timing;
  clearSession(current_timing.values.source_identity);
  clearSession(rebuilt_timing.values.source_identity);

  auto current_index = current.arc_group_index;
  auto rebuilt_index = rebuilt.arc_group_index;
  current_index.session_token = timing_edit::TimingEditSessionToken{};
  rebuilt_index.session_token = timing_edit::TimingEditSessionToken{};

  return current.generation == rebuilt.generation
         && current.graph.high_water == rebuilt.graph.high_water
         && current.graph.nodes == rebuilt.graph.nodes
         && current.graph.arc_slots == rebuilt.graph.arc_slots
         && current.graph.base_out.row_offsets
                == rebuilt.graph.base_out.row_offsets
         && current.graph.base_out.physical_slots
                == rebuilt.graph.base_out.physical_slots
         && current.graph.base_in.row_offsets
                == rebuilt.graph.base_in.row_offsets
         && current.graph.base_in.physical_slots
                == rebuilt.graph.base_in.physical_slots
         && current.graph.aux_out.row_offsets
                == rebuilt.graph.aux_out.row_offsets
         && current.graph.aux_out.row_sources
                == rebuilt.graph.aux_out.row_sources
         && current.graph.aux_out.arc_ids == rebuilt.graph.aux_out.arc_ids
         && current.graph.aux_out.source_to_aux_row
                == rebuilt.graph.aux_out.source_to_aux_row
         && current_reference == rebuilt_reference
         && current_timing == rebuilt_timing
         && current.resolver == rebuilt.resolver
         && current_index == rebuilt_index
         && current.identity == rebuilt.identity
         && recoveryTelemetryMatches(current.graph.telemetry,
                                     rebuilt.graph.telemetry)
         && current.semantic_trace == rebuilt.semantic_trace
         && sessionNeutralDescriptors(current.descriptor_trace)
                == sessionNeutralDescriptors(rebuilt.descriptor_trace)
         && current.traversal_telemetry == rebuilt.traversal_telemetry
         && current.execution_trace == rebuilt.execution_trace;
}

}  // namespace

DualCsrRecoveryAttempt makeDualCsrVerifiedRecovery(
    const CommittedDualCsrAnalysisState& poisoned_committed,
    CommittedDualCsrAnalysisState rebuilt)
{
  DualCsrRecoveryAttempt attempt;
  attempt.violations
      = validateCommittedDualCsrAnalysisState(poisoned_committed);
  DualCsrViolations rebuilt_errors
      = validateCommittedDualCsrAnalysisState(rebuilt);
  attempt.violations.insert(attempt.violations.end(),
                            std::make_move_iterator(rebuilt_errors.begin()),
                            std::make_move_iterator(rebuilt_errors.end()));
  auto poisoned_binding_errors = validateDualCsrArcGroupResolverFull(
      poisoned_committed.arc_group_index, poisoned_committed.resolver);
  auto rebuilt_binding_errors = validateDualCsrArcGroupResolverFull(
      rebuilt.arc_group_index, rebuilt.resolver);
  attempt.violations.insert(attempt.violations.end(),
      std::make_move_iterator(poisoned_binding_errors.begin()),
      std::make_move_iterator(poisoned_binding_errors.end()));
  attempt.violations.insert(attempt.violations.end(),
      std::make_move_iterator(rebuilt_binding_errors.begin()),
      std::make_move_iterator(rebuilt_binding_errors.end()));
  if (!poisoned_committed.execution_trace.empty()
      || !rebuilt.execution_trace.empty())
    rejectViolation(
        attempt.violations, DualCsrViolationCode::kConflict,
        "recovery.mode_replay_required",
        "aggregates with persisted execution modes must use the mode-dispatched recovery replay capability");
  if (poisoned_committed.graph.session_token
          == rebuilt.graph.session_token
      || !recoverySemanticsMatch(poisoned_committed, rebuilt))
    rejectViolation(attempt.violations, DualCsrViolationCode::kConflict,
                    "recovery",
                    "fresh recovery must rebind only session identity, increment full-rebuild count exactly once, and preserve every other committed semantic and telemetry field");
  if (attempt.violations.empty())
    attempt.recovery.emplace(
        DualCsrVerifiedRecovery(poisoned_committed,
                                std::move(rebuilt)));
  return attempt;
}

DualCsrTransactionCoordinator::DualCsrTransactionCoordinator(
    CommittedDualCsrAnalysisState initial)
    : _committed(std::move(initial))
{
  const auto errors = validateCommittedDualCsrAnalysisState(_committed);
  const auto binding_errors = validateDualCsrArcGroupResolverFull(
      _committed.arc_group_index, _committed.resolver);
  if (!errors.empty() || !binding_errors.empty())
    throw std::invalid_argument("invalid initial Dual-CSR aggregate");
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::reject(
    DualCsrTransactionCode code) const noexcept
{
  return {code, false, _poisoned};
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::publish(
    DualCsrVerifiedStage verified,
    DualCsrPublishFailpoint failpoint)
{
  if (_poisoned)
    return reject(DualCsrTransactionCode::kPoisoned);
  if (failpoint != DualCsrPublishFailpoint::kNone
      && (!incregpusta_debug_checks_enabled || (failpoint != DualCsrPublishFailpoint::kBeforeSwap && failpoint != DualCsrPublishFailpoint::kAfterSwap)))
    return reject(DualCsrTransactionCode::kUnknownFailpoint);
  if (incregpusta_debug_checks_enabled && !validateCommittedDualCsrAnalysisState(_committed).empty()) {
    _poisoned = true;
    return reject(DualCsrTransactionCode::kInvalidCommittedState);
  }
  if (!(verified._expected_committed.matches(_committed)))
    return reject(DualCsrTransactionCode::kTraceMismatch);

  CommittedDualCsrAnalysisState staged = std::move(verified._state);
  if (incregpusta_debug_checks_enabled && !validateCommittedDualCsrAnalysisState(staged).empty())
    return reject(DualCsrTransactionCode::kInvalidStagedState);
  if (staged.graph.session_token != _committed.graph.session_token
      || staged.generation.value() != _committed.generation.value() + 1)
    return reject(DualCsrTransactionCode::kStaleGeneration);
  if (incregpusta_debug_checks_enabled && !prefixMatches(_committed, staged))
    return reject(DualCsrTransactionCode::kTraceMismatch);
  if (failpoint == DualCsrPublishFailpoint::kBeforeSwap)
    return reject(DualCsrTransactionCode::kPreSwapFault);

  _committed.swap(staged);
  if (failpoint == DualCsrPublishFailpoint::kAfterSwap) {
    _poisoned = true;
    return {DualCsrTransactionCode::kPostSwapPoisoned, true, true};
  }
  return {DualCsrTransactionCode::kOk, true, false};
}

DualCsrTransactionStatus DualCsrTransactionCoordinator::recover(
    DualCsrVerifiedRecovery verified)
{
  if (!_poisoned)
    return reject(DualCsrTransactionCode::kRecoveryNotPoisoned);
  if (!(verified._expected_poisoned.matches(_committed)))
    return reject(DualCsrTransactionCode::kTraceMismatch);

  if (!_committed.execution_trace.empty()
      || !verified._state.execution_trace.empty())
    return reject(DualCsrTransactionCode::kTraceMismatch);
  CommittedDualCsrAnalysisState rebuilt = std::move(verified._state);
  if (!validateCommittedDualCsrAnalysisState(rebuilt).empty())
    return reject(DualCsrTransactionCode::kInvalidStagedState);
  if (rebuilt.graph.session_token == _committed.graph.session_token)
    return reject(DualCsrTransactionCode::kTraceMismatch);
  _committed.swap(rebuilt);
  _poisoned = false;
  return {DualCsrTransactionCode::kOk, true, false};
}

}  // namespace ista::timing_dual_csr
