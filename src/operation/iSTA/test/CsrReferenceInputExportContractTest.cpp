// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrFingerprint.hpp"
#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "Database.hpp"
#include "ParasiticNet.hpp"
#include "ParasiticNode.hpp"
#include "ParasiticResistor.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

CsrReferenceInput requireInput(CsrReferenceInputExportResult result, const std::string& label)
{
  require(std::holds_alternative<CsrReferenceInput>(result), label + ": export returned a typed failure");
  return std::get<CsrReferenceInput>(std::move(result));
}

const NodeRecord& nodeByName(const CsrReferenceInput& input, std::string_view full_name)
{
  for (const auto& node : input.nodes)
    if (node.key.full_name == full_name)
      return node;
  throw std::runtime_error("missing exported node " + std::string(full_name));
}

const StartSeedModel& seedByName(const CsrReferenceInput& input, std::string_view full_name)
{
  const NodeId node = nodeByName(input, full_name).id;
  for (const auto& seed : input.start_seeds)
    if (seed.seed_node == node)
      return seed;
  throw std::runtime_error("missing exported seed " + std::string(full_name));
}

bool allAbsent(const OptionalLaneMatrix& matrix)
{
  return std::ranges::all_of(matrix, [](const auto& row) { return std::ranges::all_of(row, [](const auto& value) { return !value.has_value(); }); });
}

bool allPresent(const OptionalLaneMatrix& matrix)
{
  return std::ranges::all_of(matrix, [](const auto& row) { return std::ranges::all_of(row, [](const auto& value) { return value.has_value(); }); });
}

void requireIdentity(const CsrReferenceInput& input, std::uint64_t token, const std::string& label)
{
  const CsrSnapshotIdentity& identity = input.source_identity;
  require(identity.session_token == token, label + ": session token was not preserved");
  require(identity.graph_generation == 0, label + ": detached input generation must be zero");
  const std::array hashes = {identity.topology_fingerprint, identity.clock_constraint_fingerprint, identity.library_fingerprint,
                             identity.corner_fingerprint,   identity.parasitic_fingerprint,        identity.timing_input_fingerprint};
  for (const auto& hash : hashes)
    require(isLowercaseSha256(hash), label + ": identity contains a non-SHA-256 component");
  require(identity == makeReferenceInputIdentity(input, token, 0), label + ": identity differs from the unique payload helper");
}

CsrReferenceInput exportWithoutNativeWrites(CsrReferenceInputNativeFixture& fixture, const StaAnalysisContext& context, std::uint64_t token,
                                            const std::string& label)
{
  const ista::Database& database = fixture.database();
  const CsrReferenceInputRawProjection raw_before = projectCsrReferenceInputRaw(database);
  const std::vector<std::uint8_t> bytes_before = canonicalIndependentReferenceInputRawBytes(raw_before);
  CsrReferenceInput input;
  try {
    input = requireInput(exportCsrReferenceInputCandidate(database, context, token), label);
  } catch (const std::exception& error) {
    throw std::runtime_error(label + ": " + error.what());
  }
  const CsrReferenceInputRawProjection raw_after = projectCsrReferenceInputRaw(database);
  require(raw_after == raw_before, label + ": const exporter mutated the native Database");
  require(canonicalIndependentReferenceInputRawBytes(raw_after) == bytes_before, label + ": native raw bytes changed across export");
  require(validateReferenceInputForSession(input, token).empty(), label + ": exported candidate is not strict-valid");
  requireIdentity(input, token, label);
  return input;
}

void addParasitics(ista::Database& database, const std::string& source_path)
{
  ista::ParasiticNode first;
  first.set_node_name("embedded_driver");
  first.set_capacitance(0.12);
  first.set_x(-1.5);
  first.set_y(2.5);
  ista::ParasiticNode second;
  second.set_node_name("embedded_sink");
  second.set_capacitance(0.18);
  second.set_x(3.5);
  second.set_y(-4.5);
  ista::ParasiticResistor resistor;
  resistor.set_source_node("driver");
  resistor.set_sink_node("sink");
  resistor.set_resistance(8.0);
  ista::ParasiticNet net;
  net.set_net_name("embedded_div_net");
  net.set_lumped_capacitance(0.3);
  net.get_node_map().emplace("driver", std::move(first));
  net.get_node_map().emplace("sink", std::move(second));
  net.get_resistor_list().push_back(std::move(resistor));

  auto& library = database.get_parasitic_library();
  library.set_spef_file_path(source_path);
  library.set_capacitive_unit("PF");
  library.set_resistance_unit("OHM");
  library.get_net_map().emplace("div_net", std::move(net));
}

void verifyRichChain()
{
  constexpr std::uint64_t kToken = 0x1234;
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  const ReferencedLibraryCellModel focused
      = detail::projectCanonicalReferencedLibraryCell(
          fixture.database(), "DFF_X1");
  const CsrReferenceInput first = exportWithoutNativeWrites(fixture, referenceInputTestContext(), kToken, "rich chain");
  const CsrReferenceInput second = exportWithoutNativeWrites(fixture, referenceInputTestContext(), kToken, "rich chain repeat");
  require(canonicalIndependentReferenceInputBytes(first) == canonicalIndependentReferenceInputBytes(second), "rich chain repeat changed the candidate");
  require(canonicalIndependentReferenceInputBytes(first) == canonicalIndependentReferenceInputBytes(second),
          "rich chain independent wire is not deterministic");

  require(first.clocks.size() == 2, "rich chain lost raw clocks");
  require(first.clocks[0].map_key == "a_clock_key" && first.clocks[0].embedded_clock_name == "embedded_clk",
          "clock map key and embedded identity were conflated");
  require(first.clocks[0].ordered_sources.size() == 3 && first.clocks[0].ordered_sources[0] == first.clocks[0].ordered_sources[1],
          "clock source order or duplicate was lost");
  require(!first.clocks[0].ordered_sources.back().resolved_node, "unresolved clock source was fabricated");
  require(first.clocks[0].edges == std::array{-0.5, 0.0} && first.clocks[1].period == -2.0,
          "finite negative/zero raw clock values were rejected or normalized");
  require(first.port_constraints.size() == 3 && !first.port_constraints.back().resolved_node, "unresolved port constraint was not preserved");
  require(first.case_analysis_constraints.size() == 2 && !first.case_analysis_constraints.back().resolved_node, "unresolved case constraint was not preserved");

  require(first.referenced_library_cells.size() == 1 && first.referenced_library_cells[0].present, "referenced Liberty closure is incomplete");
  const auto& cell = first.referenced_library_cells[0];
  require(focused == cell, "focused cell projector differs from full ReferenceInput export");
  require(cell.cell_arcs.size() == 2 && cell.timing_checks.size() == 1, "referenced Liberty arc/check closure has the wrong shape");
  const auto& present_empty = cell.cell_arcs[0].transfer.children()[0].delay_tables[0];
  require(present_empty.present && present_empty.values.empty(), "present-empty table collapsed into missing");
  require(cell.cell_arcs[1].transfer.children()[0].delay_tables[0].axes.size() == 2, "2D Liberty table was not detached completely");
  require(cell.timing_checks[0].children()[0].check_tables[0].axes.size() == 1, "timing-check table was not detached completely");
  require(first.timing_check_endpoints.size() == 2 && first.instances[0].has_timing_checks && first.instances[1].has_timing_checks,
          "per-instance timing-check ordinal records are incomplete");

  const StartSeedModel& register_seed = seedByName(first, "ff0:CK");
  require(register_seed.availability == StartSeedAvailability::kDeferredSequential && register_seed.path_source == PathSource::kRegister,
          "reachable sequential clock did not become a structural register seed");
  require(nodeByName(first, "ff0:Q").id == register_seed.path_start_node && register_seed.crpr_node == nodeByName(first, "ff0:CK").id
              && register_seed.crpr_transition == Transition::kFall,
          "register path-start/CRPR structure differs from selected C2Q child[0]");
  require(allAbsent(register_seed.arrival) && allAbsent(register_seed.slew), "sequential register seed fabricated numeric lanes");
  for (const char* name : {"ff1:CK", "ff1:D"}) {
    const auto& seed = seedByName(first, name);
    require(seed.availability == StartSeedAvailability::kDeferredSequential && !seed.path_source && allAbsent(seed.arrival) && allAbsent(seed.slew),
            std::string(name) + ": deferred sequential-null combination is wrong");
  }
  const StartSeedModel& clock_seed = seedByName(first, "clk");
  require(clock_seed.availability == StartSeedAvailability::kMaterialized && clock_seed.path_source == PathSource::kInput && allPresent(clock_seed.arrival)
              && allPresent(clock_seed.slew) && clock_seed.clock_tag == "embedded_clk",
          "direct input-clock seed materialization or tag priority is wrong");

  require(std::ranges::all_of(first.arcs, [](const ArcRecord& arc) { return arc.output_load.has_value(); }),
          "no-SPEF export must materialize every output-load matrix");
  const auto divider = std::ranges::find(first.arcs, "div_net", [](const ArcRecord& arc) { return arc.key.base.owner_name; });
  require(divider != first.arcs.end() && divider->kind == ArcKind::kNet && divider->is_disabled, "sequential divider exact disabled mirror is absent");
  const std::vector expected_reasons = {CsrReferenceUnsupportedReason::kPropagatedClock, CsrReferenceUnsupportedReason::kClockGatingOrDivider, CsrReferenceUnsupportedReason::kCaseAnalysis};
  require(first.unsupported_reasons == expected_reasons, "rich chain support reasons are not exact-derived and sorted");
}

void verifyEmptyDiamondAndCycle()
{
  auto empty = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kEmpty);
  const CsrReferenceInput empty_input = exportWithoutNativeWrites(empty, referenceInputTestContext(), 1, "empty");
  require(empty_input.nodes.empty() && empty_input.arcs.empty() && empty_input.out_row_offsets == std::vector<std::uint64_t>{0},
          "empty Database did not produce an explicit complete empty-domain candidate");

  auto diamond = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kDiamond);
  const CsrReferenceInput diamond_input = exportWithoutNativeWrites(diamond, referenceInputTestContext(), 2, "diamond");
  require(diamond_input.nodes.size() == 4 && diamond_input.arcs.size() == 3 && !diamond_input.ordered_root_ids.empty(), "diamond fixture lost graph structure");

  auto cycle = makeReferencePropertyFixture(1);
  const CsrReferenceInput cycle_input = exportWithoutNativeWrites(cycle, referenceInputTestContext(), 3, "active cycle");
  require(cycle_input.ordered_root_ids.empty() && !cycle_input.arcs.empty(), "active-cycle fixture unexpectedly has a root");
}

void verifyParasiticAllOrNoneAndPathExclusion()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  addParasitics(fixture.database(), "first.spef");
  StaAnalysisContext context = referenceInputTestContext();
  context.rc_corner = "typical_rc";
  const CsrReferenceInput first = exportWithoutNativeWrites(fixture, context, 9, "parasitic");
  require(first.parasitics.nets.size() == 1 && first.parasitics.nets[0].nodes.size() == 2 && first.parasitics.nets[0].resistors.size() == 1,
          "raw parasitic semantic was not detached completely");
  require(std::ranges::all_of(first.arcs, [](const ArcRecord& arc) { return !arc.output_load.has_value(); }),
          "nonempty parasitics must make every output load absent");
  require(!first.unsupported_reasons.empty() && first.unsupported_reasons.front() == CsrReferenceUnsupportedReason::kParasiticNet,
          "parasitic support reason is missing or unsorted");

  fixture.database().get_parasitic_library().set_spef_file_path("renamed.spef");
  const CsrReferenceInput renamed = exportWithoutNativeWrites(fixture, context, 9, "renamed SPEF path");
  require(canonicalIndependentReferenceInputBytes(renamed) == canonicalIndependentReferenceInputBytes(first),
          "SPEF source path leaked into the detached candidate or identity");
}

void verifyImmutableSessionModels()
{
  CsrReferenceInput retained;
  {
    auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
    detail::NativeLibraryModels models(fixture.database());
    retained = requireInput(exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 71, models), "shared models");
    const auto repeated = requireInput(exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 71, models), "shared models repeat");
    for (std::size_t i = 0; i < retained.arcs.size(); ++i) {
      const auto& arc = retained.arcs[i];
      require(arc.transfer.childrenOwner() == repeated.arcs[i].transfer.childrenOwner(), "same-session export repeated a model allocation");
      if (!arc.transfer.children().empty()) {
        const auto& cell = models.cell(retained.nodes[arc.source.value()].instance_cell);
        require(std::ranges::any_of(cell.cell_arcs, [&](const auto& candidate) {
                  return candidate.transfer.childrenOwner() == arc.transfer.childrenOwner();
                }), "graph transfer did not reuse its referenced Liberty model");
      }
    }
    // Instance-only table changes must not silently use the Liberty value.
    auto& native_child = fixture.database().get_instance_map().at("ff0").get_check_arc_list().front().get_timing_arc_list().front();
    auto& native_value = native_child.get_check_table_map().begin()->second.get_value_list().front();
    const auto original_value = native_value;
    native_value += 0.25;
    const auto malformed = exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 71, models);
    require(std::holds_alternative<CsrReferenceInputExportFailure>(malformed), "instance-only check table change escaped the public boundary");
    native_value = original_value;
    auto foreign = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
    bool rejected = false;
    try {
      static_cast<void>(exportCsrReferenceInputCandidate(foreign.database(), referenceInputTestContext(), 72, models));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "foreign Database reused the session model store");
  }
  require(validateReferenceInputForSession(retained, 71).empty(), "exported models did not outlive their session");
  const auto arc = std::ranges::find_if(retained.arcs, [](const auto& value) { return !value.transfer.children().empty(); });
  require(arc != retained.arcs.end(), "model lifetime fixture is empty");
  auto editable = arc->transfer.children();
  auto& retained_alias = editable.front().time_unit_scale;
  auto detached = arc->transfer;
  detached.setChildren(editable);
  retained_alias = 9.0;
  require(detached == arc->transfer && detached.childrenOwner() != arc->transfer.childrenOwner(),
          "external mutable storage changed a frozen model");
}

void verifyIdentityAndSessionBinding()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kDiamond);
  const CsrReferenceInput baseline = exportWithoutNativeWrites(fixture, referenceInputTestContext(), 17, "session binding");

  CsrReferenceInput rebound = baseline;
  rebound.source_identity.session_token = 18;
  require(validateReferenceInput(rebound).empty(), "nonzero token substitution must remain self-valid");
  require(!validateReferenceInputForSession(rebound, 17).empty(), "bound validator accepted the previous session token");
  require(validateReferenceInputForSession(rebound, 18).empty(), "bound validator rejected the substituted session token");
  require(!validateReferenceInputForSession(rebound, 0).empty(), "bound validator accepted expected token zero");

  using HashMember = std::string CsrSnapshotIdentity::*;
  constexpr std::array<HashMember, 6> kHashes
      = {&CsrSnapshotIdentity::topology_fingerprint, &CsrSnapshotIdentity::clock_constraint_fingerprint, &CsrSnapshotIdentity::library_fingerprint,
         &CsrSnapshotIdentity::corner_fingerprint,   &CsrSnapshotIdentity::parasitic_fingerprint,        &CsrSnapshotIdentity::timing_input_fingerprint};
  for (HashMember member : kHashes) {
    CsrReferenceInput changed = baseline;
    std::string& hash = changed.source_identity.*member;
    hash[0] = hash[0] == '0' ? '1' : '0';
    require(!validateReferenceInput(changed).empty(), "self validator accepted a mutated component/composite fingerprint");
  }
}

}  // namespace

int main()
{
  try {
    verifyRichChain();
    verifyEmptyDiamondAndCycle();
    verifyParasiticAllOrNoneAndPathExclusion();
    verifyIdentityAndSessionBinding();
    verifyImmutableSessionModels();
  } catch (const std::exception& error) {
    std::cerr << "CsrReferenceInputExportContractTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
