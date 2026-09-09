// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrReferenceValidator.hpp"

namespace ista::csr::test {
namespace {

std::size_t transitionLane(std::size_t arc, std::size_t analysis, std::size_t input)
{
  return (arc * kAnalysisCount + analysis) * kTransitionCount + input;
}

std::size_t ioLane(std::size_t arc, std::size_t analysis, std::size_t input, std::size_t output)
{
  return transitionLane(arc, analysis, input) * kTransitionCount + output;
}

std::size_t childLane(std::size_t child, std::size_t analysis, std::size_t input, std::size_t output)
{
  return ((child * kAnalysisCount + analysis) * kTransitionCount + input) * kTransitionCount + output;
}

std::size_t nodeLane(std::size_t node, std::size_t analysis, std::size_t transition)
{
  return (node * kAnalysisCount + analysis) * kTransitionCount + transition;
}

std::size_t cellArcIndex(const CsrReferenceInput& input)
{
  const auto found = std::ranges::find(input.arcs, ArcKind::kCell, &ArcRecord::kind);
  requireCpuReference(found != input.arcs.end(), "cell fixture has no cell arc");
  return static_cast<std::size_t>(std::distance(input.arcs.begin(), found));
}

void requireBits(double actual, double expected, const std::string& name)
{
  requireCpuReference(std::bit_cast<std::uint64_t>(actual) == std::bit_cast<std::uint64_t>(expected), name + " bit pattern differs");
}

const CsrTimingResult& requireTiming(const CsrReferenceInput& input, const CsrComputationResult& computation, const std::string& name)
{
  const auto* result = std::get_if<CsrTimingResult>(&computation.outcome);
  requireCpuReference(result != nullptr, name + " did not return timing");
  const auto errors = validateTimingResult(input, *result);
  requireCpuReference(errors.empty(), name + " returned a validator-rejected timing result");
  return *result;
}

void requirePresent(const OptionalSoa<double>& values, std::size_t lane, double expected, const std::string& name)
{
  requireCpuReference(lane < values.presence.size() && values.presence[lane] == 1, name + " is absent");
  requireBits(values.values[lane], expected, name);
}

void requireAbsent(const OptionalSoa<double>& values, std::size_t lane, const std::string& name)
{
  requireCpuReference(lane < values.presence.size() && values.presence[lane] == 0, name + " is present");
  requireBits(values.values[lane], 0.0, name + " absent payload");
}

TimingTableModel constantTable(double value)
{
  return {true, TableVariable::kNone, TableVariable::kNone, {}, {value}};
}

void clearTables(ChildTimingArcModel& child)
{
  child.signature.delay_tables = {};
  child.signature.slew_tables = {};
  child.delay_tables = {};
  child.slew_tables = {};
  child.check_tables = {};
}

void setDelay(ChildTimingArcModel& child, std::size_t output, TimingTableModel table)
{
  child.signature.delay_tables[output] = {table.present, table.variable1, table.variable2};
  child.delay_tables[output] = std::move(table);
}

void setSlew(ChildTimingArcModel& child, std::size_t output, TimingTableModel table)
{
  child.signature.slew_tables[output] = {table.present, table.variable1, table.variable2};
  child.slew_tables[output] = std::move(table);
}

void resetChildren(CsrReferenceInput& input, const std::vector<TimingSense>& senses)
{
  auto& transfer = input.arcs[cellArcIndex(input)].transfer;
  requireCpuReference(!transfer.children.empty(), "cell fixture has no child prototype");
  ChildTimingArcModel prototype = transfer.children.front();
  clearTables(prototype);
  transfer.children.clear();
  for (std::size_t index = 0; index < senses.size(); ++index) {
    ChildTimingArcModel child = prototype;
    child.signature.arc_index = static_cast<std::int32_t>(7 + index);
    child.signature.sense = senses[index];
    child.signature.trigger_transition = Transition::kNone;
    transfer.children.push_back(std::move(child));
  }
}

void synchronizeCell(CsrReferenceInput& input, const std::string& name)
{
  const std::size_t cell_arc = cellArcIndex(input);
  auto& arc = input.arcs[cell_arc];
  arc.is_clock_arc = arc.transfer.is_clock_arc;
  arc.is_disabled = arc.transfer.is_disabled;
  arc.key.base.library_arcs.clear();
  for (const auto& child : arc.transfer.children)
    arc.key.base.library_arcs.push_back(child.signature);
  input.arc_ids_by_key[cell_arc].key = arc.key;
  input.referenced_library_cells.front().cell_arcs.front().transfer = arc.transfer;
  refreshCpuReferenceIdentity(input);
  const auto errors = validateReferenceInput(input);
  requireCpuReference(errors.empty(), name + (errors.empty() ? "" : " invalid at " + errors.front().field));
}

CsrTimingResult runTiming(const CsrReferenceInput& input, const std::string& name)
{
  const CsrComputationResult computation = computeContractTiming(input);
  return requireTiming(input, computation, name);
}

using IoExpectations = std::array<std::optional<double>, kTransitionCount * kTransitionCount>;

void requireIo(const CsrTimingResult& result, std::size_t arc, std::size_t analysis, const IoExpectations& expected, const std::string& name)
{
  for (std::size_t input = 0; input < kTransitionCount; ++input)
    for (std::size_t output = 0; output < kTransitionCount; ++output) {
      const std::size_t index = input * kTransitionCount + output;
      const std::size_t lane = ioLane(arc, analysis, input, output);
      if (expected[index])
        requirePresent(result.arcs.input_output_delays, lane, *expected[index], name);
      else
        requireAbsent(result.arcs.input_output_delays, lane, name);
    }
}

void requireCache(const CsrTimingResult& result, std::size_t child, std::size_t analysis, std::size_t input, std::size_t output, std::optional<double> expected,
                  std::uint8_t fallback, const std::string& name)
{
  const std::size_t lane = childLane(child, analysis, input, output);
  if (!expected) {
    requireAbsent(result.arcs.timing_arc_delays, lane, name);
    requireCpuReference(result.arcs.timing_arc_fallbacks.presence[lane] == 0 && result.arcs.timing_arc_fallbacks.values[lane] == 0,
                        name + " fallback is present");
    return;
  }
  requirePresent(result.arcs.timing_arc_delays, lane, *expected, name);
  requireCpuReference(result.arcs.timing_arc_fallbacks.presence[lane] == 1 && result.arcs.timing_arc_fallbacks.values[lane] == fallback,
                      name + " fallback differs");
}

void testNetIdentity()
{
  const CsrReferenceInput input = makeCpuReferenceNetFixture();
  const CsrComputationResult outcome = computeContractTiming(input);
  const CsrTimingResult result = requireTiming(input, outcome, "net identity");
  requirePresent(result.arcs.delays, 0, 0.0, "net scalar delay");
  requirePresent(result.arcs.min_delays, 0, 0.0, "net scalar min");
  requirePresent(result.arcs.max_delays, 0, 0.0, "net scalar max");
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
      requirePresent(result.arcs.transition_delays, transitionLane(0, analysis, input_transition), 0.0, "net transition delay");
      requirePresent(result.arcs.input_output_delays, ioLane(0, analysis, input_transition, input_transition), 0.0, "net IO identity");
      requirePresent(result.arcs.graph_delays, ioLane(0, analysis, input_transition, input_transition), 0.0, "net graph identity");
      requireCpuReference(result.arcs.input_output_delays.presence[ioLane(0, analysis, input_transition, 1 - input_transition)] == 0,
                          "net nonidentity IO lane became present");
    }
  requireCpuReference(result.arcs.transition_types.presence == std::vector<std::uint8_t>({1, 1}), "net transition type presence differs");
  requireCpuReference(result.arcs.transition_types.values == std::vector<Transition>({Transition::kRise, Transition::kFall}),
                      "net transition type identity differs");
  requireCpuReference(result.arcs.timing_arc_row_offsets == std::vector<std::uint64_t>({0, 0}), "net child row is not empty");
}

void testEmptyCellFallback()
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  const std::size_t cell_arc = cellArcIndex(input);
  auto& arc = input.arcs[cell_arc];
  arc.transfer.children.clear();
  arc.transfer.delay = 2.0;
  arc.transfer.delay_min = 1.0;
  arc.transfer.delay_max = 3.0;
  arc.key.base.library_arcs.clear();
  input.arc_ids_by_key[cell_arc].key = arc.key;
  input.referenced_library_cells.front().cell_arcs.front().transfer = arc.transfer;
  refreshCpuReferenceIdentity(input);
  const CsrComputationResult outcome = computeContractTiming(input);
  const CsrTimingResult result = requireTiming(input, outcome, "empty-cell fallback");
  requirePresent(result.arcs.delays, cell_arc, 3.0, "empty-cell scalar delay");
  requirePresent(result.arcs.min_delays, cell_arc, 1.0, "empty-cell scalar min");
  requirePresent(result.arcs.max_delays, cell_arc, 3.0, "empty-cell scalar max");
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      const double expected = analysis == 0 ? 3.0 : 1.0;
      requirePresent(result.arcs.transition_delays, transitionLane(cell_arc, analysis, transition), expected, "empty-cell transition");
      requirePresent(result.arcs.input_output_delays, ioLane(cell_arc, analysis, transition, transition), expected, "empty-cell initialized IO");
      requirePresent(result.arcs.graph_delays, ioLane(cell_arc, analysis, transition, transition), expected, "empty-cell dynamic graph");
      requirePresent(result.data_slews, nodeLane(2, analysis, transition), 1.0, "empty-cell pass-through sink slew");
    }
  requireCpuReference(result.arcs.timing_arc_row_offsets == std::vector<std::uint64_t>({0, 0, 0}), "empty-cell child row is not empty");
}

void testTablePresenceAndFamilyDispatch()
{
  {
    CsrReferenceInput input = makeCpuReferenceCellFixture();
    resetChildren(input, {TimingSense::kPositiveUnate});
    synchronizeCell(input, "all-table-missing fixture");
    const CsrTimingResult result = runTiming(input, "all-table-missing");
    const std::size_t arc = cellArcIndex(input);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      requireIo(result, arc, analysis, {}, "all-table-missing IO");
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition)
        requireCache(result, 0, analysis, transition, transition, std::nullopt, 0, "all-table-missing cache");
    }
    requireCpuReference(
        result.arcs.transition_types.presence[arc * kTransitionCount] == 0 && result.arcs.transition_types.presence[arc * kTransitionCount + 1] == 0,
        "all-table-missing initialized a transition type");
  }

  {
    CsrReferenceInput input = makeCpuReferenceCellFixture();
    resetChildren(input, {TimingSense::kPositiveUnate});
    setDelay(input.arcs[cellArcIndex(input)].transfer.children.front(), 0, constantTable(2.0));
    synchronizeCell(input, "delay-only fixture");
    const CsrTimingResult result = runTiming(input, "delay-only missing-fall");
    const std::size_t arc = cellArcIndex(input);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      requireIo(result, arc, analysis, {2.0, std::nullopt, std::nullopt, std::nullopt}, "delay-only IO");
      requirePresent(result.arcs.graph_delays, ioLane(arc, analysis, 0, 0), 2.0, "delay-only graph");
      requireCache(result, 0, analysis, 0, 0, 2.0, 0, "delay-only cache");
      requirePresent(result.data_slews, nodeLane(2, analysis, 0), 1.0, "delay-only sink slew");
      requireAbsent(result.data_slews, nodeLane(2, analysis, 1), "delay-only missing-fall slew");
    }
  }

  {
    CsrReferenceInput input = makeCpuReferenceCellFixture();
    resetChildren(input, {TimingSense::kPositiveUnate});
    setSlew(input.arcs[cellArcIndex(input)].transfer.children.front(), 1, constantTable(0.25));
    synchronizeCell(input, "slew-only fixture");
    const CsrTimingResult result = runTiming(input, "slew-only missing-rise");
    const std::size_t arc = cellArcIndex(input);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      requireIo(result, arc, analysis, {std::nullopt, std::nullopt, std::nullopt, 1.0}, "slew-only IO");
      requirePresent(result.arcs.graph_delays, ioLane(arc, analysis, 1, 1), 1.0, "slew-only graph");
      requireCache(result, 0, analysis, 1, 1, std::nullopt, 0, "slew-only cache");
      requireAbsent(result.data_slews, nodeLane(2, analysis, 0), "slew-only missing-rise slew");
      requirePresent(result.data_slews, nodeLane(2, analysis, 1), 0.25, "slew-only sink slew");
    }
  }

  {
    CsrReferenceInput input = makeCpuReferenceCellFixture();
    resetChildren(input, {TimingSense::kPositiveUnate});
    input.arcs[cellArcIndex(input)].transfer.children.front().check_tables[0] = constantTable(0.5);
    synchronizeCell(input, "check-only fixture");
    const CsrTimingResult result = runTiming(input, "check-only");
    const std::size_t arc = cellArcIndex(input);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      requireIo(result, arc, analysis, {1.0, std::nullopt, std::nullopt, std::nullopt}, "check-only IO");
      requirePresent(result.arcs.graph_delays, ioLane(arc, analysis, 0, 0), 1.0, "check-only graph fallback");
      requireCache(result, 0, analysis, 0, 0, std::nullopt, 0, "check-only cache");
      requirePresent(result.data_slews, nodeLane(2, analysis, 0), 1.0, "check-only pass-through slew");
    }
  }

  {
    CsrReferenceInput input = makeCpuReferenceCellFixture();
    resetChildren(input, {TimingSense::kPositiveUnate});
    auto& child = input.arcs[cellArcIndex(input)].transfer.children.front();
    setDelay(child, 0, {true, TableVariable::kNone, TableVariable::kNone, {}, {}});
    setDelay(child, 1, {true, TableVariable::kNone, TableVariable::kNone, {}, {}});
    synchronizeCell(input, "present-empty fixture");
    const CsrTimingResult result = runTiming(input, "present-empty");
    const std::size_t arc = cellArcIndex(input);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      requireIo(result, arc, analysis, {0.0, std::nullopt, std::nullopt, 0.0}, "present-empty IO");
      requireCache(result, 0, analysis, 0, 0, 0.0, 0, "present-empty rise cache");
      requireCache(result, 0, analysis, 1, 1, 0.0, 0, "present-empty fall cache");
    }
  }
}

struct SenseCase
{
  const char* name;
  std::vector<TimingSense> senses;
  std::vector<double> delays;
  IoExpectations expected;
};

void testSenseSelectionMatrix()
{
  const std::array cases{
      SenseCase{"positive", {TimingSense::kPositiveUnate}, {2.0}, {2.0, std::nullopt, std::nullopt, 2.0}},
      SenseCase{"none-positive-fallback", {TimingSense::kNoneUsesPositiveFallback}, {3.0}, {3.0, std::nullopt, std::nullopt, 3.0}},
      SenseCase{"negative", {TimingSense::kNegativeUnate}, {4.0}, {std::nullopt, 4.0, 4.0, std::nullopt}},
      SenseCase{"non-unate", {TimingSense::kNonUnate}, {5.0}, {5.0, 5.0, 5.0, 5.0}},
      SenseCase{"mixed", {TimingSense::kPositiveUnate, TimingSense::kNegativeUnate}, {2.0, 3.0}, {2.0, 3.0, 3.0, 2.0}},
      SenseCase{"exact-none-negative", {TimingSense::kNoneUsesPositiveFallback, TimingSense::kNegativeUnate}, {5.0, 7.0}, {5.0, 7.0, 7.0, 5.0}},
  };
  for (const auto& test : cases) {
    CsrReferenceInput input = makeCpuReferenceCellFixture();
    resetChildren(input, test.senses);
    auto& children = input.arcs[cellArcIndex(input)].transfer.children;
    for (std::size_t child = 0; child < children.size(); ++child)
      for (std::size_t output = 0; output < kTransitionCount; ++output)
        setDelay(children[child], output, constantTable(test.delays[child]));
    synchronizeCell(input, test.name);
    const CsrTimingResult result = runTiming(input, test.name);
    const std::size_t arc = cellArcIndex(input);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
      requireIo(result, arc, analysis, test.expected, test.name);

    if (test.senses.size() == 2 && test.senses.front() == TimingSense::kNoneUsesPositiveFallback)
      for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition)
        for (std::size_t output = 0; output < kTransitionCount; ++output) {
          const bool diagonal = input_transition == output;
          requireCache(result, 0, 0, input_transition, output, diagonal ? std::optional<double>{5.0} : std::nullopt, 0, "none child post-relation filter");
          requireCache(result, 1, 0, input_transition, output, diagonal ? std::nullopt : std::optional<double>{7.0}, 0, "negative child post-relation filter");
        }
  }
}

void testNonconstantChildInitAndDynamic()
{
  const CsrReferenceInput input = makeCpuReferenceCellFixture();
  const std::size_t cell_arc = cellArcIndex(input);
  const CsrComputationResult outcome = computeContractTiming(input);
  const CsrTimingResult result = requireTiming(input, outcome, "nonconstant child");
  requirePresent(result.arcs.delays, cell_arc, 1.0, "child immutable scalar delay");
  requirePresent(result.arcs.min_delays, cell_arc, 1.0, "ordinary child scalar min");
  requirePresent(result.arcs.max_delays, cell_arc, 1.0, "child scalar max");
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t input_transition = 0; input_transition < kTransitionCount; ++input_transition) {
      requirePresent(result.arcs.input_output_delays, ioLane(cell_arc, analysis, input_transition, input_transition), 1.0, "ordinary child initialized IO");
      requirePresent(result.arcs.graph_delays, ioLane(cell_arc, analysis, input_transition, input_transition), 2.0, "ordinary child dynamic graph");
      const std::size_t cache = childLane(0, analysis, input_transition, input_transition);
      requirePresent(result.arcs.timing_arc_delays, cache, 2.0, "ordinary child dynamic cache");
      requireCpuReference(result.arcs.timing_arc_fallbacks.presence[cache] == 1 && result.arcs.timing_arc_fallbacks.values[cache] == 0,
                          "ordinary child dynamic cache did not clear fallback");
    }
  requireCpuReference(result.arcs.timing_arc_row_offsets == std::vector<std::uint64_t>({0, 0, 1}), "ordinary child row differs");
  const std::size_t type_lane = cell_arc * kTransitionCount;
  requireCpuReference(result.arcs.transition_types.presence[type_lane] == 1 && result.arcs.transition_types.presence[type_lane + 1] == 1,
                      "ordinary non-clock cell did not initialize both input transitions");
  requireCpuReference(
      result.arcs.transition_types.values[type_lane] == Transition::kRise && result.arcs.transition_types.values[type_lane + 1] == Transition::kFall,
      "ordinary non-clock cell transition mapping differs");
}

void testClockTriggerTriStateAndMissingScaffold()
{
  struct TriggerCase
  {
    const char* name;
    Transition trigger;
    Transition later_trigger;
    std::size_t active;
  };
  const std::array cases{TriggerCase{"clock-rise", Transition::kRise, Transition::kFall, 0}, TriggerCase{"clock-fall", Transition::kFall, Transition::kRise, 1},
                         TriggerCase{"clock-none-default-rise", Transition::kNone, Transition::kFall, 0}};
  for (const auto& test : cases) {
    CsrReferenceInput input = makeCpuReferenceCellFixture();
    resetChildren(input, {TimingSense::kPositiveUnate, TimingSense::kPositiveUnate});
    const std::size_t arc = cellArcIndex(input);
    auto& transfer = input.arcs[arc].transfer;
    transfer.is_clock_arc = true;
    transfer.children[0].signature.trigger_transition = test.trigger;
    transfer.children[1].signature.trigger_transition = test.later_trigger;
    for (std::size_t output = 0; output < kTransitionCount; ++output) {
      setDelay(transfer.children[0], output, constantTable(2.0));
      setDelay(transfer.children[1], output, constantTable(4.0));
    }
    synchronizeCell(input, test.name);
    const CsrTimingResult result = runTiming(input, test.name);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      const double selected = analysis == 0 ? 4.0 : 2.0;
      IoExpectations expected{};
      expected[test.active * kTransitionCount + test.active] = selected;
      expected[test.active * kTransitionCount + (1 - test.active)] = 1.0;
      requireIo(result, arc, analysis, expected, test.name);
      requirePresent(result.arcs.graph_delays, ioLane(arc, analysis, test.active, test.active), selected, "clock selected graph");
      requirePresent(result.arcs.graph_delays, ioLane(arc, analysis, test.active, 1 - test.active), 1.0, "clock cross fallback graph");
      requireCache(result, 0, analysis, test.active, test.active, 2.0, 0, "clock first-child cache");
      requireCache(result, 1, analysis, test.active, test.active, 4.0, 0, "clock later-child cache");
      for (std::size_t child = 0; child < transfer.children.size(); ++child)
        requireCache(result, child, analysis, test.active, 1 - test.active, std::nullopt, 0, "clock filtered cache");
      requirePresent(result.arcs.transition_delays, transitionLane(arc, analysis, 1 - test.active), 0.0, "clock missing-trigger scaffold");
    }
    const std::size_t active_type = arc * kTransitionCount + test.active;
    const Transition min_winner = test.active == 0 ? Transition::kFall : Transition::kRise;
    requireCpuReference(result.arcs.transition_types.presence[active_type] == 1 && result.arcs.transition_types.values[active_type] == min_winner,
                        "clock active transition type was not overwritten by the Min winner");
    const std::size_t missing_type = arc * kTransitionCount + 1 - test.active;
    requireCpuReference(result.arcs.transition_types.presence[missing_type] == 0 && result.arcs.transition_types.values[missing_type] == Transition::kNone,
                        "clock missing trigger gained a transition type");
  }
}

void testTwoDimensionalLoadKAT()
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  resetChildren(input, {TimingSense::kPositiveUnate});
  const std::size_t arc = cellArcIndex(input);
  TimingTableModel table{true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, 1.0}, {0.0, 1.0}}, {0.0, 4.0, 8.0, 12.0}};
  setDelay(input.arcs[arc].transfer.children.front(), 0, table);
  setDelay(input.arcs[arc].transfer.children.front(), 1, table);

  ArcOutputLoadMatrix load{};
  load[0] = {0.25, 0.5};
  load[1] = {0.75, 1.0};
  OptionalLaneMatrix capacitance{};
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition)
      capacitance[analysis][transition] = load[analysis][transition];
  input.cell_pin_capacitances.front().transition_capacitance = capacitance;
  input.referenced_library_cells.front().ports.front().transition_capacitance = capacitance;
  input.nodes[2].net_name = input.nodes[1].net_name;
  input.arcs.front().output_load = load;
  input.arcs[arc].output_load = load;
  synchronizeCell(input, "2x2 load fixture");

  const CsrTimingResult result = runTiming(input, "2x2 load");
  requirePresent(result.arcs.delays, arc, 2.0, "2x2 immutable scalar");
  requirePresent(result.arcs.max_delays, arc, 2.0, "2x2 max scalar");
  requirePresent(result.arcs.min_delays, arc, 3.0, "2x2 min scalar");
  const std::array initial{std::array{1.0, 2.0}, std::array{3.0, 4.0}};
  const std::array dynamic{std::array{9.0, 10.0}, std::array{11.0, 12.0}};
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      const std::size_t lane = ioLane(arc, analysis, transition, transition);
      requirePresent(result.arcs.input_output_delays, lane, initial[analysis][transition], "2x2 initialized IO");
      requirePresent(result.arcs.graph_delays, lane, dynamic[analysis][transition], "2x2 dynamic graph");
      requireCache(result, 0, analysis, transition, transition, dynamic[analysis][transition], 0, "2x2 dynamic cache");
    }
}

void testRawNearTieWinnersAndLoserCaches()
{
  constexpr double low = 2.0;
  constexpr double high = 0x1.000004p+1;
  requireCpuReference(high > low && high - low < 1.0e-6, "raw near-tie fixture is not inside the graph epsilon");
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  resetChildren(input, {TimingSense::kPositiveUnate, TimingSense::kPositiveUnate});
  const std::size_t arc = cellArcIndex(input);
  auto& children = input.arcs[arc].transfer.children;
  for (std::size_t output = 0; output < kTransitionCount; ++output) {
    setDelay(children[0], output, constantTable(low));
    setDelay(children[1], output, constantTable(high));
  }
  synchronizeCell(input, "raw near-tie fixture");
  const CsrTimingResult result = runTiming(input, "raw near-tie");
  requirePresent(result.arcs.delays, arc, high, "near-tie scalar max");
  requirePresent(result.arcs.max_delays, arc, high, "near-tie max winner");
  requirePresent(result.arcs.min_delays, arc, low, "near-tie min winner");
  for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
    requirePresent(result.arcs.input_output_delays, ioLane(arc, 0, transition, transition), high, "near-tie initialized max");
    requirePresent(result.arcs.input_output_delays, ioLane(arc, 1, transition, transition), low, "near-tie initialized min");
    requirePresent(result.arcs.graph_delays, ioLane(arc, 0, transition, transition), high, "near-tie dynamic max");
    requirePresent(result.arcs.graph_delays, ioLane(arc, 1, transition, transition), low, "near-tie dynamic min");
    requireCache(result, 0, 0, transition, transition, low, 0, "max loser cache updated and equal fallback replaced");
    requireCache(result, 1, 0, transition, transition, high, 0, "max winner cache");
    requireCache(result, 0, 1, transition, transition, low, 0, "min winner cache");
    requireCache(result, 1, 1, transition, transition, high, 0, "min loser cache updated and equal fallback replaced");
  }
}

void testUnreachableChildRetainsFallback()
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  input.arcs.front().is_loop_disabled = true;
  input.arcs.front().eligibility = {};
  input.ordered_root_ids = {NodeId{0}, NodeId{1}};
  synchronizeCell(input, "unreachable fixture");
  const CsrTimingResult result = runTiming(input, "unreachable child");
  const std::size_t arc = cellArcIndex(input);
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      requireCache(result, 0, analysis, transition, transition, 1.0, 1, "unreachable initialized cache");
      requireAbsent(result.arcs.graph_delays, ioLane(arc, analysis, transition, transition), "unreachable graph");
    }
  }
}

void testIgnoredNoSpefPayloadIsolation()
{
  CsrReferenceInput baseline_input = makeCpuReferenceCellFixture();
  CsrReferenceInput ignored_input = baseline_input;
  auto& arc = ignored_input.arcs[cellArcIndex(ignored_input)];
  auto& child = arc.transfer.children.front();
  child.slew_derate = 3.0;
  child.slew_lower_threshold_pct = {0.1, 0.2};
  child.slew_upper_threshold_pct = {0.8, 0.9};
  child.input_threshold_pct = {0.4, 0.6};
  child.output_threshold_pct = {0.45, 0.55};
  arc.transfer.is_clear_or_preset = true;
  synchronizeCell(ignored_input, "ignored library payload fixture");
  CsrTimingResult baseline = runTiming(baseline_input, "ignored library baseline");
  CsrTimingResult ignored = runTiming(ignored_input, "ignored library payload");
  ignored.source_identity = baseline.source_identity;
  requireExactTimingResult(ignored, baseline, "ignored library payload isolation");

  CsrReferenceInput net_baseline_input = makeCpuReferenceNetFixture();
  CsrReferenceInput net_load_input = net_baseline_input;
  net_load_input.port_constraints[1].output_load = 0.75;
  net_load_input.arcs.front().output_load = ArcOutputLoadMatrix{};
  for (auto& analysis : *net_load_input.arcs.front().output_load)
    analysis.fill(0.75);
  refreshCpuReferenceIdentity(net_load_input);
  const auto errors = validateReferenceInput(net_load_input);
  requireCpuReference(errors.empty(), "ignored no-SPEF load fixture is not strict-valid");
  CsrTimingResult net_baseline = runTiming(net_baseline_input, "ignored no-SPEF load baseline");
  CsrTimingResult net_load = runTiming(net_load_input, "ignored no-SPEF load payload");
  net_load.source_identity = net_baseline.source_identity;
  requireExactTimingResult(net_load, net_baseline, "ignored no-SPEF load isolation");
}

void testDisabledChildRetainsFallback()
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  const std::size_t cell_arc = cellArcIndex(input);
  input.arcs[cell_arc].is_disabled = true;
  input.arcs[cell_arc].eligibility = {};
  input.arcs[cell_arc].transfer.is_disabled = true;
  input.ordered_root_ids = {NodeId{0}, NodeId{2}};
  synchronizeCell(input, "disabled fixture");
  const CsrTimingResult result = runTiming(input, "disabled child");
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      requireCache(result, 0, analysis, transition, transition, 1.0, 1, "disabled initialized cache");
      requireAbsent(result.arcs.graph_delays, ioLane(cell_arc, analysis, transition, transition), "disabled graph");
    }
  }
}

}  // namespace

void runCpuReferenceArcContracts()
{
  testNetIdentity();
  testEmptyCellFallback();
  testTablePresenceAndFamilyDispatch();
  testSenseSelectionMatrix();
  testNonconstantChildInitAndDynamic();
  testClockTriggerTriStateAndMissingScaffold();
  testTwoDimensionalLoadKAT();
  testRawNearTieWinnersAndLoserCaches();
  testUnreachableChildRetainsFallback();
  testDisabledChildRetainsFallback();
  testIgnoredNoSpefPayloadIsolation();
}

}  // namespace ista::csr::test
