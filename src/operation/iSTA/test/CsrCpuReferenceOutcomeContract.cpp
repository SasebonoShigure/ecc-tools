// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceBackendInternal.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrValidatorTestFixture.hpp"

namespace ista::csr::test {
namespace {

enum class ReplayCase
{
  kComplete,
  kCycle,
  kBadShape
};

ReplayCase replay_case = ReplayCase::kComplete;
std::size_t replay_calls = 0;

CsrFifoLevelization countingReplay(const CsrReferenceInput& input)
{
  ++replay_calls;
  if (replay_case == ReplayCase::kCycle)
    return {};
  if (replay_case == ReplayCase::kBadShape)
    return {true, {0}, {}, std::vector<std::uint64_t>(input.nodes.size(), 0)};
  if (input.nodes.empty())
    return {true, {0}, {}, {}};
  if (input.nodes.size() == 2)
    return {true, {0, 1, 2}, {NodeId{0}, NodeId{1}}, {1, 2}};
  if (input.nodes.size() == 3)
    return {true, {0, 1, 2, 3}, {NodeId{0}, NodeId{1}, NodeId{2}}, {1, 2, 3}};
  return {true, {0, input.nodes.size()}, input.ordered_root_ids, std::vector<std::uint64_t>(input.nodes.size(), 1)};
}

void expectCode(const CsrComputationResult& result, CsrComputationCode code, const std::string& name)
{
  const auto* actual = std::get_if<CsrComputationCode>(&result.outcome);
  requireCpuReference(actual != nullptr && *actual == code, name + " returned the wrong computation code");
}

void expectSupport(const CsrComputationResult& result, const CsrReferenceInput& input, const std::string& name)
{
  const auto* support = std::get_if<CsrReferenceSupportResult>(&result.outcome);
  requireCpuReference(support != nullptr, name + " did not return the support alternative");
  requireCpuReference(support->unsupported_reasons == input.unsupported_reasons, name + " changed unsupported reasons");
}

void requireStrictOutcome(const CsrReferenceInput& input, const std::string& name)
{
  const auto errors = validateReferenceInput(input);
  if (!errors.empty())
    throw std::runtime_error(name + " is not strict-valid at " + errors.front().field + ": " + errors.front().message);
}

void synchronizeCellOutcome(CsrReferenceInput& input)
{
  const auto arc = std::ranges::find(input.arcs, ArcKind::kCell, &ArcRecord::kind);
  requireCpuReference(arc != input.arcs.end(), "outcome fixture has no cell arc");
  arc->key.base.library_arcs.clear();
  for (const auto& child : arc->transfer.children)
    arc->key.base.library_arcs.push_back(child.signature);
  input.arc_ids_by_key[arc->id.value()].key = arc->key;
  input.referenced_library_cells.front().cell_arcs.front().transfer = arc->transfer;
  refreshCpuReferenceIdentity(input);
}

CsrReferenceInput parasiticUnsupportedInput()
{
  CsrReferenceInput input = referenceInput();
  input.analysis_context.rc_corner = "rc";
  for (auto& arc : input.arcs)
    arc.output_load.reset();
  input.parasitics = {"PF", "OHM", {ParasiticNetModel{"n1", "n1", 0.2, {}, {}}}};
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kParasiticNet};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput registerUnsupportedInput()
{
  CsrReferenceInput input = cellInput();
  input.instances.front().is_sequential = true;
  input.instances.front().has_timing_checks = true;
  for (auto& node : input.nodes)
    node.is_sequential = true;
  input.nodes[0].is_clock_pin = true;
  input.nodes[1].is_data_pin = true;
  auto& cell = input.referenced_library_cells.front();
  cell.is_sequential = true;
  cell.clock_pin_name = "A";
  cell.data_pin_name = "Y";
  cell.timing_checks = {LibraryTimingCheckArcModel{"A", "Y", TimingCheckKind::kSetup, 0.0, Transition::kRise, {}}};
  input.timing_check_endpoints = {TimingCheckEndpointModel{"u1", 0, TimingCheckKind::kSetup, "A", "Y", "u1:A", "u1:Y", NodeId{0}, NodeId{1}}};
  input.clocks = {ClockConstraintModel{"clk", "clk", {RawNodeReference{"u1:A", NodeId{0}}}, 10.0, {0.0, 5.0}, false}};
  input.clock_owner_by_node = {std::optional<std::string>{"clk"}, std::nullopt};
  auto& seed = input.start_seeds.front();
  seed.availability = StartSeedAvailability::kUnsupportedSequential;
  seed.path_start_node = NodeId{1};
  seed.path_source = PathSource::kRegister;
  seed.crpr_node = NodeId{0};
  seed.crpr_transition = Transition::kRise;
  for (auto& analysis : seed.arrival)
    for (auto& value : analysis)
      value.reset();
  for (auto& analysis : seed.slew)
    for (auto& value : analysis)
      value.reset();
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kSequentialInstance, CsrReferenceUnsupportedReason::kSequentialLaunch,
                               CsrReferenceUnsupportedReason::kTimingCheck, CsrReferenceUnsupportedReason::kCrpr};
  input.clock_tags = {"clk"};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput propagatedUnsupportedInput()
{
  CsrReferenceInput input = clockInput();
  input.clocks.front().propagated = true;
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kPropagatedClock, CsrReferenceUnsupportedReason::kClockTreeFanout};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput gatingUnsupportedInput(CsrReferenceInput input = cellInput())
{
  input.instances.front().is_clock_gating = true;
  input.referenced_library_cells.front().is_clock_gating = true;
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kClockGatingOrDivider};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput conditionalUnsupportedInput()
{
  CsrReferenceInput input = cellInput();
  input.arcs.front().transfer.children.front().signature.sdf_condition = "A";
  input.arcs.front().key.base.library_arcs.front().sdf_condition = "A";
  input.arc_ids_by_key.front().key = input.arcs.front().key;
  input.referenced_library_cells.front().cell_arcs.front().transfer = input.arcs.front().transfer;
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kConditionalTimingArc};
  refreshIdentity(input);
  return input;
}

CsrReferenceInput caseUnsupportedInput()
{
  CsrReferenceInput input = referenceInput();
  input.case_analysis_constraints = {{"in", NodeId{0}, true}};
  input.unsupported_reasons = {CsrReferenceUnsupportedReason::kCaseAnalysis};
  refreshIdentity(input);
  return input;
}

std::vector<CsrReferenceInput> unsupportedReasonFixtures()
{
  return {parasiticUnsupportedInput(), registerUnsupportedInput(),    propagatedUnsupportedInput(),
          gatingUnsupportedInput(),    conditionalUnsupportedInput(), caseUnsupportedInput()};
}

CsrReferenceInput presentEmptySlewOverflowInput(bool unsafe_cap)
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  auto& arc = input.arcs[1];
  auto& child = arc.transfer.children.front();
  child.time_unit_scale = 2.0;
  for (std::size_t output = 0; output < kTransitionCount; ++output) {
    child.signature.delay_tables[output] = {true, TableVariable::kNone, TableVariable::kNone};
    child.delay_tables[output] = {true, TableVariable::kNone, TableVariable::kNone, {}, {}};
  }
  const double maximum = std::numeric_limits<double>::max();
  input.port_constraints.front().input_transition = maximum;
  for (auto& analysis : input.start_seeds.front().slew)
    for (auto& value : analysis)
      value = maximum;
  if (unsafe_cap) {
    child.capacitance_unit_scale = 2.0;
    arc.output_load = outputLoad(static_cast<double>(std::numeric_limits<int>::max()));
  }
  synchronizeCellOutcome(input);
  return input;
}
CsrReferenceInput interpolationOverflowInput()
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  auto& table = input.arcs[1].transfer.children.front().delay_tables.front();
  table.values = {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), 0.0};
  synchronizeCellOutcome(input);
  return input;
}

CsrReferenceInput negativeComputedSlewInput()
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  auto& child = input.arcs[1].transfer.children.front();
  for (std::size_t output = 0; output < kTransitionCount; ++output) {
    child.signature.slew_tables[output] = {true, TableVariable::kNone, TableVariable::kNone};
    child.slew_tables[output] = {true, TableVariable::kNone, TableVariable::kNone, {}, {-1.0}};
  }
  synchronizeCellOutcome(input);
  return input;
}

CsrReferenceInput roundedOverflowInput()
{
  CsrReferenceInput input = makeCpuReferenceNetFixture();
  const double arrival = std::numeric_limits<double>::max() / 2.0;
  input.port_constraints.front().input_delay = {arrival, arrival};
  for (auto& analysis : input.start_seeds.front().arrival)
    for (auto& value : analysis)
      value = arrival;
  refreshCpuReferenceIdentity(input);
  return input;
}
CsrReferenceInput arrivalAdditionOverflowInput()
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  const double maximum = std::numeric_limits<double>::max();
  const double arrival = maximum / 2.0;
  auto& transfer = input.arcs[1].transfer;
  transfer.children.clear();
  transfer.delay = maximum;
  transfer.delay_min = maximum;
  transfer.delay_max = maximum;
  input.port_constraints.front().input_delay = {arrival, arrival};
  for (auto& analysis : input.start_seeds.front().arrival)
    for (auto& value : analysis)
      value = arrival;
  synchronizeCellOutcome(input);
  return input;
}

CsrReferenceInput singletonOverflowInput(double arrival, double output_delay)
{
  CsrReferenceInput input = referenceInput();
  input.nodes.resize(1);
  input.nodes.front().direction = PinDirection::kInout;
  input.nodes.front().is_endpoint = true;
  input.node_ids_by_key.resize(1);
  input.arcs.clear();
  input.arc_ids_by_key.clear();
  input.out_row_offsets = {0, 0};
  input.out_arc_ids.clear();
  input.in_row_offsets = {0, 0};
  input.in_arc_ids.clear();
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids = {NodeId{0}};
  input.port_constraints.resize(1);
  auto& port = input.port_constraints.front();
  port.input_delay = {arrival, arrival};
  port.output_delay = {output_delay, std::nullopt};
  port.output_load = 0.0;
  port.clock_name = "clk";
  auto& seed = input.start_seeds.front();
  for (auto& analysis : seed.arrival)
    for (auto& value : analysis)
      value = arrival;
  input.clocks = {ClockConstraintModel{"clk", "clk", {}, std::numeric_limits<double>::max(), {0.0, 0.0}, false}};
  input.clock_owner_by_node = {std::nullopt};
  input.endpoint_required = {EndpointRequiredModel{NodeId{0}, "clk", std::numeric_limits<double>::max(), {output_delay, std::nullopt}}};
  input.clock_tags = {"clk"};
  refreshIdentity(input);
  return input;
}

void testRuntimePrecedence()
{
  CsrReferenceInput malformed = makeCpuReferenceNetFixture();
  malformed.out_row_offsets.pop_back();
  replay_case = ReplayCase::kComplete;
  replay_calls = 0;
  expectCode(computeCsrCpuReferenceWithReplay(malformed, &countingReplay), CsrComputationCode::kInvariantViolation, "malformed-before-FIFO");
  requireCpuReference(replay_calls == 0, "malformed input called the FIFO seam");

  const CsrReferenceInput unsupported = clockInput();
  replay_calls = 0;
  expectSupport(computeCsrCpuReferenceWithReplay(unsupported, &countingReplay), unsupported, "support-before-FIFO");
  requireCpuReference(replay_calls == 0, "unsupported input called the FIFO seam");

  const CsrReferenceInput cycle = makeCpuReferenceCycleFixture();
  replay_case = ReplayCase::kCycle;
  replay_calls = 0;
  expectCode(computeCsrCpuReferenceWithReplay(cycle, &countingReplay), CsrComputationCode::kCycle, "supported cycle");
  requireCpuReference(replay_calls == 1, "supported cycle did not call the FIFO seam exactly once");

  const CsrReferenceInput complete = makeCpuReferenceNetFixture();
  replay_case = ReplayCase::kComplete;
  replay_calls = 0;
  const CsrComputationResult success = computeCsrCpuReferenceWithReplay(complete, &countingReplay);
  requireCpuReference(std::holds_alternative<CsrTimingResult>(success.outcome), "supported complete DAG did not return timing");
  requireCpuReference(replay_calls == 1, "supported complete DAG did not call the FIFO seam exactly once");

  replay_case = ReplayCase::kBadShape;
  replay_calls = 0;
  expectCode(computeCsrCpuReferenceWithReplay(complete, &countingReplay), CsrComputationCode::kInvariantViolation, "post-FIFO shape invariant");
  requireCpuReference(replay_calls == 1, "bad replay did not call the FIFO seam exactly once");

  const CsrReferenceInput numeric = interpolationOverflowInput();
  requireStrictOutcome(numeric, "runtime interpolation overflow");
  replay_case = ReplayCase::kComplete;
  replay_calls = 0;
  expectCode(computeCsrCpuReferenceWithReplay(numeric, &countingReplay), CsrComputationCode::kNumericFailure, "runtime interpolation overflow");
  requireCpuReference(replay_calls == 1, "numeric failure did not call the FIFO seam exactly once");
}

void testUnsupportedReasonRuntimeTable()
{
  std::vector<std::uint8_t> seen(10, 0);
  std::size_t fixture_index = 0;
  for (const CsrReferenceInput& input : unsupportedReasonFixtures()) {
    const std::string name = "unsupported reason fixture " + std::to_string(fixture_index++);
    requireStrictOutcome(input, name);
    for (CsrReferenceUnsupportedReason reason : input.unsupported_reasons)
      seen[static_cast<std::size_t>(reason)] = 1;
    replay_case = ReplayCase::kCycle;
    replay_calls = 0;
    const CsrComputationResult result = computeCsrCpuReferenceWithReplay(input, &countingReplay);
    expectSupport(result, input, name);
    requireCpuReference(replay_calls == 0, name + " called FIFO");
    requireCpuReference(validateComputationResult(input, result).empty(), name + " produced an invalid support projection");
  }
  requireCpuReference(std::ranges::all_of(seen, [](std::uint8_t value) { return value == 1; }), "runtime table did not cover all ten unsupported reasons");
}

void testRuntimeOverlapCrossProduct()
{
  CsrReferenceInput unsupported_cycle = gatingUnsupportedInput(makeCpuReferenceCycleFixture());
  requireStrictOutcome(unsupported_cycle, "unsupported plus cycle");
  replay_case = ReplayCase::kCycle;
  replay_calls = 0;
  expectSupport(computeCsrCpuReferenceWithReplay(unsupported_cycle, &countingReplay), unsupported_cycle, "unsupported plus cycle");
  requireCpuReference(replay_calls == 0, "unsupported cycle called FIFO");

  CsrReferenceInput triple = unsupported_cycle;
  triple.out_row_offsets.pop_back();
  replay_calls = 0;
  expectCode(computeCsrCpuReferenceWithReplay(triple, &countingReplay), CsrComputationCode::kInvariantViolation,
             "malformed plus unsupported plus active cycle");
  requireCpuReference(replay_calls == 0, "malformed unsupported cycle called FIFO");

  CsrReferenceInput unsupported_numeric = gatingUnsupportedInput(interpolationOverflowInput());
  requireStrictOutcome(unsupported_numeric, "unsupported plus numeric domain");
  replay_case = ReplayCase::kComplete;
  replay_calls = 0;
  expectSupport(computeCsrCpuReferenceWithReplay(unsupported_numeric, &countingReplay), unsupported_numeric, "unsupported plus numeric domain");
  requireCpuReference(replay_calls == 0, "unsupported numeric domain called FIFO");
}

void testPresentEmptyOrdering()
{
  const CsrReferenceInput malformed = presentEmptySlewOverflowInput(true);
  requireCpuReference(!validateReferenceInput(malformed).empty(), "unsafe-cap present-empty fixture was not malformed");
  replay_case = ReplayCase::kComplete;
  replay_calls = 0;
  expectCode(computeCsrCpuReferenceWithReplay(malformed, &countingReplay), CsrComputationCode::kInvariantViolation, "present-empty plus unsafe-cap ordering");
  requireCpuReference(replay_calls == 0, "malformed unsafe-cap present-empty input called FIFO");

  const CsrReferenceInput overflow = presentEmptySlewOverflowInput(false);
  requireStrictOutcome(overflow, "valid present-empty scaled-slew overflow");
  replay_calls = 0;
  expectCode(computeCsrCpuReferenceWithReplay(overflow, &countingReplay), CsrComputationCode::kNumericFailure, "valid present-empty scaled-slew overflow");
  requireCpuReference(replay_calls == 1, "valid present-empty scaled-slew overflow did not call FIFO exactly once");
}

std::vector<std::pair<std::string, CsrReferenceInput>> numericFailureFixtures()
{
  return {
      {"negative computed slew", negativeComputedSlewInput()},
      {"path rounding scale overflow", roundedOverflowInput()},
      {"arrival addition overflow", arrivalAdditionOverflowInput()},
      {"required subtraction overflow", singletonOverflowInput(1.0, -std::numeric_limits<double>::max())},
      {"slack subtraction overflow", singletonOverflowInput(-std::numeric_limits<double>::max(), 0.0)},
  };
}

void testNumericCategories()
{
  replay_case = ReplayCase::kComplete;
  for (const auto& [name, input] : numericFailureFixtures()) {
    requireStrictOutcome(input, name);
    replay_calls = 0;
    expectCode(computeCsrCpuReferenceWithReplay(input, &countingReplay), CsrComputationCode::kNumericFailure, name);
    requireCpuReference(replay_calls == 1, name + " did not call FIFO exactly once");
  }
}

void testPublicWrapperParity()
{
  CsrCpuReferenceBackend backend;
  std::vector<CsrReferenceInput> inputs{makeCpuReferenceNetFixture(), makeCpuReferenceCycleFixture(), clockInput(), presentEmptySlewOverflowInput(false)};
  CsrReferenceInput malformed = makeCpuReferenceNetFixture();
  malformed.out_row_offsets.pop_back();
  inputs.push_back(std::move(malformed));
  for (const CsrReferenceInput& input : inputs) {
    const CsrComputationResult public_result = backend.compute(input);
    const CsrComputationResult seam_result = computeCsrCpuReferenceWithReplay(input, &replayFifoLevelization);
    requireExactComputationResult(public_result, seam_result, "public/internal production-helper parity");
  }
}

void testOutcomeProjection()
{
  const CsrReferenceInput supported = makeCpuReferenceNetFixture();
  CsrComputationResult result{CsrComputationCode::kInvariantViolation};
  requireCpuReference(validateComputationResult(supported, result).empty(), "supported complete invariant projection was rejected");
  result.outcome = CsrComputationCode::kNumericFailure;
  requireCpuReference(validateComputationResult(supported, result).empty(), "supported complete numeric projection was rejected");
  result.outcome = CsrComputationCode::kCycle;
  requireCpuReference(!validateComputationResult(supported, result).empty(), "supported complete spurious cycle was accepted");
  result.outcome = CsrReferenceSupportResult{{CsrReferenceUnsupportedReason::kParasiticNet}};
  requireCpuReference(!validateComputationResult(supported, result).empty(), "supported complete support alternative was accepted");

  const CsrReferenceInput unsupported = clockInput();
  result.outcome = CsrReferenceSupportResult{unsupported.unsupported_reasons};
  requireCpuReference(validateComputationResult(unsupported, result).empty(), "exact unsupported projection was rejected");
  result.outcome = CsrComputationCode::kInvariantViolation;
  requireCpuReference(!validateComputationResult(unsupported, result).empty(), "unsupported computation code was accepted");

  CsrReferenceInput malformed = supported;
  malformed.in_row_offsets.clear();
  result.outcome = CsrComputationCode::kInvariantViolation;
  requireCpuReference(validateComputationResult(malformed, result).empty(), "malformed invariant projection was rejected");
  result.outcome = CsrReferenceSupportResult{{CsrReferenceUnsupportedReason::kParasiticNet}};
  requireCpuReference(!validateComputationResult(malformed, result).empty(), "malformed support projection was accepted");
}

}  // namespace

void runComputedNumericFailureContracts()
{
  for (const auto& [name, input] : numericFailureFixtures()) {
    requireStrictOutcome(input, name);
    expectCode(computeContractTiming(input), CsrComputationCode::kNumericFailure, name);
    const auto healthy = computeContractTiming(makeCpuReferenceNetFixture());
    requireCpuReference(std::holds_alternative<CsrTimingResult>(healthy.outcome), name + " prevented the next healthy computation");
  }
}

void runCpuReferenceOutcomeContracts()
{
  testRuntimePrecedence();
  testUnsupportedReasonRuntimeTable();
  testRuntimeOverlapCrossProduct();
  testPresentEmptyOrdering();
  testNumericCategories();
  testPublicWrapperParity();
  testOutcomeProjection();
}

}  // namespace ista::csr::test
