// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferTraceValidationRules.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace ista::buffer_trace::detail {
namespace {

void reject(BufferTraceContractViolations& errors,
            BufferTraceContractCode code, std::string field,
            std::string message)
{
  errors.push_back(
      BufferTraceContractViolation{code, std::move(field), std::move(message)});
}

bool expectedComparability(const ReplayHeader& header)
{
  if (header.workload_kind == SemanticWorkloadKind::kRealDesign) {
    return header.comparability_status == ComparabilityStatus::kNonComparable
           && header.comparability_reason
                  == ComparabilityReason::kEngineeringRcNoPhysical;
  }
  if (header.profile_id == ProfileId::kEngineeringSmoke) {
    return header.comparability_status == ComparabilityStatus::kNonComparable
           && header.comparability_reason
                  == ComparabilityReason::kEngineeringRcNoPhysical;
  }
  return header.comparability_status == ComparabilityStatus::kProtocolOnly
         && !header.comparability_reason;
}

}  // namespace

void appendReplayProfileViolations(
    const ReplayHeader& header, const std::vector<timing_edit::EditBatch>& replay_batches,
    BufferTraceContractViolations& errors)
{
  if (!expectedComparability(header)) {
    reject(errors, BufferTraceContractCode::kInvalidProfile,
           "replay.header.comparability_status",
           "comparability must exactly match workload kind and frozen profile");
  }

  const SelectionProfile profile
      = frozenSelectionProfile(header.profile_id);
  if (replay_batches.size() > profile.rounds) {
    reject(errors, BufferTraceContractCode::kGenerationMismatch,
           "replay.workload.batches",
           "replay batch count exceeds the frozen profile rounds");
  }
  std::uint64_t total_edits = 0;
  for (std::size_t index = 0; index < replay_batches.size(); ++index) {
    const auto& edits = replay_batches[index].edits;
    const std::string field = "replay.workload.batches["
                              + std::to_string(index) + "].edits";
    if (edits.size() > profile.top_k) {
      reject(errors, BufferTraceContractCode::kGenerationMismatch, field,
             "replay batch exceeds the frozen profile top-k");
    }
    if (edits.size() > profile.max_total_insertions - total_edits) {
      reject(errors, BufferTraceContractCode::kGenerationMismatch,
             "replay.workload.batches",
             "replay edits exceed the frozen profile insertion limit");
      total_edits = profile.max_total_insertions;
    } else {
      total_edits += edits.size();
    }
  }
}

void appendEvaluationContractViolations(
    const CandidateEvaluationReceipt& evaluation,
    const SelectionProfile& profile, const std::string& field,
    BufferTraceContractViolations& errors)
{
  const bool accepted = evaluation.decision == CandidateDecision::kAccepted;
  const bool ranked
      = evaluation.decision == CandidateDecision::kNonCriticalNet
        || evaluation.decision == CandidateDecision::kLowerRankedCandidate;
  const bool rejected = evaluation.decision == CandidateDecision::kRejected;
  const bool positive_numeric
      = evaluation.numeric && evaluation.numeric->gain_ns > +0.0;
  const bool nonpositive_rejection
      = rejected && evaluation.rejection_reason
        == CandidateRejectionReason::kNonPositiveGain;

  bool decision_consistent = false;
  if (accepted) {
    decision_consistent
        = positive_numeric && evaluation.accepted_creation_token
          && !evaluation.rejection_reason;
  } else if (ranked) {
    decision_consistent
        = positive_numeric && !evaluation.accepted_creation_token
          && !evaluation.rejection_reason;
  } else if (rejected) {
    decision_consistent
        = evaluation.rejection_reason && !evaluation.accepted_creation_token
          && ((nonpositive_rejection && evaluation.numeric
               && !(evaluation.numeric->gain_ns > +0.0))
              || (!nonpositive_rejection && !evaluation.numeric));
  }
  if (!decision_consistent) {
    reject(errors, BufferTraceContractCode::kInvalidValue, field,
           "decision, positive numeric evidence, reason and token disagree");
  }

  if (!evaluation.numeric) {
    return;
  }
  const auto& numeric = *evaluation.numeric;
  const std::array<double, 5> physical_operands{
      numeric.upstream_resistance_kohm, numeric.candidate_load_pf,
      numeric.buffer_capacitance_pf, numeric.buffer_resistance_kohm,
      numeric.buffer_delay_ns};
  if (std::any_of(physical_operands.begin(), physical_operands.end(),
                  [](double value) { return value < 0.0; })) {
    reject(errors, BufferTraceContractCode::kInvalidValue, field + ".numeric",
           "resistance, capacitance and delay operands must be nonnegative");
  }
  if (numeric.upstream_resistance_kohm
          != profile.rc_profile.upstream_resistance_kohm
      || numeric.buffer_resistance_kohm
             != profile.rc_profile.buffer_resistance_kohm) {
    reject(errors, BufferTraceContractCode::kInvalidProfile, field + ".numeric",
           "numeric resistance operands must equal the frozen RC profile");
  }
}

}  // namespace ista::buffer_trace::detail
