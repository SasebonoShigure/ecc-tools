// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include "CsrModelTestMutation.hpp"
#include "CsrCpuReferenceTestFixture.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrValidatorTestFixture.hpp"

namespace ista::csr::test {
namespace {

template <typename T>
bool exactValue(const T& lhs, const T& rhs)
{
  return lhs == rhs;
}

bool exactValue(double lhs, double rhs)
{
  return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

bool mismatchAt(std::string* mismatch, const std::string& field)
{
  if (mismatch != nullptr)
    *mismatch = field;
  return false;
}

template <typename T>
bool exactVector(const std::vector<T>& lhs, const std::vector<T>& rhs)
{
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t index = 0; index < lhs.size(); ++index)
    if (!exactValue(lhs[index], rhs[index]))
      return false;
  return true;
}

template <typename T>
bool exactSoa(const OptionalSoa<T>& lhs, const OptionalSoa<T>& rhs)
{
  return exactVector(lhs.presence, rhs.presence) && exactVector(lhs.values, rhs.values);
}

bool exactClock(const ClockResultSoa& lhs, const ClockResultSoa& rhs)
{
  return exactSoa(lhs.arrivals, rhs.arrivals) && exactSoa(lhs.slews, rhs.slews) && exactSoa(lhs.predecessor_nodes, rhs.predecessor_nodes)
         && exactSoa(lhs.predecessor_arc_delays, rhs.predecessor_arc_delays) && exactSoa(lhs.predecessor_transitions, rhs.predecessor_transitions);
}

template <typename T>
OptionalSoa<T> canarySoa(T present, T absent)
{
  return {{1, 0}, {std::move(present), std::move(absent)}};
}

void mutateCanary(double& value)
{
  value = -0.0;
}

void mutateCanary(std::uint8_t& value)
{
  value = 1;
}

void mutateCanary(std::string& value)
{
  value = "mutated";
}

void mutateCanary(NodeId& value)
{
  value = NodeId{9};
}

void mutateCanary(ArcId& value)
{
  value = ArcId{9};
}

void mutateCanary(ClockTagId& value)
{
  value = ClockTagId{9};
}

void mutateCanary(Transition& value)
{
  value = Transition::kRise;
}

CsrTimingResult comparatorCanary()
{
  CsrTimingResult result;
  result.schema_version = "fixture";
  result.source_identity.session_token = 1;
  result.level_offsets = {0, 2};
  result.level_nodes = {NodeId{0}, NodeId{1}};
  result.node_to_level = {1, 1};

  result.nodes.is_clock_point = canarySoa<std::uint8_t>(1, 0);
  result.nodes.arrivals = canarySoa(1.25, 0.0);
  result.nodes.requireds = canarySoa(2.5, 0.0);
  result.nodes.slacks = canarySoa(1.25, 0.0);
  result.nodes.launches = canarySoa(0.5, 0.0);
  result.nodes.clock_names = canarySoa(std::string{"clk"}, std::string{});
  result.nodes.predecessor_nodes = canarySoa(NodeId{1}, NodeId{0});
  result.nodes.predecessor_arcs = canarySoa(ArcId{1}, ArcId{0});

  result.effective_clock.arrivals = canarySoa(0.5, 0.0);
  result.effective_clock.slews = canarySoa(0.25, 0.0);
  result.effective_clock.predecessor_nodes = canarySoa(NodeId{1}, NodeId{0});
  result.effective_clock.predecessor_arc_delays = canarySoa(0.125, 0.0);
  result.effective_clock.predecessor_transitions = canarySoa(Transition::kFall, Transition::kNone);
  result.physical_clock = result.effective_clock;
  result.data_slews = canarySoa(0.75, 0.0);

  result.paths.row_offsets = {0, 1, 2};
  result.paths.clock_tags = canarySoa(ClockTagId{1}, ClockTagId{0});
  result.paths.arrivals = canarySoa(2.0, 0.0);
  result.paths.slews = canarySoa(0.5, 0.0);
  result.paths.launches = canarySoa(0.25, 0.0);
  result.paths.start_nodes = canarySoa(NodeId{1}, NodeId{0});
  result.paths.predecessor_nodes = canarySoa(NodeId{1}, NodeId{0});
  result.paths.predecessor_arcs = canarySoa(ArcId{1}, ArcId{0});
  result.paths.predecessor_arc_delays = canarySoa(0.375, 0.0);
  result.paths.predecessor_transitions = canarySoa(Transition::kFall, Transition::kNone);
  result.paths.crpr_nodes = canarySoa(NodeId{1}, NodeId{0});
  result.paths.crpr_transitions = canarySoa(Transition::kRise, Transition::kNone);

  result.arcs.delays = canarySoa(3.0, 0.0);
  result.arcs.min_delays = canarySoa(2.0, 0.0);
  result.arcs.max_delays = canarySoa(4.0, 0.0);
  result.arcs.transition_delays = canarySoa(1.0, 0.0);
  result.arcs.input_output_delays = canarySoa(1.5, 0.0);
  result.arcs.graph_delays = canarySoa(1.75, 0.0);
  result.arcs.transition_types = canarySoa(Transition::kFall, Transition::kNone);
  result.arcs.timing_arc_row_offsets = {0, 2};
  result.arcs.timing_arc_indices = {-7, 11};
  result.arcs.timing_arc_delays = canarySoa(2.25, 0.0);
  result.arcs.timing_arc_fallbacks = canarySoa<std::uint8_t>(1, 0);
  return result;
}

template <typename Getter>
void exerciseSoaCanary(const CsrTimingResult& baseline, const std::string& field, Getter getter)
{
  CsrTimingResult changed = baseline;
  auto& presence = getter(changed).presence;
  presence.back() = presence.back() == 0 ? 1 : 0;
  requireCpuReference(!exactTimingResultEqual(baseline, changed), field + " presence mutation escaped exact comparator");

  changed = baseline;
  mutateCanary(getter(changed).values.back());
  requireCpuReference(!exactTimingResultEqual(baseline, changed), field + " absent-payload mutation escaped exact comparator");
}

void requireStrictValid(const CsrReferenceInput& input, const std::string& name)
{
  const auto errors = validateReferenceInput(input);
  if (!errors.empty())
    throw std::runtime_error(name + " is not strict-valid at " + errors.front().field + ": " + errors.front().message);
}

CsrReferenceInput materializedCellInput()
{
  CsrReferenceInput input = cellInput();
  NodeRecord input_port{
      .id = NodeId{0},
      .key = port("in"),
      .direction = PinDirection::kInput,
      .instance_cell = {},
      .net_name = "n_in",
      .is_port = true,
      .is_startpoint = true,
  };
  NodeRecord cell_input = input.nodes[0];
  cell_input.id = NodeId{1};
  cell_input.net_name = "n_in";
  cell_input.is_startpoint = false;
  NodeRecord cell_output = input.nodes[1];
  cell_output.id = NodeId{2};
  input.nodes = {input_port, cell_input, cell_output};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}, {input.nodes[1].key, NodeId{1}}, {input.nodes[2].key, NodeId{2}}};
  input.instances.front().ordered_pins = {NodeId{1}, NodeId{2}};

  ArcRecord cell_arc = input.arcs.front();
  cell_arc.id = ArcId{1};
  cell_arc.source = NodeId{1};
  cell_arc.sink = NodeId{2};
  cell_arc.native_traversal_rank = 1;
  cell_arc.output_load = outputLoad(0.0);

  ArcKeyBase net_base;
  net_base.source = input.nodes[0].key;
  net_base.sink = input.nodes[1].key;
  net_base.kind = ArcKind::kNet;
  net_base.owner_name = "n_in";
  ArcRecord net_arc;
  net_arc.id = ArcId{0};
  net_arc.key = {net_base, 0};
  net_arc.source = NodeId{0};
  net_arc.sink = NodeId{1};
  net_arc.kind = ArcKind::kNet;
  net_arc.native_traversal_rank = 0;
  net_arc.eligibility = {true, true, true, false};
  net_arc.transfer.delay = 0.0;
  net_arc.transfer.delay_min = 0.0;
  net_arc.transfer.delay_max = 0.0;
  net_arc.output_load = outputLoad(0.1);

  input.arcs = {net_arc, cell_arc};
  input.arc_ids_by_key = {{input.arcs[0].key, ArcId{0}}, {input.arcs[1].key, ArcId{1}}};
  input.out_row_offsets = {0, 1, 2, 2};
  input.out_arc_ids = {ArcId{0}, ArcId{1}};
  input.in_row_offsets = {0, 0, 1, 2};
  input.in_arc_ids = {ArcId{0}, ArcId{1}};
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids = {NodeId{2}};
  input.clock_owner_by_node = {std::nullopt, std::nullopt, std::nullopt};

  PortConstraintModel constraint;
  constraint.map_key = "in";
  constraint.embedded_port_name = "in";
  constraint.resolved_node = NodeId{0};
  constraint.input_delay = {1.0, 1.0};
  constraint.input_transition = 1.0;
  input.port_constraints = {constraint};

  StartSeedModel seed;
  seed.seed_node = NodeId{0};
  seed.path_start_node = NodeId{0};
  seed.path_source = PathSource::kInput;
  seed.clock_tag = "clk";
  for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      seed.arrival[analysis][transition] = 1.0;
      seed.slew[analysis][transition] = 1.0;
    }
  input.start_seeds = {seed};
  input.cell_pin_capacitances = {{NodeId{1}, 0.1, {}}, {NodeId{2}, 0.1, {}}};
  return input;
}

void configureCellTables(CsrReferenceInput& input)
{
  auto arc_it = std::ranges::find(input.arcs, ArcKind::kCell, &ArcRecord::kind);
  if (arc_it == input.arcs.end())
    throw std::runtime_error("CPU reference cell fixture has no cell arc");
  auto& arc = *arc_it;
  auto children = arc.transfer.children();
  auto& child = children.front();
  child.signature.arc_index = 7;
  child.signature.sense = TimingSense::kPositiveUnate;
  child.signature.trigger_transition = Transition::kNone;
  for (std::size_t output = 0; output < kTransitionCount; ++output) {
    child.signature.delay_tables[output] = {true, TableVariable::kInputTransition, TableVariable::kNone};
    child.signature.slew_tables[output] = {true, TableVariable::kInputTransition, TableVariable::kNone};
    child.delay_tables[output] = {true, TableVariable::kInputTransition, TableVariable::kNone, {{0.0, 1.0, 2.0}}, {1.0, 2.0, 4.0}};
    child.slew_tables[output] = {true, TableVariable::kInputTransition, TableVariable::kNone, {{0.0, 1.0, 2.0}}, {0.25, 0.5, 1.0}};
  }
  arc.key.base.library_arcs = {child.signature};
  arc.output_load = outputLoad(0.0);
  input.arc_ids_by_key[arc.id.value()].key = arc.key;
  arc.transfer.setChildren(children);
  input.referenced_library_cells.front().cell_arcs.front().transfer = arc.transfer;
}

}  // namespace

void requireCpuReference(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

CsrReferenceInput makeCpuReferenceNetFixture()
{
  CsrReferenceInput input = referenceInput();
  requireStrictValid(input, "CPU reference net fixture");
  return input;
}

CsrReferenceInput makeCpuReferenceCellFixture()
{
  CsrReferenceInput input = materializedCellInput();
  configureCellTables(input);
  refreshCpuReferenceIdentity(input);
  requireStrictValid(input, "CPU reference cell fixture");
  return input;
}

CsrReferenceInput makeCpuReferenceCycleFixture()
{
  CsrReferenceInput input = cycleInput();
  requireStrictValid(input, "CPU reference cycle fixture");
  return input;
}

CsrReferenceInput makeCpuReferencePropertyFixture(std::uint64_t seed)
{
  CsrReferenceInput input = makeCpuReferenceCellFixture();
  const std::size_t active_count = 1 + static_cast<std::size_t>(seed % 3);
  const bool add_disabled = (seed & 1U) != 0;
  const bool add_loop_disabled = (seed & 2U) != 0;
  const std::size_t cell_arc_count = active_count + (add_disabled ? 1 : 0) + (add_loop_disabled ? 1 : 0);
  const ArcRecord net_arc = input.arcs[0];
  const ArcRecord prototype = input.arcs[1];

  input.arcs = {net_arc};
  input.arc_ids_by_key = {{net_arc.key, net_arc.id}};
  input.referenced_library_cells.front().cell_arcs.clear();
  for (std::size_t index = 0; index < cell_arc_count; ++index) {
    ArcRecord arc = prototype;
    arc.id = ArcId{index + 1};
    arc.key.parallel_ordinal = index;
    arc.native_traversal_rank = index + 1;
    arc.native_cell_arc_ordinal = index;
    const double bias = 0.03125 * static_cast<double>(1 + (seed % 13)) + 0.5 * static_cast<double>(index);
    ista::csr::test::mutateChildren(arc.transfer, [&](auto& children) {
    for (auto& child : children)
      for (std::size_t output = 0; output < kTransitionCount; ++output) {
        for (double& value : child.delay_tables[output].values)
          value += bias;
        for (double& value : child.slew_tables[output].values)
          value += bias * 0.125;
      }
  });
    if (index >= active_count) {
      arc.eligibility = {};
      if (add_disabled && index == active_count) {
        arc.is_disabled = true;
        arc.transfer.is_disabled = true;
      } else {
        arc.is_loop_disabled = true;
      }
    }
    input.arcs.push_back(arc);
    input.arc_ids_by_key.push_back({arc.key, arc.id});
    input.referenced_library_cells.front().cell_arcs.push_back({"A", "Y", arc.transfer});
  }

  const std::size_t arc_count = input.arcs.size();
  input.out_row_offsets = {0, 1, arc_count, arc_count};
  input.out_arc_ids.clear();
  input.out_arc_ids.reserve(arc_count);
  input.in_row_offsets = {0, 0, 1, arc_count};
  input.in_arc_ids.clear();
  input.in_arc_ids.reserve(arc_count);
  for (std::size_t index = 0; index < arc_count; ++index) {
    input.out_arc_ids.push_back(ArcId{index});
    input.in_arc_ids.push_back(ArcId{index});
  }

  const double max_arrival = 0.125 * static_cast<double>(1 + seed % 17);
  const double min_arrival = 0.125 * static_cast<double>(1 + (seed / 17) % 17);
  const double slew = 0.0625 * static_cast<double>(1 + seed % 7);
  input.port_constraints.front().input_delay = {max_arrival, min_arrival};
  input.port_constraints.front().input_transition = slew;
  for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
    input.start_seeds.front().arrival[0][transition] = max_arrival;
    input.start_seeds.front().arrival[1][transition] = min_arrival;
  }
  for (auto& analysis : input.start_seeds.front().slew)
    for (auto& transition : analysis)
      transition = slew;
  refreshCpuReferenceIdentity(input);
  requireStrictValid(input, "CPU reference property fixture");
  return input;
}

void refreshCpuReferenceIdentity(CsrReferenceInput& input)
{
  input.source_identity = makeReferenceInputIdentity(input, 11, 0);
}

std::vector<std::uint8_t> cpuReferenceCanonicalBytes(const CsrReferenceInput& input)
{
  return canonicalReferenceInputBytes(input);
}

bool exactTimingResultEqual(const CsrTimingResult& lhs, const CsrTimingResult& rhs, std::string* mismatch)
{
  if (lhs.schema_version != rhs.schema_version)
    return mismatchAt(mismatch, "schema_version");
  if (!(lhs.source_identity == rhs.source_identity))
    return mismatchAt(mismatch, "source_identity");
  if (!exactVector(lhs.level_offsets, rhs.level_offsets))
    return mismatchAt(mismatch, "level_offsets");
  if (!exactVector(lhs.level_nodes, rhs.level_nodes))
    return mismatchAt(mismatch, "level_nodes");
  if (!exactVector(lhs.node_to_level, rhs.node_to_level))
    return mismatchAt(mismatch, "node_to_level");

#define CSR_EXACT_SOA(FIELD)           \
  if (!exactSoa(lhs.FIELD, rhs.FIELD)) \
  return mismatchAt(mismatch, #FIELD)
  CSR_EXACT_SOA(nodes.is_clock_point);
  CSR_EXACT_SOA(nodes.arrivals);
  CSR_EXACT_SOA(nodes.requireds);
  CSR_EXACT_SOA(nodes.slacks);
  CSR_EXACT_SOA(nodes.launches);
  CSR_EXACT_SOA(nodes.clock_names);
  CSR_EXACT_SOA(nodes.predecessor_nodes);
  CSR_EXACT_SOA(nodes.predecessor_arcs);
  if (!exactClock(lhs.effective_clock, rhs.effective_clock))
    return mismatchAt(mismatch, "effective_clock");
  if (!exactClock(lhs.physical_clock, rhs.physical_clock))
    return mismatchAt(mismatch, "physical_clock");
  CSR_EXACT_SOA(data_slews);
  if (!exactVector(lhs.paths.row_offsets, rhs.paths.row_offsets))
    return mismatchAt(mismatch, "paths.row_offsets");
  CSR_EXACT_SOA(paths.clock_tags);
  CSR_EXACT_SOA(paths.arrivals);
  CSR_EXACT_SOA(paths.slews);
  CSR_EXACT_SOA(paths.launches);
  CSR_EXACT_SOA(paths.start_nodes);
  CSR_EXACT_SOA(paths.predecessor_nodes);
  CSR_EXACT_SOA(paths.predecessor_arcs);
  CSR_EXACT_SOA(paths.predecessor_arc_delays);
  CSR_EXACT_SOA(paths.predecessor_transitions);
  CSR_EXACT_SOA(paths.crpr_nodes);
  CSR_EXACT_SOA(paths.crpr_transitions);
  CSR_EXACT_SOA(arcs.delays);
  CSR_EXACT_SOA(arcs.min_delays);
  CSR_EXACT_SOA(arcs.max_delays);
  CSR_EXACT_SOA(arcs.transition_delays);
  CSR_EXACT_SOA(arcs.input_output_delays);
  CSR_EXACT_SOA(arcs.graph_delays);
  CSR_EXACT_SOA(arcs.transition_types);
  if (!exactVector(lhs.arcs.timing_arc_row_offsets, rhs.arcs.timing_arc_row_offsets))
    return mismatchAt(mismatch, "arcs.timing_arc_row_offsets");
  if (!exactVector(lhs.arcs.timing_arc_indices, rhs.arcs.timing_arc_indices))
    return mismatchAt(mismatch, "arcs.timing_arc_indices");
  CSR_EXACT_SOA(arcs.timing_arc_delays);
  CSR_EXACT_SOA(arcs.timing_arc_fallbacks);
#undef CSR_EXACT_SOA
  return true;
}

void requireExactTimingResult(const CsrTimingResult& actual, const CsrTimingResult& expected, const std::string& name)
{
  std::string mismatch;
  if (!exactTimingResultEqual(actual, expected, &mismatch))
    throw std::runtime_error(name + " differs at " + mismatch);
}

void requireExactComputationResult(const CsrComputationResult& actual, const CsrComputationResult& expected, const std::string& name)
{
  if (actual.outcome.index() != expected.outcome.index())
    throw std::runtime_error(name + " outcome alternative differs");
  if (const auto* actual_timing = std::get_if<CsrTimingResult>(&actual.outcome)) {
    const auto* expected_timing = std::get_if<CsrTimingResult>(&expected.outcome);
    requireCpuReference(expected_timing != nullptr, name + " expected timing alternative is missing");
    requireExactTimingResult(*actual_timing, *expected_timing, name);
    return;
  }
  if (const auto* actual_support = std::get_if<CsrReferenceSupportResult>(&actual.outcome)) {
    const auto* expected_support = std::get_if<CsrReferenceSupportResult>(&expected.outcome);
    requireCpuReference(expected_support != nullptr, name + " expected support alternative is missing");
    requireCpuReference(actual_support->unsupported_reasons == expected_support->unsupported_reasons, name + " unsupported-reason payload differs");
    return;
  }
  const auto* actual_code = std::get_if<CsrComputationCode>(&actual.outcome);
  const auto* expected_code = std::get_if<CsrComputationCode>(&expected.outcome);
  requireCpuReference(actual_code != nullptr && expected_code != nullptr && *actual_code == *expected_code, name + " computation code differs");
}

void runExactTimingResultComparatorCanaries()
{
  const CsrTimingResult baseline = comparatorCanary();
  const auto reject = [&](const std::string& field, auto mutate) {
    CsrTimingResult changed = baseline;
    mutate(changed);
    requireCpuReference(!exactTimingResultEqual(baseline, changed), field + " mutation escaped exact comparator");
  };
  reject("level_offsets", [](auto& value) { ++value.level_offsets.back(); });
  reject("schema_version", [](auto& value) { value.schema_version.push_back('x'); });
  reject("source_identity.fingerprint_schema_version", [](auto& value) { value.source_identity.fingerprint_schema_version.push_back('x'); });
  reject("source_identity.session_token", [](auto& value) { ++value.source_identity.session_token; });
  reject("source_identity.graph_generation", [](auto& value) { ++value.source_identity.graph_generation; });
  reject("source_identity.topology_fingerprint", [](auto& value) { value.source_identity.topology_fingerprint.push_back('x'); });
  reject("source_identity.clock_constraint_fingerprint", [](auto& value) { value.source_identity.clock_constraint_fingerprint.push_back('x'); });
  reject("source_identity.library_fingerprint", [](auto& value) { value.source_identity.library_fingerprint.push_back('x'); });
  reject("source_identity.corner_fingerprint", [](auto& value) { value.source_identity.corner_fingerprint.push_back('x'); });
  reject("source_identity.parasitic_fingerprint", [](auto& value) { value.source_identity.parasitic_fingerprint.push_back('x'); });
  reject("source_identity.timing_input_fingerprint", [](auto& value) { value.source_identity.timing_input_fingerprint.push_back('x'); });
  reject("level_nodes", [](auto& value) { value.level_nodes.back() = NodeId{9}; });
  reject("node_to_level", [](auto& value) { ++value.node_to_level.back(); });

#define CSR_CANARY(FIELD) exerciseSoaCanary(baseline, #FIELD, [](auto& value) -> auto& { return value.FIELD; })
  CSR_CANARY(nodes.is_clock_point);
  CSR_CANARY(nodes.arrivals);
  CSR_CANARY(nodes.requireds);
  CSR_CANARY(nodes.slacks);
  CSR_CANARY(nodes.launches);
  CSR_CANARY(nodes.clock_names);
  CSR_CANARY(nodes.predecessor_nodes);
  CSR_CANARY(nodes.predecessor_arcs);
  CSR_CANARY(effective_clock.arrivals);
  CSR_CANARY(effective_clock.slews);
  CSR_CANARY(effective_clock.predecessor_nodes);
  CSR_CANARY(effective_clock.predecessor_arc_delays);
  CSR_CANARY(effective_clock.predecessor_transitions);
  CSR_CANARY(physical_clock.arrivals);
  CSR_CANARY(physical_clock.slews);
  CSR_CANARY(physical_clock.predecessor_nodes);
  CSR_CANARY(physical_clock.predecessor_arc_delays);
  CSR_CANARY(physical_clock.predecessor_transitions);
  CSR_CANARY(data_slews);
  reject("paths.row_offsets", [](auto& value) { ++value.paths.row_offsets.back(); });
  CSR_CANARY(paths.clock_tags);
  CSR_CANARY(paths.arrivals);
  CSR_CANARY(paths.slews);
  CSR_CANARY(paths.launches);
  CSR_CANARY(paths.start_nodes);
  CSR_CANARY(paths.predecessor_nodes);
  CSR_CANARY(paths.predecessor_arcs);
  CSR_CANARY(paths.predecessor_arc_delays);
  CSR_CANARY(paths.predecessor_transitions);
  CSR_CANARY(paths.crpr_nodes);
  CSR_CANARY(paths.crpr_transitions);
  CSR_CANARY(arcs.delays);
  CSR_CANARY(arcs.min_delays);
  CSR_CANARY(arcs.max_delays);
  CSR_CANARY(arcs.transition_delays);
  CSR_CANARY(arcs.input_output_delays);
  CSR_CANARY(arcs.graph_delays);
  CSR_CANARY(arcs.transition_types);
  reject("arcs.timing_arc_row_offsets", [](auto& value) { ++value.arcs.timing_arc_row_offsets.back(); });
  reject("arcs.timing_arc_indices", [](auto& value) { ++value.arcs.timing_arc_indices.back(); });
  CSR_CANARY(arcs.timing_arc_delays);
  CSR_CANARY(arcs.timing_arc_fallbacks);
#undef CSR_CANARY
}

}  // namespace ista::csr::test
