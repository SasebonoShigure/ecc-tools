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
#include "CsrModelTestMutation.hpp"
#include <algorithm>
#include <limits>
#include <string>

#include "CsrValidatorTestFixture.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

template <typename Mutate>
void rejectMutation(const CsrReferenceInput& baseline, const std::string& name, Mutate mutate)
{
  requireValid(validateReferenceInput(baseline), name + " baseline");
  CsrReferenceInput changed = baseline;
  mutate(changed);
  requireInvalid(validateReferenceInput(changed), name);
}

void testValidBaselines()
{
  const CsrReferenceInput reference = referenceInput();
  const CsrReferenceInput cell = cellInput();
  const CsrReferenceInput table = tableInput();
  const CsrReferenceInput clock = clockInput();
  require(reference.nodes.size() == 2 && reference.arcs.size() == 1 && !reference.start_seeds.empty() && !reference.endpoint_required.empty(),
          "reference fixture must remain nonvacuous");
  require(cell.arcs[0].transfer.children().size() == 1 && cell.cell_pin_capacitances.size() == 2, "cell fixture must remain nonvacuous");
  requireValid(validateReferenceInput(reference), "reference input");
  requireValid(validateReferenceInput(cell), "cell input");
  requireValid(validateReferenceInput(table), "table input");
  requireValid(validateReferenceInput(clock), "clock input");
}

void testMalformedStructureNeverThrows()
{
  const CsrReferenceInput input = referenceInput();
  rejectMutation(input, "schema", [](auto& value) { value.schema_version = "2.0.0"; });
  rejectMutation(input, "identity", [](auto& value) { value.source_identity.session_token = 0; });
  rejectMutation(input, "analysis enum", [](auto& value) { value.analysis_order[0] = static_cast<AnalysisMode>(0xff); });
  rejectMutation(input, "dense NodeId", [](auto& value) { value.nodes[0].id = NodeId{9}; });
  rejectMutation(input, "node order", [](auto& value) { std::ranges::reverse(value.nodes); });
  rejectMutation(input, "node reverse lookup", [](auto& value) { value.node_ids_by_key.pop_back(); });
  rejectMutation(input, "arc endpoint OOB", [](auto& value) { value.arcs[0].sink = NodeId{99}; });
  rejectMutation(input, "arc key endpoint", [](auto& value) { value.arcs[0].key.base.sink.full_name = "wrong"; });
  rejectMutation(input, "parallel ordinal", [](auto& value) { value.arcs[0].key.parallel_ordinal = 1; });
  rejectMutation(input, "native rank", [](auto& value) { value.arcs[0].native_traversal_rank = 1; });
  rejectMutation(input, "out adjacency cardinality", [](auto& value) { value.out_arc_ids.push_back(ArcId{0}); });
  rejectMutation(input, "out offsets empty", [](auto& value) { value.out_row_offsets.clear(); });
  rejectMutation(input, "in adjacency missing", [](auto& value) { value.in_arc_ids.clear(); });
  rejectMutation(input, "root projection", [](auto& value) { value.ordered_root_ids.clear(); });
  rejectMutation(input, "start projection", [](auto& value) { value.ordered_start_ids.clear(); });
  rejectMutation(input, "end projection", [](auto& value) { value.ordered_end_ids.clear(); });

  const CsrReferenceInput cycle = cycleInput();
  requireValid(validateReferenceInput(cycle), "same-size adjacency baseline");
  CsrReferenceInput duplicate = cycle;
  duplicate.out_arc_ids[1] = ArcId{0};
  requireViolation(validateReferenceInput(duplicate), "base_out", "ArcId is duplicated", "same-size adjacency duplicate and missing ArcId");

  const CsrReferenceInput cell = cellInput();
  rejectMutation(cell, "instance pin duplicate", [](auto& value) { value.instances[0].ordered_pins[1] = NodeId{0}; });
  rejectMutation(cell, "missing cell capacitance", [](auto& value) { value.cell_pin_capacitances.pop_back(); });
  rejectMutation(cell, "child signature mismatch", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].signature.library_name = "other.lib"; }); });
}

void testTablesConstraintsLoadsAndSeeds()
{
  const CsrReferenceInput input = referenceInput();
  rejectMutation(input, "temperature token", [](auto& value) { value.analysis_context.temperature = "-0"; });
  rejectMutation(input, "nonparasitic load", [](auto& value) { (*value.arcs[0].output_load)[0][0] = 0.25; });
  rejectMutation(input, "nonparasitic load absence", [](auto& value) { value.arcs[0].output_load.reset(); });
  rejectMutation(input, "negative input transition", [](auto& value) { value.port_constraints[0].input_transition = -0.1; });
  rejectMutation(input, "endpoint period", [](auto& value) { value.endpoint_required[0].clock_period = -1.0; });
  rejectMutation(input, "clock tag projection", [](auto& value) { value.clock_tags.clear(); });
  rejectMutation(input, "seed row count", [](auto& value) { value.start_seeds.clear(); });
  rejectMutation(input, "seed path-source presence", [](auto& value) { value.start_seeds[0].path_source.reset(); });
  rejectMutation(input, "seed path-source enum", [](auto& value) { value.start_seeds[0].path_source = static_cast<PathSource>(0xff); });
  rejectMutation(input, "seed availability enum", [](auto& value) { value.start_seeds[0].availability = static_cast<StartSeedAvailability>(0xff); });
  rejectMutation(input, "seed CRPR pair", [](auto& value) { value.start_seeds[0].crpr_node = NodeId{0}; });
  rejectMutation(input, "seed arrival/slew pair", [](auto& value) { value.start_seeds[0].slew[0][0].reset(); });

  const CsrReferenceInput cell = cellInput();
  rejectMutation(cell, "native-none arrival", [](auto& value) { value.start_seeds[0].arrival[0][0] = 0.0; });
  rejectMutation(cell, "negative capacitance", [](auto& value) { value.cell_pin_capacitances[0].default_capacitance = -1.0; });

  const CsrReferenceInput table = tableInput();
  rejectMutation(table, "table value count", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].delay_tables[0].values.pop_back(); }); });
  rejectMutation(table, "table axis order", [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { std::ranges::reverse(children[0].delay_tables[0].axes[0]); }); });
  rejectMutation(table, "table NaN",
                 [](auto& value) { ista::csr::test::mutateChildren(value.arcs[0].transfer, [&](auto& children) { children[0].delay_tables[0].values[0] = std::numeric_limits<double>::quiet_NaN(); }); });
}

void testCaseAndUnsupportedReasons()
{
  CsrReferenceInput case_input = referenceInput();
  case_input.case_analysis_constraints = {{"in", NodeId{0}, true}, {"out", NodeId{1}, false}};
  case_input.unsupported_reasons = {CsrReferenceUnsupportedReason::kCaseAnalysis};
  refreshIdentity(case_input);
  requireValid(validateReferenceInput(case_input), "case-analysis input");
  rejectMutation(case_input, "case order", [](auto& value) { std::ranges::reverse(value.case_analysis_constraints); });
  rejectMutation(case_input, "case reason exact", [](auto& value) { value.unsupported_reasons.clear(); });

  const CsrReferenceInput input = referenceInput();
  rejectMutation(input, "spurious gating reason", [](auto& value) { value.unsupported_reasons = {CsrReferenceUnsupportedReason::kClockGatingOrDivider}; });
  rejectMutation(input, "duplicate reason",
                 [](auto& value) { value.unsupported_reasons = {CsrReferenceUnsupportedReason::kCaseAnalysis, CsrReferenceUnsupportedReason::kCaseAnalysis}; });
  rejectMutation(input, "unknown reason", [](auto& value) { value.unsupported_reasons = {static_cast<CsrReferenceUnsupportedReason>(0xff)}; });
}

void testClockWaveformReachabilityAndOverlap()
{
  const CsrReferenceInput input = clockInput();
  CsrReferenceInput empty_sources = input;
  empty_sources.clocks[0].ordered_sources.clear();
  empty_sources.clock_owner_by_node = {std::nullopt, std::nullopt};
  empty_sources.arcs[0].eligibility.clock = false;
  empty_sources.unsupported_reasons.clear();
  for (auto& analysis : empty_sources.start_seeds[0].arrival)
    analysis.fill(1.0);
  empty_sources.start_seeds[0].slew[0].fill(0.2);
  empty_sources.start_seeds[0].slew[1].fill(0.2);
  empty_sources.endpoint_required[0].clock_period = 10.0;
  refreshIdentity(empty_sources);
  requireValid(validateReferenceInput(empty_sources), "raw clock with empty source list");

  rejectMutation(input, "clock resolved source mismatch", [](auto& value) { value.clocks[0].ordered_sources[0].resolved_node = NodeId{99}; });

  CsrReferenceInput equal_edges = input;
  equal_edges.clocks[0].edges = {0.0, 0.0};
  for (auto& analysis : equal_edges.start_seeds[0].arrival)
    analysis.fill(0.0);
  refreshIdentity(equal_edges);
  requireValid(validateReferenceInput(equal_edges), "finite equal raw clock edges");

  CsrReferenceInput edge_at_period = input;
  edge_at_period.clocks[0].edges[1] = edge_at_period.clocks[0].period;
  edge_at_period.start_seeds[0].arrival[0][1] = edge_at_period.clocks[0].period;
  refreshIdentity(edge_at_period);
  requireValid(validateReferenceInput(edge_at_period), "finite raw clock edge at period");

  rejectMutation(input, "clock eligibility", [](auto& value) { value.arcs[0].eligibility.clock = false; });

  CsrReferenceInput repeated_source = input;
  repeated_source.clocks[0].ordered_sources.push_back({"in", NodeId{0}});
  refreshIdentity(repeated_source);
  requireValid(validateReferenceInput(repeated_source), "ordered repeated clock source");

  CsrReferenceInput unresolved_source = input;
  unresolved_source.clocks[0].ordered_sources.push_back({"missing", std::nullopt});
  refreshIdentity(unresolved_source);
  requireValid(validateReferenceInput(unresolved_source), "ordered unresolved clock source");
  rejectMutation(unresolved_source, "unresolved source with fabricated NodeId",
                 [](auto& value) { value.clocks[0].ordered_sources.back().resolved_node = NodeId{0}; });

  CsrReferenceInput mismatched_identity = input;
  mismatched_identity.clocks[0].embedded_clock_name = "embedded";
  mismatched_identity.start_seeds[0].clock_tag = "embedded";
  mismatched_identity.clock_tags = {"clk", "embedded"};
  refreshIdentity(mismatched_identity);
  requireValid(validateReferenceInput(mismatched_identity), "clock map key and embedded identity mismatch");

  CsrReferenceInput overlap = input;
  overlap.clocks.push_back({"clk2", "clk2", {RawNodeReference{"in", NodeId{0}}}, 12.0, {0.0, 6.0}, false});
  overlap.clock_tags.push_back("clk2");
  requireInvalid(validateReferenceInput(overlap), "overlapping different clocks");

  CsrReferenceInput propagated = input;
  propagated.clocks[0].propagated = true;
  propagated.unsupported_reasons.clear();
  refreshIdentity(propagated);
  requireValid(validateReferenceInput(propagated), "propagated-clock input");
  rejectMutation(propagated, "spurious propagated rejection", [](auto& value) { value.unsupported_reasons = {CsrReferenceUnsupportedReason::kPropagatedClock}; });
}

}  // namespace

int main()
{
  testValidBaselines();
  testMalformedStructureNeverThrows();
  testTablesConstraintsLoadsAndSeeds();
  testCaseAndUnsupportedReasons();
  testClockWaveformReachabilityAndOverlap();
  return 0;
}
