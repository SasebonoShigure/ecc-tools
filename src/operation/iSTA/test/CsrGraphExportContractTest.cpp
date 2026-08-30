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
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrGraphEncoding.hpp"
#include "CsrGraphValidator.hpp"
#include "CsrNativeGraphFixture.hpp"
#include "CsrNativeGraphTestSupport.hpp"
#include "Database.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

NodeKey port(std::string name)
{
  return NodeKey{NodeKind::kPort, std::move(name), {}, {}};
}

ArcKey parallelKey(std::uint64_t ordinal)
{
  ArcKeyBase base;
  base.source = port("aa");
  base.sink = port("b");
  base.kind = ArcKind::kNet;
  base.owner_name = "shared";
  return ArcKey{std::move(base), ordinal};
}

NativeGraphNodeProjection portProjection(std::string name, bool is_start, bool is_end)
{
  NativeGraphNodeProjection projection;
  projection.key = port(std::move(name));
  projection.direction = PinDirection::kInout;
  projection.net_name = "shared";
  projection.is_port = true;
  projection.is_startpoint = is_start;
  projection.is_endpoint = is_end;
  return projection;
}

NativeGraphProjection handwrittenParallelProjection()
{
  const ArcKey first = parallelKey(0);
  const ArcKey second = parallelKey(1);
  NativeGraphProjection expected;
  // Canonical strings are length-prefixed, so the one-byte name sorts first.
  expected.nodes = {portProjection("b", false, true), portProjection("aa", true, false)};
  expected.arcs = {NativeGraphArcProjection{first, 0, false, false, false}, NativeGraphArcProjection{second, 1, false, false, false}};
  expected.base_out = {{port("b"), {}}, {port("aa"), {second, first}}};
  expected.base_in = {{port("b"), {second, first}}, {port("aa"), {}}};
  expected.ordered_roots = {port("aa")};
  expected.ordered_starts = {port("aa")};
  expected.ordered_ends = {port("b")};
  return expected;
}

const CsrGraphSnapshot& requireSnapshot(const CsrGraphExportResult& result, const std::string& label)
{
  const auto* snapshot = std::get_if<CsrGraphSnapshot>(&result);
  require(snapshot != nullptr, label + ": export failed");
  return *snapshot;
}

const NativeGraphProjection& requireProjection(const NativeGraphProjectionResult& result, const std::string& label)
{
  const auto* projection = std::get_if<NativeGraphProjection>(&result);
  require(projection != nullptr, label + ": decode failed");
  return *projection;
}

void verifyRoundTrip(NativeGraphFixtureCase fixture_case, const std::string& label, bool expect_cycle = false)
{
  auto fixture = makeNativeGraphFixture(fixture_case);
  const auto direct_before = captureDirectNativeGraphProjection(fixture.database());
  const auto before_bytes = canonicalIndependentNativeGraphProjectionBytes(direct_before);

  const CsrGraphExportResult first = exportCsrGraphSnapshot(fixture.database());
  const CsrGraphSnapshot& snapshot = requireSnapshot(first, label);
  require(validateCsrGraphSnapshot(snapshot).empty(), label + ": exported snapshot is invalid");
  const NativeGraphProjectionResult decoded_result = decodeNativeGraphProjection(snapshot);
  const NativeGraphProjection& decoded = requireProjection(decoded_result, label);
  require(decoded == direct_before, label + ": direct projection differs from decoded snapshot");
  require(canonicalNativeGraphProjectionBytes(decoded) == before_bytes, label + ": production and independent encoders differ");

  const auto direct_after = captureDirectNativeGraphProjection(fixture.database());
  require(direct_after == direct_before, label + ": exporter mutated Database");
  require(canonicalIndependentNativeGraphProjectionBytes(direct_after) == before_bytes, label + ": Database bytes changed after export");

  const CsrGraphExportResult second = exportCsrGraphSnapshot(fixture.database());
  require(requireSnapshot(second, label + " repeat") == snapshot, label + ": repeated export is not deterministic");

  const CsrNativeGraphOracle native_oracle = readNativeGraphOracle(fixture.database());
  const IndependentFifoOracle fifo = runIndependentFifoOracle(fixture.database());
  require(fifo.has_active_cycle == expect_cycle, label + ": independent FIFO cycle result is wrong");
  require(native_oracle == fifo.oracle, label + ": native order/levels differ from independent FIFO");
  require(canonicalNativeGraphOracleBytes(native_oracle) == canonicalIndependentNativeGraphOracleBytes(native_oracle), label + ": oracle encoders differ");
}

void verifyHandwrittenLiteral()
{
  auto fixture = makeNativeGraphFixture(NativeGraphFixtureCase::kParallel);
  const NativeGraphProjection direct = captureDirectNativeGraphProjection(fixture.database());
  const NativeGraphProjection literal = handwrittenParallelProjection();
  require(direct == literal, "parallel direct projection differs from handwritten literal");

  const auto snapshot = requireSnapshot(exportCsrGraphSnapshot(fixture.database()), "parallel literal");
  const auto decoded = requireProjection(decodeNativeGraphProjection(snapshot), "parallel literal");
  require(decoded == literal, "parallel decoded projection differs from handwritten literal");
  require(snapshot.nodes.at(0).key == port("b") && snapshot.nodes.at(0).id == NodeId{0}, "NodeId must follow canonical bytes rather than native map order");
  require(snapshot.out_arc_ids.size() == 2 && snapshot.out_arc_ids.at(0) == ArcId{1} && snapshot.out_arc_ids.at(1) == ArcId{0},
          "native adjacency order must remain opposite canonical ArcId order");

  auto mutation = literal;
  mutation.nodes.at(0).net_name = "other";
  require(mutation != literal, "node-field mutation did not change equality");
  mutation = literal;
  mutation.arcs.at(0).native_traversal_rank = 9;
  require(mutation != literal, "arc-field mutation did not change equality");
  mutation = literal;
  mutation.base_out.at(1).ordered_arcs.clear();
  require(mutation != literal, "adjacency mutation did not change equality");
  mutation = literal;
  mutation.ordered_starts.clear();
  require(mutation != literal, "ordered-collection mutation did not change equality");
}

void verifyArcIdDiffersFromRawRank()
{
  auto fixture = makeNativeGraphFixture(NativeGraphFixtureCase::kDiamond);
  const auto snapshot = requireSnapshot(exportCsrGraphSnapshot(fixture.database()), "diamond rank");
  bool found_difference = false;
  for (const auto& arc : snapshot.arcs) {
    found_difference = found_difference || arc.id.value() != arc.native_traversal_rank;
  }
  require(found_difference, "diamond must distinguish canonical ArcId from raw traversal rank");
}

void verifyBoundSignature()
{
  auto fixture = makeNativeGraphFixture(NativeGraphFixtureCase::kBoundSignature);
  const auto projection = captureDirectNativeGraphProjection(fixture.database());
  const NativeGraphArcProjection* cell_arc = nullptr;
  for (const auto& arc : projection.arcs) {
    if (arc.key.base.kind == ArcKind::kCell) {
      cell_arc = &arc;
    }
  }
  require(cell_arc != nullptr, "bound signature fixture lacks a cell arc");
  require(cell_arc->key.base.library_source_port == "A" && cell_arc->key.base.library_sink_port == "Y", "bound library ports were not preserved");
  require(cell_arc->key.base.library_arcs.size() == 1, "bound child signature count is wrong");
  const auto& child = cell_arc->key.base.library_arcs.front();
  require(
      child.library_name == "slow.lib" && child.arc_index == -7 && child.sense == TimingSense::kNegativeUnate && child.trigger_transition == Transition::kRise,
      "bound child scalar signature is wrong");
  require(child.delay_tables.at(0) == TimingTableSignature{true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance},
          "bound delay table signature is wrong");
  require(child.slew_tables.at(1) == TimingTableSignature{true, TableVariable::kConstrainedTransition, TableVariable::kNone},
          "bound slew table signature is wrong");
}

void verifyUnboundNormalization()
{
  auto fixture = makeNativeGraphFixture(NativeGraphFixtureCase::kUnboundFallback);
  const ista::Arc& raw = fixture.database().get_arc_list().at(0);
  require(raw.get_library_source_port().empty() && raw.get_library_sink_port().empty() && raw.get_timing_cell_arc() == nullptr,
          "unbound fixture raw identity is not empty");
  const auto projection = captureDirectNativeGraphProjection(fixture.database());
  const ArcKeyBase& key = projection.arcs.at(0).key.base;
  require(key.library_source_port == "A" && key.library_sink_port == "Y" && key.library_arcs.empty(),
          "unbound fallback was not normalized to endpoint pin names");
  const auto decoded = requireProjection(decodeNativeGraphProjection(requireSnapshot(exportCsrGraphSnapshot(fixture.database()), "unbound")), "unbound");
  require(decoded == projection, "unbound normalized projection did not round-trip");
}

void verifyFlagsAndCycle()
{
  auto flags = makeNativeGraphFixture(NativeGraphFixtureCase::kFlags);
  const auto snapshot = requireSnapshot(exportCsrGraphSnapshot(flags.database()), "flags");
  require(snapshot.arcs.size() == 2, "flags fixture arc count is wrong");
  require(snapshot.arcs.at(0).is_clock_arc || snapshot.arcs.at(1).is_clock_arc, "clock flag was not exported");
  require(snapshot.arcs.at(0).is_disabled || snapshot.arcs.at(1).is_disabled, "disabled flag was not exported");
  require(snapshot.arcs.at(0).is_loop_disabled || snapshot.arcs.at(1).is_loop_disabled, "loop-disabled flag was not exported");
  require(snapshot.ordered_root_ids.size() == 2, "inactive overlays must not contribute to indegree");

  auto cycle = makeNativeGraphFixture(NativeGraphFixtureCase::kActiveCycle);
  const auto cycle_snapshot = requireSnapshot(exportCsrGraphSnapshot(cycle.database()), "active cycle");
  require(cycle_snapshot.ordered_root_ids.empty(), "active cycle unexpectedly has a root");
  require(validateCsrGraphSnapshot(cycle_snapshot).empty(), "active cycle structural snapshot is invalid");
}

}  // namespace

int main()
{
  try {
    const std::array cases = {NativeGraphFixtureCase::kEmpty,
                              NativeGraphFixtureCase::kSingleton,
                              NativeGraphFixtureCase::kChain,
                              NativeGraphFixtureCase::kDiamond,
                              NativeGraphFixtureCase::kMultiRoot,
                              NativeGraphFixtureCase::kParallel,
                              NativeGraphFixtureCase::kFlags,
                              NativeGraphFixtureCase::kSparseAdjacency,
                              NativeGraphFixtureCase::kUnboundFallback,
                              NativeGraphFixtureCase::kBoundSignature,
                              NativeGraphFixtureCase::kUtf8};
    for (std::size_t index = 0; index < cases.size(); ++index) {
      verifyRoundTrip(cases[index], "fixture " + std::to_string(index));
    }
    verifyRoundTrip(NativeGraphFixtureCase::kActiveCycle, "active cycle", true);
    verifyHandwrittenLiteral();
    verifyArcIdDiffersFromRawRank();
    verifyBoundSignature();
    verifyUnboundNormalization();
    verifyFlagsAndCycle();
  } catch (const std::exception& error) {
    std::cerr << "CsrGraphExportContractTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
