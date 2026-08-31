// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "BufferTraceCanonicalEncoding.hpp"
#include "CsrTableLookup.hpp"
#include "GreedyBufferCandidateSelector.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditValidation.hpp"

namespace {

using namespace ista;
using namespace ista::buffer_trace;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

csr::NodeKey port(std::string name)
{
  return csr::NodeKey{csr::NodeKind::kPort, std::move(name), {}, {}};
}

csr::NodeKey pin(std::string instance, std::string name)
{
  return csr::NodeKey{csr::NodeKind::kInstancePin,
                      instance + ":" + name,
                      std::move(instance),
                      std::move(name)};
}

timing_edit::LibraryCellRef bufferCell()
{
  csr::LibraryArcSignature arc;
  arc.library_name = "stage4_buffer_lib";
  arc.arc_index = 0;
  arc.sense = csr::TimingSense::kPositiveUnate;
  return timing_edit::LibraryCellRef{
      "stage4_buffer_lib", "BUF_X1", "A", "Y", {std::move(arc)}};
}

BufferCandidateObservation candidate(std::string net_name,
                                     std::string driver_name,
                                     std::string sink_instance,
                                     double slack_ns,
                                     double load_pf,
                                     double delay_ns)
{
  const auto driver = timing_edit::StableNodeRef{
      timing_edit::ExistingNodeRef{port(std::move(driver_name))}};
  const auto sink = timing_edit::StableNodeRef{
      timing_edit::ExistingNodeRef{pin(std::move(sink_instance), "A")}};
  return BufferCandidateObservation{
      .net = timing_edit::ExistingNetRef{std::move(net_name)},
      .driver = driver,
      .candidate_pin = sink,
      .moved_sinks = {sink},
      .setup_timing = MaterializedSetupTiming{
          .arrival_ns = 0.0, .required_ns = slack_ns, .slack_ns = slack_ns},
      .upstream_resistance_kohm = 64.0,
      .candidate_load_pf = load_pf,
      .buffer_capacitance_pf = 0.001,
      .buffer_resistance_kohm = 1.0,
      .buffer_delay_ns = delay_ns,
      .buffer = bufferCell(),
      .placement = timing_edit::UnplacedInputs{},
      .rc = timing_edit::NoParasiticRcInputs{}};
}

GreedySelectionRequest request(std::vector<BufferCandidateObservation> candidates,
                               std::uint64_t seed = 0)
{
  const SelectionProfile profile = frozenSelectionProfile(ProfileId::kEngineeringSmoke);
  return GreedySelectionRequest{.profile = profile,
                                .case_id = "stage4_selector",
                                .seed = seed,
                                .batch_ordinal = 1,
                                .max_acceptances = profile.top_k,
                                .candidates = std::move(candidates),
                                .reserved_creation_tokens = {},
                                .reserved_materialized_names = {}};
}

GreedySelection select(const GreedySelectionRequest& value)
{
  auto result = GreedyBufferCandidateSelector{}.select(value);
  require(std::holds_alternative<GreedySelection>(result),
          "valid selector request returned a typed error");
  return std::get<GreedySelection>(std::move(result));
}

const CandidateEvaluationReceipt& evaluationFor(
    const GreedySelection& selection, const std::string& net_name)
{
  const auto found = std::find_if(
      selection.receipt.evaluations.begin(), selection.receipt.evaluations.end(),
      [&](const CandidateEvaluationReceipt& receipt) {
        const auto* net = std::get_if<timing_edit::ExistingNetRef>(&receipt.net);
        return net && net->baseline_name == net_name;
      });
  require(found != selection.receipt.evaluations.end(),
          "selector receipt omitted a candidate");
  return *found;
}

double stage4FixtureBufferDelay()
{
  const std::vector<std::vector<double>> axes{{0.01, 0.10}, {0.001, 0.020}};
  const csr::TimingTableModel rise{
      true, csr::TableVariable::kInputTransition,
      csr::TableVariable::kOutputCapacitance, axes,
      {0.004, 0.006, 0.006, 0.008}};
  const csr::TimingTableModel fall{
      true, csr::TableVariable::kInputTransition,
      csr::TableVariable::kOutputCapacitance, axes,
      {0.003, 0.005, 0.005, 0.007}};
  const auto profile = frozenSelectionProfile(ProfileId::kEngineeringSmoke);
  const auto rise_delay = csr::lookupCsrTimingTable(
      rise, profile.operating_point.input_slew_ns,
      profile.operating_point.output_load_pf, 1.0, 1.0);
  const auto fall_delay = csr::lookupCsrTimingTable(
      fall, profile.operating_point.input_slew_ns,
      profile.operating_point.output_load_pf, 1.0, 1.0);
  require(rise_delay && fall_delay, "production CsrTableLookup rejected fixture tables");
  return std::max(*rise_delay, *fall_delay);
}

void testProductionLookupFeedsExactPositiveGainReceipt()
{
  const double delay_ns = stage4FixtureBufferDelay();
  const auto selection = select(request({candidate(
      "fanout", "data_in", "u0", -0.25, 0.010, delay_ns)}));
  require(selection.accepted.size() == 1,
          "actual table lookup did not produce a positive accepted candidate");
  const auto& numeric = selection.accepted.front().numeric;

  const double delta = 0.010 - 0.001;
  const double benefit = 64.0 * delta;
  const double penalty = 1.0 * 0.001;
  const double benefit_minus_penalty = benefit - penalty;
  const double gain = benefit_minus_penalty - delay_ns;
  require(std::isfinite(gain) && gain > +0.0,
          "fixture operands do not yield a finite strict-positive gain");
  require(numeric.delta_capacitance_pf == delta
              && numeric.benefit_ns == benefit
              && numeric.penalty_ns == penalty
              && numeric.benefit_minus_penalty_ns == benefit_minus_penalty
              && numeric.buffer_delay_ns == delay_ns
              && numeric.gain_ns == gain,
          "selector receipt did not preserve the required binary64 evaluation order");
}

void testPermutationTopKAndStableTieOrder()
{
  const double delay = stage4FixtureBufferDelay();
  std::vector<BufferCandidateObservation> candidates;
  for (std::size_t index = 0; index < 5; ++index) {
    candidates.push_back(candidate("n" + std::to_string(index),
                                   "in" + std::to_string(index),
                                   "u" + std::to_string(index),
                                   -1.0 + static_cast<double>(index) * 0.1,
                                   0.010, delay));
  }
  const auto forward = select(request(candidates, 0));
  std::reverse(candidates.begin(), candidates.end());
  const auto reverse = select(request(candidates, 0));
  require(forward == reverse,
          "candidate input permutation changed accepted order or field receipts");
  require(forward.accepted.size() == 4,
          "engineering-smoke top-k did not limit the five critical nets to four");
  require(evaluationFor(forward, "n4").decision
              == CandidateDecision::kNonCriticalNet,
          "least-critical net was not excluded by top-k");

  auto limited_request = request(candidates, 0);
  limited_request.max_acceptances = 2;
  const auto limited = select(limited_request);
  require(limited.accepted.size() == 2,
          "remaining insertion capacity did not limit the accepted prefix");
  require(evaluationFor(limited, "n2").decision
              == CandidateDecision::kLowerRankedCandidate,
          "remaining capacity incorrectly shrank the top-k critical window");

  auto pin_b = candidate("tie", "tie_driver", "u_b", -0.5, 0.010, delay);
  auto pin_a = candidate("tie", "tie_driver", "u_a", -0.5, 0.010, delay);
  const auto tied = select(request({pin_b, pin_a}));
  require(tied.accepted.size() == 1,
          "one net accepted more than one exact-gain candidate");
  const auto a_bytes = timing_edit::canonicalStableNodeRefBytes(pin_a.candidate_pin);
  const auto b_bytes = timing_edit::canonicalStableNodeRefBytes(pin_b.candidate_pin);
  const auto& expected = std::lexicographical_compare(a_bytes.begin(), a_bytes.end(),
                                                      b_bytes.begin(), b_bytes.end())
                             ? pin_a.candidate_pin
                             : pin_b.candidate_pin;
  require(tied.accepted.front().candidate.candidate_pin == expected,
          "exact gain tie did not use stable candidate-pin bytes");
}

void testRemainingCapacityDoesNotShrinkCriticalWindow()
{
  const double delay = stage4FixtureBufferDelay();
  auto nonpositive = candidate("rank0", "d0", "u0", -1.0, 0.001,
                               delay);
  auto positive = candidate("rank1", "d1", "u1", -0.5, 0.010, delay);
  auto limited = request({positive, nonpositive});
  limited.max_acceptances = 1;
  const auto selection = select(limited);
  require(selection.accepted.size() == 1,
          "remaining capacity stopped before a later positive critical net");
  require(selection.accepted.front().candidate.net
              == timing_edit::StableNetRef{
                  timing_edit::ExistingNetRef{"rank1"}},
          "selector did not skip the first nonpositive critical net");
  require(evaluationFor(selection, "rank0").rejection_reason
              == CandidateRejectionReason::kNonPositiveGain,
          "first critical net did not retain its numeric rejection receipt");
}

void testPaperProfileTotalCapacityDoesNotViolatePerRoundTopK()
{
  const double delay = stage4FixtureBufferDelay();
  std::vector<BufferCandidateObservation> candidates;
  for (std::size_t index = 0; index < 101; ++index) {
    candidates.push_back(candidate("paper_n" + std::to_string(index),
                                   "paper_in" + std::to_string(index),
                                   "paper_u" + std::to_string(index),
                                   -static_cast<double>(index + 1), 0.010,
                                   delay));
  }
  auto paper_request = request(std::move(candidates));
  paper_request.profile
      = frozenSelectionProfile(ProfileId::kPaperProtocolTop100X10);
  paper_request.max_acceptances
      = paper_request.profile.max_total_insertions;
  const auto selection = select(paper_request);
  require(paper_request.max_acceptances > paper_request.profile.top_k,
          "paper profile fixture did not exercise total capacity above top-k");
  require(selection.accepted.size() == paper_request.profile.top_k,
          "one paper-profile round did not remain bounded by top-k");
  require(evaluationFor(selection, "paper_n0").decision
              == CandidateDecision::kNonCriticalNet,
          "candidate beyond the full top-k window was not noncritical");
}

void testStableRejectionReasonsAndStrictPositiveGate()
{
  const double delay = stage4FixtureBufferDelay();
  auto missing = candidate("missing", "d0", "u0", -0.1, 0.010, delay);
  missing.setup_timing.reset();
  auto mismatch = candidate("mismatch", "d1", "u1", -0.1, 0.010, delay);
  mismatch.setup_timing->slack_ns = -0.2;
  auto nan = candidate("nan", "d2", "u2", -0.1, 0.010, delay);
  nan.buffer_delay_ns = std::numeric_limits<double>::quiet_NaN();
  auto negative = candidate("negative", "d3", "u3", -0.1, 0.010, delay);
  negative.buffer_capacitance_pf = -0.001;
  auto zero = candidate("zero", "d4", "u4", -0.1, 1.015625, 0.0);
  zero.buffer_capacitance_pf = 1.0;

  const auto selection = select(request({missing, mismatch, nan, negative, zero}));
  require(selection.accepted.empty(), "rejected numeric candidates were accepted");
  require(evaluationFor(selection, "missing").rejection_reason
              == CandidateRejectionReason::kTimingNotMaterialized,
          "missing timing did not report the stable reason");
  require(evaluationFor(selection, "mismatch").rejection_reason
              == CandidateRejectionReason::kSlackMismatch,
          "required-arrival mismatch did not report the stable reason");
  require(evaluationFor(selection, "nan").rejection_reason
              == CandidateRejectionReason::kNonFiniteIntermediate,
          "NaN operand did not report the stable reason");
  require(evaluationFor(selection, "negative").rejection_reason
              == CandidateRejectionReason::kNegativeOperand,
          "negative operand did not report the stable reason");
  require(evaluationFor(selection, "zero").rejection_reason
              == CandidateRejectionReason::kNonPositiveGain,
          "positive-zero gain did not report the strict gate");
}

void testBinary64OverflowInfinityAndNegativeZero()
{
  const double delay = stage4FixtureBufferDelay();
  auto overflow = candidate("overflow", "d0", "u0", -0.1,
                            std::numeric_limits<double>::max(), delay);
  auto infinity = candidate("infinity", "d1", "u1", -0.1, 0.010, delay);
  infinity.buffer_delay_ns = std::numeric_limits<double>::infinity();
  auto negative_zero = candidate("negative_zero", "d2", "u2", -0.1,
                                 -0.0, +0.0);
  negative_zero.buffer_capacitance_pf = +0.0;

  const auto selection = select(request({overflow, infinity, negative_zero}));
  require(selection.accepted.empty(),
          "nonfinite or signed-zero boundary candidate was accepted");
  require(evaluationFor(selection, "overflow").rejection_reason
              == CandidateRejectionReason::kNonFiniteIntermediate,
          "finite DBL_MAX operand overflow did not report nonfinite intermediate");
  require(evaluationFor(selection, "infinity").rejection_reason
              == CandidateRejectionReason::kNonFiniteIntermediate,
          "+Inf operand did not report nonfinite intermediate");
  const auto zero_receipt = evaluationFor(selection, "negative_zero");
  require(zero_receipt.rejection_reason
              == CandidateRejectionReason::kNonPositiveGain,
          "negative-zero gain did not report the strict nonpositive gate");
  require(zero_receipt.numeric.has_value(),
          "negative-zero gain did not retain its staged numeric receipt");
  require(zero_receipt.numeric->gain_ns == 0.0
              && std::signbit(zero_receipt.numeric->gain_ns),
          "staged binary64 arithmetic did not preserve a -0 gain");
}

void testWholeBatchConflictAndReservedTokenRejection()
{
  const double delay = stage4FixtureBufferDelay();
  auto conflicting = request({candidate("n0", "shared", "u0", -0.2, 0.010, delay),
                              candidate("n1", "shared", "u1", -0.1, 0.010, delay)});
  auto result = GreedyBufferCandidateSelector{}.select(conflicting);
  require(std::holds_alternative<GreedySelectionError>(result)
              && std::get<GreedySelectionError>(result).code
                     == GreedySelectionErrorCode::kAcceptedReferenceConflict,
          "shared accepted driver did not reject the whole batch");

  auto reserved = request({candidate("n0", "d0", "u0", -0.2, 0.010, delay)}, 7);
  reserved.reserved_creation_tokens.push_back(timing_edit::CreationToken{
      .case_id = reserved.case_id,
      .profile_id = std::string(token(reserved.profile.id)),
      .seed = reserved.seed,
      .batch_ordinal = reserved.batch_ordinal,
      .accepted_ordinal = 0});
  result = GreedyBufferCandidateSelector{}.select(reserved);
  require(std::holds_alternative<GreedySelectionError>(result)
              && std::get<GreedySelectionError>(result).code
                     == GreedySelectionErrorCode::kCreationTokenConflict,
          "reserved creation token collision did not reject the whole batch");
}

std::uint64_t nextXorshift64(std::uint64_t& state) noexcept
{
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

void testFixedSeedPermutation(std::uint64_t seed)
{
  const double delay = stage4FixtureBufferDelay();
  std::vector<BufferCandidateObservation> candidates;
  for (std::size_t index = 0; index < 7; ++index) {
    candidates.push_back(candidate("property_n" + std::to_string(index),
                                   "property_in" + std::to_string(index),
                                   "property_u" + std::to_string(index),
                                   -static_cast<double>(index + 1), 0.010,
                                   delay));
  }
  const GreedySelection expected = select(request(candidates, seed));
  std::uint64_t state = seed;
  for (std::size_t remaining = candidates.size(); remaining > 1; --remaining) {
    const std::size_t selected = static_cast<std::size_t>(
        nextXorshift64(state) % static_cast<std::uint64_t>(remaining));
    std::swap(candidates[remaining - 1], candidates[selected]);
  }
  const GreedySelection actual = select(request(candidates, seed));
  require(actual == expected,
          "fixed-seed candidate permutation changed selector fields");
}

}  // namespace

int main(int argc, char* argv[])
{
  testProductionLookupFeedsExactPositiveGainReceipt();
  testPermutationTopKAndStableTieOrder();
  testRemainingCapacityDoesNotShrinkCriticalWindow();
  testPaperProfileTotalCapacityDoesNotViolatePerRoundTopK();
  testStableRejectionReasonsAndStrictPositiveGate();
  testBinary64OverflowInfinityAndNegativeZero();
  testWholeBatchConflictAndReservedTokenRejection();
  if (argc == 3 && std::string(argv[1]) == "--property-seed") {
    std::size_t consumed = 0;
    const std::string text(argv[2]);
    const std::uint64_t seed = std::stoull(text, &consumed, 0);
    require(consumed == text.size(), "property seed contains trailing text");
    testFixedSeedPermutation(seed);
  } else {
    require(argc == 1, "usage: selector-test [--property-seed U64]");
  }
  return 0;
}
