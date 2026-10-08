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
#include "GreedyBufferCandidateSelector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "BufferTraceCanonicalEncoding.hpp"
#include "BufferInsertionGain.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "TimingEditByteOrder.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditValidation.hpp"

namespace ista::buffer_trace {
namespace {

using Bytes = std::vector<std::uint8_t>;
using ByteSet = std::set<Bytes, timing_edit::detail::CanonicalBytesLess>;

bool validText(const std::string& value)
{
  return !value.empty() && value.find('\0') == std::string::npos;
}

bool bytesLess(const Bytes& lhs, const Bytes& rhs)
{
  return timing_edit::detail::canonicalBytesLess(lhs, rhs);
}

struct EvaluatedCandidate
{
  BufferCandidateObservation candidate;
  CandidateEvaluationReceipt receipt;
  Bytes identity_bytes;
  Bytes net_bytes;
  Bytes driver_bytes;
  Bytes pin_bytes;
  Bytes buffer_bytes;
};

struct NetGroup
{
  Bytes net_bytes;
  Bytes driver_bytes;
  double slack_ns{0.0};
  std::vector<std::size_t> candidates;
};

CandidateEvaluationReceipt basicReceipt(const BufferCandidateObservation& candidate)
{
  return CandidateEvaluationReceipt{.net = candidate.net,
                                    .driver = candidate.driver,
                                    .candidate_pin = candidate.candidate_pin,
                                    .buffer = candidate.buffer,
                                    .numeric = std::nullopt,
                                    .decision = CandidateDecision::kRejected,
                                    .rejection_reason = std::nullopt,
                                    .accepted_creation_token = std::nullopt};
}

bool validLibraryCell(const timing_edit::LibraryCellRef& cell)
{
  if (!validText(cell.library_name) || !validText(cell.cell_name) || !validText(cell.input_port) || !validText(cell.output_port)
      || cell.input_port == cell.output_port || cell.ordered_arc_signatures.empty())
    return false;

  std::optional<Bytes> previous;
  bool positive_unate = false;
  for (const auto& signature : cell.ordered_arc_signatures) {
    if (!validText(signature.library_name) || signature.library_name != cell.library_name || signature.arc_index < 0)
      return false;
    if (signature.sense != csr::TimingSense::kPositiveUnate)
      return false;
    positive_unate = true;
    const Bytes current = csr::canonicalLibraryArcSignatureBytes(signature);
    if (previous && !bytesLess(*previous, current))
      return false;
    previous = current;
  }
  return positive_unate;
}

bool validEnvironment(const BufferCandidateObservation& candidate)
{
  const bool placement_valid = std::visit(
      [](const auto& placement) {
        using Placement = std::decay_t<decltype(placement)>;
        if constexpr (std::is_same_v<Placement, timing_edit::UnplacedInputs>)
          return true;
        else
          return std::isfinite(placement.x_microns) && std::isfinite(placement.y_microns);
      },
      candidate.placement);
  if (!placement_valid)
    return false;
  try {
    return std::visit(
        [](const auto& rc) {
          using Rc = std::decay_t<decltype(rc)>;
          if constexpr (std::is_same_v<Rc, timing_edit::NoParasiticRcInputs>)
            return true;
          else {
            static_cast<void>(timing_edit::token(rc.parasitic_policy));
            return true;
          }
        },
        candidate.rc);
  } catch (const std::invalid_argument&) {
    return false;
  }
}

std::optional<CandidateRejectionReason> structuralRejection(const BufferCandidateObservation& candidate)
{
  if (!timing_edit::validateStableNetRef(candidate.net).empty() || !timing_edit::validateStableNodeRef(candidate.driver).empty()
      || !timing_edit::validateStableNodeRef(candidate.candidate_pin).empty())
    return CandidateRejectionReason::kInvalidSemanticReference;
  for (const auto& sink : candidate.moved_sinks)
    if (!timing_edit::validateStableNodeRef(sink).empty())
      return CandidateRejectionReason::kInvalidSemanticReference;

  if (candidate.moved_sinks.size() != 1
      || timing_edit::canonicalStableNodeRefBytes(candidate.moved_sinks.front())
             != timing_edit::canonicalStableNodeRefBytes(candidate.candidate_pin)
      || timing_edit::canonicalStableNodeRefBytes(candidate.driver) == timing_edit::canonicalStableNodeRefBytes(candidate.candidate_pin))
    return CandidateRejectionReason::kInvalidMovedSinkSet;
  if (!validLibraryCell(candidate.buffer))
    return CandidateRejectionReason::kInvalidBufferCell;
  if (!validEnvironment(candidate))
    return CandidateRejectionReason::kInvalidEnvironmentInputs;
  return std::nullopt;
}

std::variant<GainComputationReceipt, CandidateRejectionReason> evaluateNumeric(const BufferCandidateObservation& candidate,
                                                                               const SelectionProfile& profile)
{
  if (!candidate.setup_timing)
    return CandidateRejectionReason::kTimingNotMaterialized;
  const auto& timing = *candidate.setup_timing;
  if (!std::isfinite(timing.arrival_ns) || !std::isfinite(timing.required_ns) || !std::isfinite(timing.slack_ns))
    return CandidateRejectionReason::kNonFiniteTiming;
  const double derived_slack_ns = timing.required_ns - timing.arrival_ns;
  if (!std::isfinite(derived_slack_ns))
    return CandidateRejectionReason::kNonFiniteTiming;
  if (derived_slack_ns != timing.slack_ns)
    return CandidateRejectionReason::kSlackMismatch;

  const double inputs[] = {candidate.upstream_resistance_kohm, candidate.candidate_load_pf, candidate.buffer_capacitance_pf,
                           candidate.buffer_resistance_kohm, candidate.buffer_delay_ns};
  if (!std::all_of(std::begin(inputs), std::end(inputs), [](double value) { return std::isfinite(value); }))
    return CandidateRejectionReason::kNonFiniteIntermediate;
  if (!std::all_of(std::begin(inputs), std::end(inputs), [](double value) { return value >= 0.0; }))
    return CandidateRejectionReason::kNegativeOperand;
  if (candidate.upstream_resistance_kohm != profile.rc_profile.upstream_resistance_kohm
      || candidate.buffer_resistance_kohm != profile.rc_profile.buffer_resistance_kohm)
    return CandidateRejectionReason::kInvalidEnvironmentInputs;

  const auto gain = timing_edit::bufferInsertionGain(candidate.upstream_resistance_kohm,
      candidate.candidate_load_pf, candidate.buffer_capacitance_pf,
      candidate.buffer_resistance_kohm, candidate.buffer_delay_ns);
  if (!gain) return CandidateRejectionReason::kNonFiniteIntermediate;

  return GainComputationReceipt{.arrival_ns = timing.arrival_ns,
                                .required_ns = timing.required_ns,
                                .slack_ns = timing.slack_ns,
                                .upstream_resistance_kohm = candidate.upstream_resistance_kohm,
                                .candidate_load_pf = candidate.candidate_load_pf,
                                .buffer_capacitance_pf = candidate.buffer_capacitance_pf,
                                .delta_capacitance_pf = gain->delta_capacitance_pf,
                                .benefit_ns = gain->benefit_ns,
                                .buffer_resistance_kohm = candidate.buffer_resistance_kohm,
                                .penalty_ns = gain->penalty_ns,
                                .benefit_minus_penalty_ns = gain->benefit_minus_penalty_ns,
                                .buffer_delay_ns = candidate.buffer_delay_ns,
                                .gain_ns = gain->gain_ns};
}

std::variant<EvaluatedCandidate, GreedySelectionError> evaluate(const BufferCandidateObservation& candidate,
                                                                const SelectionProfile& profile, std::size_t input_index)
{
  CandidateEvaluationReceipt receipt = basicReceipt(candidate);
  if (const auto reason = structuralRejection(candidate)) {
    receipt.rejection_reason = reason;
  } else {
    auto numeric = evaluateNumeric(candidate, profile);
    if (const auto* reason = std::get_if<CandidateRejectionReason>(&numeric)) {
      receipt.rejection_reason = *reason;
    } else {
      receipt.numeric = std::get<GainComputationReceipt>(numeric);
      if (receipt.numeric->gain_ns > +0.0) {
        receipt.decision = CandidateDecision::kLowerRankedCandidate;
      } else {
        receipt.rejection_reason = CandidateRejectionReason::kNonPositiveGain;
      }
    }
  }

  try {
    return EvaluatedCandidate{.candidate = candidate,
                              .receipt = std::move(receipt),
                              .identity_bytes = canonicalBufferCandidateIdentityBytes(candidate),
                              .net_bytes = timing_edit::canonicalStableNetRefBytes(candidate.net),
                              .driver_bytes = timing_edit::canonicalStableNodeRefBytes(candidate.driver),
                              .pin_bytes = timing_edit::canonicalStableNodeRefBytes(candidate.candidate_pin),
                              .buffer_bytes = timing_edit::canonicalLibraryCellRefBytes(candidate.buffer)};
  } catch (const std::exception& error) {
    return GreedySelectionError{GreedySelectionErrorCode::kInvalidRequest, "request.candidates[" + std::to_string(input_index) + "]",
                                std::string("candidate cannot be canonically identified: ") + error.what()};
  }
}

bool rankedCandidateLess(const EvaluatedCandidate& lhs, const EvaluatedCandidate& rhs)
{
  const double lhs_gain = lhs.receipt.numeric->gain_ns;
  const double rhs_gain = rhs.receipt.numeric->gain_ns;
  if (lhs_gain != rhs_gain)
    return lhs_gain > rhs_gain;
  if (lhs.pin_bytes != rhs.pin_bytes)
    return bytesLess(lhs.pin_bytes, rhs.pin_bytes);
  return bytesLess(lhs.buffer_bytes, rhs.buffer_bytes);
}

GreedySelectionError conflict(GreedySelectionErrorCode code, std::string field, std::string message)
{
  return GreedySelectionError{code, std::move(field), std::move(message)};
}

}  // namespace

std::string_view token(GreedySelectionErrorCode value)
{
  switch (value) {
    case GreedySelectionErrorCode::kInvalidRequest: return "invalid_request";
    case GreedySelectionErrorCode::kDuplicateCandidate: return "duplicate_candidate";
    case GreedySelectionErrorCode::kInconsistentNetDriver: return "inconsistent_net_driver";
    case GreedySelectionErrorCode::kAcceptedReferenceConflict: return "accepted_reference_conflict";
    case GreedySelectionErrorCode::kCreationTokenConflict: return "creation_token_conflict";
    case GreedySelectionErrorCode::kMaterializedNameConflict: return "materialized_name_conflict";
  }
  throw std::invalid_argument("unknown GreedySelectionErrorCode");
}

GreedySelectionResult GreedyBufferCandidateSelector::select(const GreedySelectionRequest& request) const
{
  if (const auto violations = validateSelectionProfile(request.profile); !violations.empty())
    return conflict(GreedySelectionErrorCode::kInvalidRequest, "request.profile", violations.front().message);
  if (!validText(request.case_id))
    return conflict(GreedySelectionErrorCode::kInvalidRequest, "request.case_id", "must be nonempty and contain no NUL");
  if (request.batch_ordinal == 0)
    return conflict(GreedySelectionErrorCode::kInvalidRequest, "request.batch_ordinal", "must be nonzero");
  if (request.max_acceptances == 0)
    return conflict(GreedySelectionErrorCode::kInvalidRequest, "request.max_acceptances",
                    "must be nonzero");

  std::set<timing_edit::CreationToken> reserved_tokens;
  for (const auto& reserved : request.reserved_creation_tokens) {
    if (!reserved_tokens.insert(reserved).second)
      return conflict(GreedySelectionErrorCode::kInvalidRequest, "request.reserved_creation_tokens", "must be duplicate-free");
  }
  std::set<std::string> reserved_names;
  for (const auto& reserved : request.reserved_materialized_names) {
    if (!validText(reserved) || !reserved_names.insert(reserved).second)
      return conflict(GreedySelectionErrorCode::kInvalidRequest, "request.reserved_materialized_names",
                      "must contain unique nonempty NUL-free names");
  }

  std::vector<EvaluatedCandidate> evaluated;
  evaluated.reserve(request.candidates.size());
  for (std::size_t index = 0; index < request.candidates.size(); ++index) {
    auto result = evaluate(request.candidates[index], request.profile, index);
    if (const auto* error = std::get_if<GreedySelectionError>(&result))
      return *error;
    evaluated.push_back(std::get<EvaluatedCandidate>(std::move(result)));
  }
  std::sort(evaluated.begin(), evaluated.end(), [](const EvaluatedCandidate& lhs, const EvaluatedCandidate& rhs) {
    return bytesLess(lhs.identity_bytes, rhs.identity_bytes);
  });
  for (std::size_t index = 1; index < evaluated.size(); ++index)
    if (evaluated[index - 1].identity_bytes == evaluated[index].identity_bytes)
      return conflict(GreedySelectionErrorCode::kDuplicateCandidate, "request.candidates", "duplicate semantic candidate identity");

  std::vector<NetGroup> nets;
  for (std::size_t index = 0; index < evaluated.size(); ++index) {
    const auto& item = evaluated[index];
    if (!item.receipt.numeric)
      continue;
    auto net = std::find_if(nets.begin(), nets.end(), [&](const NetGroup& group) { return group.net_bytes == item.net_bytes; });
    if (net == nets.end()) {
      nets.push_back(NetGroup{item.net_bytes, item.driver_bytes, item.receipt.numeric->slack_ns, {index}});
      continue;
    }
    if (net->driver_bytes != item.driver_bytes)
      return conflict(GreedySelectionErrorCode::kInconsistentNetDriver, "request.candidates", "one stable net has multiple candidate drivers");
    net->slack_ns = std::min(net->slack_ns, item.receipt.numeric->slack_ns);
    net->candidates.push_back(index);
  }
  std::sort(nets.begin(), nets.end(), [](const NetGroup& lhs, const NetGroup& rhs) {
    if (lhs.slack_ns != rhs.slack_ns)
      return lhs.slack_ns < rhs.slack_ns;
    if (lhs.net_bytes != rhs.net_bytes)
      return bytesLess(lhs.net_bytes, rhs.net_bytes);
    return bytesLess(lhs.driver_bytes, rhs.driver_bytes);
  });

  std::vector<std::size_t> accepted_indices;
  const std::size_t critical_count
      = static_cast<std::size_t>(std::min<std::uint64_t>(request.profile.top_k, nets.size()));
  for (std::size_t net_index = 0; net_index < nets.size(); ++net_index) {
    std::vector<std::size_t> positive;
    for (const std::size_t candidate_index : nets[net_index].candidates)
      if (evaluated[candidate_index].receipt.numeric->gain_ns > +0.0)
        positive.push_back(candidate_index);
    std::sort(positive.begin(), positive.end(), [&](std::size_t lhs, std::size_t rhs) {
      return rankedCandidateLess(evaluated[lhs], evaluated[rhs]);
    });
    if (net_index >= critical_count) {
      for (const std::size_t index : positive)
        evaluated[index].receipt.decision = CandidateDecision::kNonCriticalNet;
      continue;
    }
    if (accepted_indices.size() >= request.max_acceptances) {
      for (const std::size_t index : positive)
        evaluated[index].receipt.decision = CandidateDecision::kLowerRankedCandidate;
      continue;
    }
    if (!positive.empty()) {
      evaluated[positive.front()].receipt.decision = CandidateDecision::kAccepted;
      accepted_indices.push_back(positive.front());
      for (std::size_t index = 1; index < positive.size(); ++index)
        evaluated[positive[index]].receipt.decision = CandidateDecision::kLowerRankedCandidate;
    }
  }

  ByteSet accepted_nets;
  ByteSet accepted_nodes;
  std::vector<SelectedBufferCandidate> accepted;
  std::vector<timing_edit::CreationToken> accepted_tokens;
  accepted.reserve(accepted_indices.size());
  accepted_tokens.reserve(accepted_indices.size());
  for (std::size_t ordinal = 0; ordinal < accepted_indices.size(); ++ordinal) {
    auto& item = evaluated[accepted_indices[ordinal]];
    if (!accepted_nets.insert(item.net_bytes).second || !accepted_nodes.insert(item.driver_bytes).second
        || !accepted_nodes.insert(item.pin_bytes).second)
      return conflict(GreedySelectionErrorCode::kAcceptedReferenceConflict, "selection.accepted",
                      "accepted candidates share a stable net or node reference");

    timing_edit::CreationToken creation_token{.case_id = request.case_id,
                                                .profile_id = std::string(token(request.profile.id)),
                                                .seed = request.seed,
                                                .batch_ordinal = request.batch_ordinal,
                                                .accepted_ordinal = ordinal};
    if (reserved_tokens.contains(creation_token))
      return conflict(GreedySelectionErrorCode::kCreationTokenConflict, "selection.accepted.creation_token",
                      "derived creation token collides with a reserved token");
    const auto materialization = timing_edit::defaultTokenMaterialization(creation_token, item.candidate.buffer);
    for (const auto* name : {&materialization.instance_name, &materialization.output_net_name, &materialization.input_pin_name,
                             &materialization.output_pin_name})
      if (!reserved_names.insert(*name).second)
        return conflict(GreedySelectionErrorCode::kMaterializedNameConflict, "selection.accepted.materialized_name",
                        "derived materialized name collides with an existing name");

    item.receipt.accepted_creation_token = creation_token;
    accepted_tokens.push_back(creation_token);
    accepted.push_back(SelectedBufferCandidate{item.candidate, *item.receipt.numeric, std::move(creation_token)});
  }

  GenerationRoundReceipt receipt;
  receipt.batch_ordinal = request.batch_ordinal;
  receipt.accepted_tokens = std::move(accepted_tokens);
  receipt.evaluations.reserve(evaluated.size());
  for (auto& item : evaluated)
    receipt.evaluations.push_back(std::move(item.receipt));
  return GreedySelection{std::move(accepted), std::move(receipt)};
}

}  // namespace ista::buffer_trace
