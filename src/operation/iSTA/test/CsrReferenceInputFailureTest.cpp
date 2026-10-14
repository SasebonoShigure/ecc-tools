// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

#include "Arc.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "Database.hpp"
#include "ParasiticNet.hpp"
#include "ParasiticNode.hpp"
#include "ParasiticResistor.hpp"
#include "TimingCell.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingTable.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void requireFailure(CsrReferenceInputExportResult result, CsrCaptureCode expected, const std::string& label)
{
  const auto* failure = std::get_if<CsrReferenceInputExportFailure>(&result);
  require(failure != nullptr, label + ": exporter returned a partial/success candidate");
  require(failure->code == expected, label + ": exporter returned the wrong typed failure code");
}

CsrReferenceInput requireInput(CsrReferenceInputExportResult result, const std::string& label)
{
  require(std::holds_alternative<CsrReferenceInput>(result), label + ": valid control export failed");
  return std::get<CsrReferenceInput>(std::move(result));
}

StaAnalysisContext rcContext()
{
  StaAnalysisContext context = referenceInputTestContext();
  context.rc_corner = "typical_rc";
  return context;
}

void addParasitic(ista::Database& database, double capacitance, bool add_units = true)
{
  ista::ParasiticNode source;
  source.set_node_name("source");
  source.set_capacitance(capacitance);
  ista::ParasiticNode sink;
  sink.set_node_name("sink");
  sink.set_capacitance(0.1);
  ista::ParasiticResistor resistor;
  resistor.set_source_node("source");
  resistor.set_sink_node("sink");
  resistor.set_resistance(1.0);
  ista::ParasiticNet net;
  net.set_net_name("div_net");
  net.set_lumped_capacitance(0.2);
  net.get_node_map().emplace("source", std::move(source));
  net.get_node_map().emplace("sink", std::move(sink));
  net.get_resistor_list().push_back(std::move(resistor));

  auto& library = database.get_parasitic_library();
  library.set_spef_file_path("fixture.spef");
  if (add_units) {
    library.set_capacitive_unit("PF");
    library.set_resistance_unit("OHM");
  }
  library.get_net_map().emplace("div_net", std::move(net));
}

void verifyPreDatabaseRejection()
{
  auto malformed = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kMalformedAdjacency);
  bool token_threw = false;
  try {
    static_cast<void>(exportCsrReferenceInputCandidate(malformed.database(), referenceInputTestContext(), 0));
  } catch (const std::invalid_argument&) {
    token_threw = true;
  }
  require(token_threw, "token zero did not throw before reading a malformed Database");

  StaAnalysisContext bad_context = referenceInputTestContext();
  bad_context.temperature = "-0";
  requireFailure(exportCsrReferenceInputCandidate(malformed.database(), bad_context, 1), CsrCaptureCode::kInvalidAnalysisContext, "bad context pre-Database");
  bad_context = {"", "25", "__no_rc_corner__"};
  requireFailure(exportCsrReferenceInputCandidate(malformed.database(), bad_context, 1), CsrCaptureCode::kInvalidAnalysisContext, "empty context pre-Database");
}

void verifyMalformedFailures()
{
  auto adjacency = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kMalformedAdjacency);
  const auto adjacency_result = exportCsrReferenceInputCandidate(adjacency.database(), referenceInputTestContext(), 1);
  const auto* adjacency_failure = std::get_if<CsrReferenceInputExportFailure>(&adjacency_result);
  const CsrReferenceInputExportFailure expected_adjacency_failure{CsrCaptureCode::kMalformedTopology, CsrReferenceInputExportPhase::kTopology, {}};
  require(adjacency_failure && *adjacency_failure == expected_adjacency_failure,
          "malformed adjacency changed its topology failure or claimed completed fingerprints");

  auto library_identity = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  library_identity.database().get_timing_library().get_cell_map().at("DFF_X1").set_library_name("");
  requireFailure(exportCsrReferenceInputCandidate(library_identity.database(), referenceInputTestContext(), 1), CsrCaptureCode::kMalformedTopology,
                 "missing referenced library identity");

  auto parasitic_units = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  addParasitic(parasitic_units.database(), 0.1, false);
  requireFailure(exportCsrReferenceInputCandidate(parasitic_units.database(), rcContext(), 1), CsrCaptureCode::kMalformedTopology, "missing parasitic units");

  auto unknown_arc = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  unknown_arc.database().get_arc_list().front().set_type(static_cast<ista::ArcType>(0xff));
  requireFailure(exportCsrReferenceInputCandidate(unknown_arc.database(), referenceInputTestContext(), 1), CsrCaptureCode::kMalformedTopology,
                 "unknown native arc kind");
}

void verifyTableFailures()
{
  auto bad_shape = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  auto& present_empty
      = bad_shape.database().get_timing_library().get_cell_map().at("DFF_X1").get_cell_arc_list().at(0).get_timing_arc_list().at(0).get_delay_table_map().at(
          ista::TransType::kRise);
  present_empty.set_value_list({0.1, 0.2});
  requireFailure(exportCsrReferenceInputCandidate(bad_shape.database(), referenceInputTestContext(), 1), CsrCaptureCode::kInvalidTableShape,
                 "0D table value cardinality");

  auto nonfinite_table = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  nonfinite_table.database()
      .get_timing_library()
      .get_cell_map()
      .at("DFF_X1")
      .get_cell_arc_list()
      .at(0)
      .get_timing_arc_list()
      .at(0)
      .get_delay_table_map()
      .at(ista::TransType::kRise)
      .set_value_list({std::numeric_limits<double>::infinity()});
  requireFailure(exportCsrReferenceInputCandidate(nonfinite_table.database(), referenceInputTestContext(), 1), CsrCaptureCode::kIllegalNumeric,
                 "nonfinite table value");
}

void verifyFailureFingerprintPrefix()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  const auto control = requireInput(exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 1), "failure prefix control");
  auto& table = fixture.database().get_timing_library().get_cell_map().at("DFF_X1")
                    .get_cell_arc_list().at(0).get_timing_arc_list().at(0).get_delay_table_map().at(ista::TransType::kRise);
  table.set_value_list({0.1, 0.2});
  auto exported = exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 1);
  const auto* failure = std::get_if<CsrReferenceInputExportFailure>(&exported);
  require(failure && failure->code == CsrCaptureCode::kInvalidTableShape
              && failure->failed_phase == CsrReferenceInputExportPhase::kLibrary,
          "failure prefix did not preserve the typed error and phase");
  CsrCurrentFingerprints expected;
  expected.topology_fingerprint = control.source_identity.topology_fingerprint;
  expected.clock_constraint_fingerprint = control.source_identity.clock_constraint_fingerprint;
  require(failure->current_fingerprints == expected, "failure prefix changed completed component identities or claimed unfinished components");
}

void verifyNumericFailures()
{
  auto clock = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  clock.database().get_timing_constraint().get_clock_map().at("a_clock_key").set_period(std::numeric_limits<double>::quiet_NaN());
  requireFailure(exportCsrReferenceInputCandidate(clock.database(), referenceInputTestContext(), 1), CsrCaptureCode::kIllegalNumeric, "nonfinite raw clock");

  auto load = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  auto& output = load.database().get_timing_constraint().get_port_constraint_map().at("out");
  output.set_load(-0.1);
  requireFailure(exportCsrReferenceInputCandidate(load.database(), referenceInputTestContext(), 1), CsrCaptureCode::kIllegalNumeric, "negative port load");

  auto parasitic = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  addParasitic(parasitic.database(), -0.1);
  requireFailure(exportCsrReferenceInputCandidate(parasitic.database(), rcContext(), 1), CsrCaptureCode::kIllegalNumeric, "negative parasitic capacitance");

  auto converted_load = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  converted_load.database().get_timing_library().get_cell_map().at("DFF_X1").get_cell_arc_list().at(0).get_timing_arc_list().at(0).set_cap_unit_scale(1.0e30);
  requireFailure(exportCsrReferenceInputCandidate(converted_load.database(), referenceInputTestContext(), 1), CsrCaptureCode::kIllegalNumeric,
                 "converted output-load overflow");
}

void verifyParasiticSentinelFailure()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  addParasitic(fixture.database(), 0.1);
  requireFailure(exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 7), CsrCaptureCode::kInvalidAnalysisContext,
                 "nonempty SPEF with no-RC sentinel");

  const CsrReferenceInput control = requireInput(exportCsrReferenceInputCandidate(fixture.database(), rcContext(), 7), "nonempty SPEF control");
  require(validateReferenceInputForSession(control, 7).empty(), "nonempty SPEF control is not strict-valid");
}

void verifyNativeClockOverlap()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kDiamond);
  auto& clocks = fixture.database().get_timing_constraint().get_clock_map();
  auto overlap = clocks.at("diamond_key");
  overlap.set_clock_name("zz_overlap");
  clocks.emplace("zz_overlap", std::move(overlap));
  requireFailure(exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 5),
                 CsrCaptureCode::kMalformedTopology, "native overlapping clock owners");
}

void verifyArtificialOverlapIsPureOnly()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kDiamond);
  CsrReferenceInput input = requireInput(exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 5), "overlap baseline");
  const NodeId root = std::ranges::find(input.nodes, std::string{"root"}, [](const NodeRecord& node) { return node.key.full_name; })->id;
  input.clocks.push_back({"zz_overlap", "zz_overlap", {RawNodeReference{"root", root}}, 1.0, {0.0, 0.5}, false});
  input.clock_tags.push_back("zz_overlap");
  const auto violations = validateReferenceInputIntrinsic(input);
  const bool found = std::ranges::any_of(violations, [](const CsrContractViolation& violation) {
    return violation.field == "clock_owner_by_node" && violation.message.find("different clock map keys") != std::string::npos;
  });
  require(found, "artificial different-clock overlap was not rejected by the pure intrinsic validator");
}

}  // namespace

int main()
{
  try {
    verifyPreDatabaseRejection();
    verifyMalformedFailures();
    verifyTableFailures();
    verifyFailureFingerprintPrefix();
    verifyNumericFailures();
    verifyParasiticSentinelFailure();
    verifyNativeClockOverlap();
    verifyArtificialOverlapIsPureOnly();
  } catch (const std::exception& error) {
    std::cerr << "CsrReferenceInputFailureTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
