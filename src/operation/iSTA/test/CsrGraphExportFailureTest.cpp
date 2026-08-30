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
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrGraphValidator.hpp"
#include "CsrNativeGraphFixture.hpp"
#include "Database.hpp"
#include "Pin.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
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

std::string rawWitness(const ista::Database& database)
{
  std::ostringstream stream;
  const auto append_tables = [&](char family, const auto& tables) {
    for (const auto& [transition, table] : tables) {
      stream << family << static_cast<int>(transition) << static_cast<int>(table.get_variable_type1()) << static_cast<int>(table.get_variable_type2());
    }
  };
  for (const auto& [name, pin] : database.get_pin_map()) {
    stream << "P" << name << '\0' << pin.get_full_name() << '\0' << pin.get_pin_name() << '\0' << pin.get_instance_name() << '\0' << pin.get_net_name() << '\0'
           << static_cast<int>(pin.get_direction()) << pin.get_is_port();
  }
  for (const auto& [name, instance] : database.get_instance_map()) {
    stream << "I" << name << '\0' << instance.get_instance_name() << '\0' << instance.get_cell_name() << '\0';
    for (const std::string& pin : instance.get_pin_name_list()) {
      stream << pin << '\0';
    }
    stream << instance.get_clock_pin_name() << '\0' << instance.get_data_pin_name() << '\0' << instance.get_output_pin_name() << '\0'
           << instance.get_is_sequential() << instance.get_is_clock_gating() << instance.get_check_arc_list().size();
  }
  for (const auto& [name, net] : database.get_net_map()) {
    stream << "N" << name << '\0' << net.get_net_name() << '\0';
  }
  for (const ista::Arc& arc : database.get_arc_list()) {
    stream << "A" << arc.get_source_pin() << '\0' << arc.get_sink_pin() << '\0' << arc.get_owner_name() << '\0' << arc.get_library_source_port() << '\0'
           << arc.get_library_sink_port() << '\0' << static_cast<int>(arc.get_type()) << arc.get_is_clock_arc() << arc.get_is_disable_arc()
           << arc.get_is_loop_disable() << reinterpret_cast<std::uintptr_t>(arc.get_timing_cell_arc());
  }
  for (const auto& [name, row] : database.get_outgoing_arc_list_map()) {
    stream << "O" << name << '\0';
    for (std::size_t value : row) {
      stream << value << ',';
    }
  }
  for (const auto& [name, row] : database.get_incoming_arc_list_map()) {
    stream << "Q" << name << '\0';
    for (std::size_t value : row) {
      stream << value << ',';
    }
  }
  for (const std::string& name : database.get_start_point_list()) {
    stream << "S" << name << '\0';
  }
  for (const std::string& name : database.get_end_point_list()) {
    stream << "E" << name << '\0';
  }
  for (const std::string& name : database.get_timing_order_list()) {
    stream << "T" << name << '\0';
  }
  for (const auto& [name, point] : database.get_timing_point_map()) {
    stream << "L" << name << '\0' << point.get_level();
  }
  for (const auto& [name, cell] : database.get_timing_library().get_cell_map()) {
    stream << "C" << name << '\0' << cell.get_cell_name() << '\0' << cell.get_cell_arc_list().size();
    for (const auto& cell_arc : cell.get_cell_arc_list()) {
      stream << cell_arc.get_source_port() << '\0' << cell_arc.get_sink_port() << '\0' << cell_arc.get_is_timing_graph_arc()
             << cell_arc.get_timing_arc_list().size();
      for (const auto& child : cell_arc.get_timing_arc_list()) {
        stream << child.get_library_name() << '\0' << child.get_arc_idx() << static_cast<int>(child.get_sense())
               << static_cast<int>(child.get_trigger_trans_type()) << static_cast<int>(child.get_check_trans_type()) << child.get_delay_table_map().size()
               << child.get_slew_table_map().size() << child.get_check_table_map().size();
        append_tables('D', child.get_delay_table_map());
        append_tables('W', child.get_slew_table_map());
        append_tables('K', child.get_check_table_map());
      }
    }
  }
  return stream.str();
}

template <typename Mutator>
void expectMalformed(std::string_view label, NativeGraphFixtureCase fixture_case, Mutator mutate)
{
  auto fixture = makeNativeGraphFixture(fixture_case);
  mutate(fixture.database());
  const std::string before = rawWitness(fixture.database());
  const CsrGraphExportResult result = exportCsrGraphSnapshot(fixture.database());
  const auto* failure = std::get_if<CsrGraphExportFailure>(&result);
  require(failure != nullptr, std::string(label) + ": malformed Database produced a snapshot");
  require(failure->code == CsrCaptureCode::kMalformedTopology, std::string(label) + ": wrong failure code");
  require(rawWitness(fixture.database()) == before, std::string(label) + ": failed export mutated Database");
}

ista::Pin makeInstancePin(const std::string& full_name, const std::string& local_name)
{
  ista::Pin pin;
  pin.set_full_name(full_name);
  pin.set_pin_name(local_name);
  pin.set_instance_name("fallback");
  pin.set_direction(ista::PinDirection::kInput);
  return pin;
}

void verifyMissingAndMapOwnerFailures()
{
  expectMalformed("missing pin", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_pin_map().erase("in"); });
  expectMalformed("missing instance", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_instance_map().erase("u0"); });
  expectMalformed("missing timing point", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_timing_point_map().erase("u0:A"); });
  expectMalformed("pin map/internal mismatch", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_pin_map().at("in").set_full_name("other"); });
  expectMalformed("instance map/internal mismatch", NativeGraphFixtureCase::kChain,
                  [](ista::Database& db) { db.get_instance_map().at("u0").set_instance_name("other"); });
  expectMalformed("net map/internal mismatch", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_net_map().at("n_in").set_net_name("other"); });
  expectMalformed("cell map/internal mismatch", NativeGraphFixtureCase::kChain,
                  [](ista::Database& db) { db.get_timing_library().get_cell_map().at("BUF_X1").set_cell_name("other"); });
  expectMalformed("ghost role owner", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_instance_map().at("u0").set_clock_pin_name("ghost"); });
}

void verifyAdjacencyFailures()
{
  expectMalformed("unknown adjacency key", NativeGraphFixtureCase::kChain,
                  [](ista::Database& db) { db.get_outgoing_arc_list_map().emplace("ghost", std::vector<std::size_t>{}); });
  expectMalformed("stale adjacency index", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_outgoing_arc_list_map().at("in").at(0) = 99; });
  expectMalformed("duplicate adjacency arc", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_outgoing_arc_list_map().at("in").push_back(0); });
  expectMalformed("missing adjacency arc", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_incoming_arc_list_map().at("u0:A").clear(); });
  expectMalformed("wrong adjacency row", NativeGraphFixtureCase::kChain, [](ista::Database& db) {
    db.get_outgoing_arc_list_map().at("in").clear();
    db.get_outgoing_arc_list_map().at("u0:A").insert(db.get_outgoing_arc_list_map().at("u0:A").begin(), 0);
  });
}

void addForeignCell(ista::Database& database)
{
  ista::TimingCellArc foreign_arc;
  foreign_arc.set_source_port("A");
  foreign_arc.set_sink_port("Y");
  ista::TimingCell foreign_cell;
  foreign_cell.set_cell_name("FOREIGN");
  foreign_cell.get_cell_arc_list().push_back(std::move(foreign_arc));
  database.get_timing_library().get_cell_map().emplace("FOREIGN", std::move(foreign_cell));
  auto& stored = database.get_timing_library().get_cell_map().at("FOREIGN").get_cell_arc_list().at(0);
  database.get_arc_list().at(1).set_timing_cell_arc(&stored);
}

void verifyCellIdentityFailures()
{
  expectMalformed("foreign library pointer", NativeGraphFixtureCase::kChain, addForeignCell);
  expectMalformed("non-graph library pointer", NativeGraphFixtureCase::kChain,
                  [](ista::Database& db) { db.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list().at(0).set_is_timing_graph_arc(false); });
  expectMalformed("partial raw ports", NativeGraphFixtureCase::kChain, [](ista::Database& db) { db.get_arc_list().at(1).set_library_source_port(""); });
  expectMalformed("null with nonempty raw ports", NativeGraphFixtureCase::kChain,
                  [](ista::Database& db) { db.get_arc_list().at(1).set_timing_cell_arc(nullptr); });
  expectMalformed("invalid null fallback", NativeGraphFixtureCase::kChain, [](ista::Database& db) {
    auto& arc = db.get_arc_list().at(1);
    arc.set_timing_cell_arc(nullptr);
    arc.set_library_source_port("");
    arc.set_library_sink_port("");
  });
  expectMalformed("unknown arc enum", NativeGraphFixtureCase::kChain,
                  [](ista::Database& db) { db.get_arc_list().at(0).set_type(static_cast<ista::ArcType>(99)); });
}

void verifySignatureFailures()
{
  expectMalformed("unknown transition map key", NativeGraphFixtureCase::kBoundSignature, [](ista::Database& db) {
    auto& child = db.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list().at(0).get_timing_arc_list().at(0);
    child.get_delay_table_map().emplace(ista::TransType::kNone, ista::TimingTable{});
  });
  expectMalformed("graph child check table", NativeGraphFixtureCase::kBoundSignature, [](ista::Database& db) {
    auto& child = db.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list().at(0).get_timing_arc_list().at(0);
    child.get_check_table_map().emplace(ista::TransType::kRise, ista::TimingTable{});
  });
  expectMalformed("table variable2 without variable1", NativeGraphFixtureCase::kBoundSignature, [](ista::Database& db) {
    auto& table = db.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list().at(0).get_timing_arc_list().at(0).get_delay_table_map().at(
        ista::TransType::kRise);
    table.set_variable_type1(ista::TimingTableVariableType::kNone);
  });
}

void verifyFallbackCartesianFailure()
{
  expectMalformed("incomplete fallback Cartesian product", NativeGraphFixtureCase::kUnboundFallback, [](ista::Database& db) {
    db.get_pin_map().emplace("fallback:B", makeInstancePin("fallback:B", "B"));
    db.get_timing_point_map().emplace("fallback:B", ista::TimingPoint{});
    db.get_instance_map().at("fallback").get_pin_name_list().insert(db.get_instance_map().at("fallback").get_pin_name_list().begin() + 1, "fallback:B");
  });
}

CsrGraphSnapshot snapshotFor(NativeGraphFixtureCase fixture_case)
{
  auto fixture = makeNativeGraphFixture(fixture_case);
  const auto result = exportCsrGraphSnapshot(fixture.database());
  const auto* snapshot = std::get_if<CsrGraphSnapshot>(&result);
  require(snapshot != nullptr, "valid snapshot fixture failed to export");
  return *snapshot;
}

CsrGraphSnapshot validSnapshot()
{
  return snapshotFor(NativeGraphFixtureCase::kChain);
}

bool hasField(const std::vector<CsrGraphContractViolation>& violations, std::string_view field)
{
  return std::ranges::any_of(violations, [&](const auto& violation) { return violation.field == field; });
}

void requireExactViolation(const std::vector<CsrGraphContractViolation>& violations, std::string_view field, std::string_view message, std::string_view label)
{
  const bool found = std::ranges::any_of(violations, [&](const auto& violation) {
    return violation.code == CsrCaptureCode::kMalformedTopology && violation.field == field && violation.message == message;
  });
  require(found, std::string(label) + ": missing exact malformed violation for " + std::string(field) + " with message '" + std::string(message) + "'");
}

std::size_t firstFamily(const std::vector<CsrGraphContractViolation>& violations, std::string_view prefix)
{
  const auto found = std::ranges::find_if(violations, [&](const auto& violation) { return violation.field.starts_with(prefix); });
  require(found != violations.end(), "missing violation family " + std::string(prefix));
  return static_cast<std::size_t>(std::distance(violations.begin(), found));
}

void verifySnapshotValidation()
{
  CsrGraphSnapshot snapshot = validSnapshot();
  snapshot.schema.clear();
  snapshot.nodes.at(0).id = NodeId{99};
  snapshot.instances.at(0).ordered_pins.push_back(NodeId{99});
  snapshot.arcs.at(0).native_traversal_rank = 99;
  snapshot.out_row_offsets.clear();
  snapshot.ordered_start_ids.push_back(snapshot.ordered_start_ids.front());
  const auto violations = validateCsrGraphSnapshot(snapshot);
  require(!violations.empty() && violations.front().field == "schema", "schema must be the first violation");
  const std::size_t node = firstFamily(violations, "nodes");
  const std::size_t instance = firstFamily(violations, "instances");
  const std::size_t arc = firstFamily(violations, "arcs");
  const std::size_t adjacency = firstFamily(violations, "out_row_offsets");
  const std::size_t ordered = firstFamily(violations, "ordered_start_ids");
  require(node < instance && instance < arc && arc < adjacency && adjacency < ordered,
          "validator stages are not in schema/node/instance/arc/adjacency/ordered order");

  const auto decoded = decodeNativeGraphProjection(snapshot);
  const auto* failure = std::get_if<CsrGraphExportFailure>(&decoded);
  require(failure != nullptr && failure->code == violations.front().code, "decoder did not return the first validator code without a partial projection");

  snapshot = validSnapshot();
  snapshot.nodes.at(1).key = snapshot.nodes.at(0).key;
  snapshot.node_ids_by_key.at(1).key = snapshot.nodes.at(0).key;
  const auto node_collision = validateCsrGraphSnapshot(snapshot);
  require(hasField(node_collision, "nodes[1].key"), "NodeKey collision was not reported");
  require(std::ranges::any_of(node_collision, [](const auto& violation) { return violation.code == CsrCaptureCode::kSemanticKeyCollision; }),
          "NodeKey collision used the wrong code");

  auto parallel = makeNativeGraphFixture(NativeGraphFixtureCase::kParallel);
  const auto result = exportCsrGraphSnapshot(parallel.database());
  snapshot = std::get<CsrGraphSnapshot>(result);
  snapshot.arcs.at(1).key = snapshot.arcs.at(0).key;
  snapshot.arc_ids_by_key.at(1).key = snapshot.arcs.at(0).key;
  const auto arc_collision = validateCsrGraphSnapshot(snapshot);
  require(hasField(arc_collision, "arcs[1].key"), "ArcKey collision was not reported");
}

std::size_t nodeRow(const CsrGraphSnapshot& snapshot, std::string_view full_name)
{
  const auto found = std::ranges::find_if(snapshot.nodes, [&](const auto& node) { return node.key.full_name == full_name; });
  require(found != snapshot.nodes.end(), "missing snapshot node " + std::string(full_name));
  return static_cast<std::size_t>(std::distance(snapshot.nodes.begin(), found));
}

std::size_t boundArcRow(const CsrGraphSnapshot& snapshot)
{
  const auto found = std::ranges::find_if(snapshot.arcs, [](const auto& arc) { return arc.kind == ArcKind::kCell && !arc.key.base.library_arcs.empty(); });
  require(found != snapshot.arcs.end(), "missing bound cell arc in snapshot fixture");
  return static_cast<std::size_t>(std::distance(snapshot.arcs.begin(), found));
}

void refreshArcLookup(CsrGraphSnapshot& snapshot, std::size_t row)
{
  snapshot.arc_ids_by_key.at(row).key = snapshot.arcs.at(row).key;
  snapshot.arc_ids_by_key.at(row).id = ArcId{row};
}

void verifyParallelOrdinalValidation()
{
  CsrGraphSnapshot snapshot = snapshotFor(NativeGraphFixtureCase::kParallel);
  require(snapshot.arcs.size() == 2 && snapshot.arcs.at(0).key.base == snapshot.arcs.at(1).key.base, "parallel fixture does not expose one two-arc base class");
  std::swap(snapshot.arcs.at(0).key.parallel_ordinal, snapshot.arcs.at(1).key.parallel_ordinal);
  std::swap(snapshot.arcs.at(0), snapshot.arcs.at(1));
  for (std::size_t row = 0; row < snapshot.arcs.size(); ++row) {
    snapshot.arcs[row].id = ArcId{row};
    refreshArcLookup(snapshot, row);
  }
  require(snapshot.arcs.at(0).key.parallel_ordinal == 0 && snapshot.arcs.at(1).key.parallel_ordinal == 1,
          "parallel mutation did not preserve canonical key order");
  require(snapshot.arcs.at(0).native_traversal_rank == 1 && snapshot.arcs.at(1).native_traversal_rank == 0,
          "parallel mutation did not preserve the native-rank permutation witness");

  const auto violations = validateCsrGraphSnapshot(snapshot);
  constexpr std::string_view message = "parallel ordinal must be assigned within its ArcKeyBase class in native traversal order";
  requireExactViolation(violations, "arcs[0].key.parallel_ordinal", message, "parallel ordinal row 0");
  requireExactViolation(violations, "arcs[1].key.parallel_ordinal", message, "parallel ordinal row 1");
}

template <typename Mutator>
void expectSignatureViolation(std::string_view label, Mutator mutate, std::string_view message)
{
  CsrGraphSnapshot snapshot = snapshotFor(NativeGraphFixtureCase::kBoundSignature);
  const std::size_t row = boundArcRow(snapshot);
  mutate(snapshot.arcs.at(row).key.base.library_arcs.at(0));
  refreshArcLookup(snapshot, row);
  const auto violations = validateCsrGraphSnapshot(snapshot);
  require(violations.size() == 1, std::string(label) + ": signature-only mutation produced unrelated violations");
  requireExactViolation(violations, "arcs[" + std::to_string(row) + "].key", message, label);
}

void verifySignatureSnapshotValidation()
{
  expectSignatureViolation(
      "absent table variables",
      [](LibraryArcSignature& signature) {
        auto& table = signature.delay_tables.at(1);
        require(!table.present, "bound fixture delay fall table unexpectedly present");
        table.variable1 = TableVariable::kInputTransition;
      },
      "library arc[0] delay table[1] is absent but retains table variables");
  expectSignatureViolation(
      "variable2 without variable1", [](LibraryArcSignature& signature) { signature.delay_tables.at(0).variable1 = TableVariable::kNone; },
      "library arc[0] delay table[0] has variable2 without variable1");
  expectSignatureViolation(
      "unknown variable1", [](LibraryArcSignature& signature) { signature.delay_tables.at(0).variable1 = static_cast<TableVariable>(255); },
      "library arc[0] delay table[0] has unknown variable1");
  expectSignatureViolation(
      "unknown variable2", [](LibraryArcSignature& signature) { signature.slew_tables.at(0).variable2 = static_cast<TableVariable>(255); },
      "library arc[0] slew table[0] has unknown variable2");
  expectSignatureViolation(
      "unknown timing sense", [](LibraryArcSignature& signature) { signature.sense = static_cast<TimingSense>(255); },
      "library arc[0] has unknown timing sense");
  expectSignatureViolation(
      "unknown trigger transition", [](LibraryArcSignature& signature) { signature.trigger_transition = static_cast<Transition>(255); },
      "library arc[0] has unknown trigger transition");
  expectSignatureViolation(
      "unknown check transition", [](LibraryArcSignature& signature) { signature.check_transition = static_cast<Transition>(255); },
      "library arc[0] has unknown check transition");

  CsrGraphSnapshot zero_dimensional = snapshotFor(NativeGraphFixtureCase::kBoundSignature);
  const std::size_t row = boundArcRow(zero_dimensional);
  zero_dimensional.arcs.at(row).key.base.library_arcs.at(0).delay_tables.at(0) = TimingTableSignature{true, TableVariable::kNone, TableVariable::kNone};
  refreshArcLookup(zero_dimensional, row);
  require(validateCsrGraphSnapshot(zero_dimensional).empty(), "present zero-dimensional table with two none variables was rejected");
}

void verifyInstanceAndNodeValidation()
{
  const CsrGraphSnapshot base = validSnapshot();
  const std::size_t input = nodeRow(base, "in");
  const std::size_t pin_a = nodeRow(base, "u0:A");
  const std::size_t pin_y = nodeRow(base, "u0:Y");
  require(base.instances.size() == 1, "chain fixture must contain exactly one instance");
  require(validateCsrGraphSnapshot(base).empty(), "valid chain snapshot failed baseline validation");

  CsrGraphSnapshot snapshot = base;
  const NodeKey original = snapshot.nodes.at(pin_a).key;
  snapshot.nodes.at(pin_a).key.full_name = "u0/A";
  const NodeKey inconsistent = snapshot.nodes.at(pin_a).key;
  snapshot.node_ids_by_key.at(pin_a).key = inconsistent;
  for (std::size_t row = 0; row < snapshot.arcs.size(); ++row) {
    auto& key = snapshot.arcs.at(row).key.base;
    if (key.source == original)
      key.source = inconsistent;
    if (key.sink == original)
      key.sink = inconsistent;
    refreshArcLookup(snapshot, row);
  }
  const auto inconsistent_name = validateCsrGraphSnapshot(snapshot);
  require(!inconsistent_name.empty(), "self-consistent slash-delimited instance-pin full_name was accepted");
  require(inconsistent_name.front().code == CsrCaptureCode::kMalformedTopology && inconsistent_name.front().field == "nodes[" + std::to_string(pin_a) + "].key"
              && inconsistent_name.front().message == "instance-pin full_name must equal instance_name + ':' + pin_name",
          "instance-pin full_name relation used the wrong first field or message");

  snapshot = base;
  snapshot.nodes.at(pin_a).is_sequential = true;
  requireExactViolation(validateCsrGraphSnapshot(snapshot), "nodes[" + std::to_string(pin_a) + "].is_sequential",
                        "instance-pin sequential role disagrees with its instance", "sequential role mismatch");

  const auto verify_role_cardinality = [&](bool NodeRecord::* role, std::string_view label) {
    CsrGraphSnapshot role_snapshot = base;
    role_snapshot.nodes.at(pin_a).*role = true;
    role_snapshot.nodes.at(pin_y).*role = true;
    requireExactViolation(validateCsrGraphSnapshot(role_snapshot), "instances[0].ordered_pins",
                          "clock, data, and output roles must each identify at most one ordered pin", label);
  };
  verify_role_cardinality(&NodeRecord::is_clock_pin, "clock role cardinality");
  verify_role_cardinality(&NodeRecord::is_data_pin, "data role cardinality");
  verify_role_cardinality(&NodeRecord::is_output_pin, "output role cardinality");

  snapshot = base;
  snapshot.nodes.at(input).is_clock_pin = true;
  requireExactViolation(validateCsrGraphSnapshot(snapshot), "nodes[" + std::to_string(input) + "].roles", "port contains instance-only cell or role state",
                        "port instance-only role");

  snapshot = base;
  snapshot.instances.at(0).ordered_pins.erase(snapshot.instances.at(0).ordered_pins.begin());
  requireExactViolation(validateCsrGraphSnapshot(snapshot), "instances[0].ordered_pins", "instance pin is missing from the ordered pin list",
                        "missing ordered pin");

  snapshot = base;
  snapshot.instances.at(0).ordered_pins.push_back(snapshot.instances.at(0).ordered_pins.front());
  requireExactViolation(validateCsrGraphSnapshot(snapshot), "instances[0].ordered_pins", "ordered pin list contains a duplicate NodeId",
                        "duplicate ordered pin");

  snapshot = base;
  snapshot.instances.at(0).ordered_pins.front() = NodeId{input};
  requireExactViolation(validateCsrGraphSnapshot(snapshot), "instances[0].ordered_pins", "ordered pin does not belong to this instance", "foreign ordered pin");

  snapshot = base;
  snapshot.instances.clear();
  requireExactViolation(validateCsrGraphSnapshot(snapshot), "nodes[" + std::to_string(pin_a) + "].key.instance_name",
                        "instance pin must reference exactly one instance", "missing instance owner");
}

void verifyLargeInstanceOwnership()
{
  constexpr std::size_t kInstanceCount = 8192;
  ista::Database database;
  for (std::size_t index = 0; index < kInstanceCount; ++index) {
    const std::string instance_name = "bulk_" + std::to_string(index);
    const std::string full_name = instance_name + ":P";

    ista::Pin pin;
    pin.set_full_name(full_name);
    pin.set_pin_name("P");
    pin.set_instance_name(instance_name);
    pin.set_direction(ista::PinDirection::kNone);
    pin.set_is_port(false);
    database.get_pin_map().emplace(full_name, std::move(pin));
    database.get_timing_point_map().emplace(full_name, ista::TimingPoint{});

    ista::Instance instance;
    instance.set_instance_name(instance_name);
    instance.set_cell_name("MISSING_CELL");
    instance.set_pin_name_list({full_name});
    database.get_instance_map().emplace(instance_name, std::move(instance));
  }

  const auto result = exportCsrGraphSnapshot(database);
  const auto* snapshot = std::get_if<CsrGraphSnapshot>(&result);
  require(snapshot != nullptr, "large instance-ownership fixture failed export");
  require(snapshot->nodes.size() == kInstanceCount && snapshot->instances.size() == kInstanceCount && snapshot->arcs.empty(),
          "large instance-ownership fixture changed graph shape");
  require(snapshot->ordered_root_ids.size() == kInstanceCount, "large instance-ownership fixture lost roots");
  require(validateCsrGraphSnapshot(*snapshot).empty(), "large instance-ownership snapshot failed strict validation");
  for (const auto& instance : snapshot->instances) {
    require(instance.ordered_pins.size() == 1, "large instance lost its sole ordered pin");
    const auto row = static_cast<std::size_t>(instance.ordered_pins.front().value());
    require(snapshot->nodes.at(row).key.instance_name == instance.instance_name, "large instance ordered-pin owner mismatch");
  }
}

void addFallbackPin(ista::Database& database, ista::Instance& instance, std::string local_name, ista::PinDirection direction, std::vector<std::string>& names)
{
  const std::string full_name = instance.get_instance_name() + ":" + local_name;
  ista::Pin pin;
  pin.set_full_name(full_name);
  pin.set_pin_name(local_name);
  pin.set_instance_name(instance.get_instance_name());
  pin.set_direction(direction);
  pin.set_is_port(false);
  database.get_pin_map().emplace(full_name, std::move(pin));
  database.get_timing_point_map().emplace(full_name, ista::TimingPoint{});
  instance.get_pin_name_list().push_back(full_name);
  names.push_back(full_name);
}

void verifyLargeFallbackCartesianProduct()
{
  constexpr std::size_t kSide = 128;
  ista::Database database;
  ista::Instance instance;
  instance.set_instance_name("large_fallback");
  instance.set_cell_name("MISSING_CELL");
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  inputs.reserve(kSide);
  outputs.reserve(kSide);
  for (std::size_t index = 0; index < kSide; ++index) {
    addFallbackPin(database, instance, "I" + std::to_string(index), ista::PinDirection::kInput, inputs);
    addFallbackPin(database, instance, "O" + std::to_string(index), ista::PinDirection::kOutput, outputs);
  }
  database.get_instance_map().emplace(instance.get_instance_name(), std::move(instance));

  for (const auto& source : inputs) {
    for (const auto& sink : outputs) {
      ista::Arc arc;
      arc.set_source_pin(source);
      arc.set_sink_pin(sink);
      arc.set_owner_name("large_fallback");
      arc.set_type(ista::ArcType::kCell);
      const std::size_t rank = database.get_arc_list().size();
      database.get_arc_list().push_back(std::move(arc));
      database.get_outgoing_arc_list_map().try_emplace(source).first->second.push_back(rank);
      database.get_incoming_arc_list_map().try_emplace(sink).first->second.push_back(rank);
    }
  }

  const auto result = exportCsrGraphSnapshot(database);
  const auto* snapshot = std::get_if<CsrGraphSnapshot>(&result);
  require(snapshot != nullptr, "large fallback Cartesian fixture failed export");
  require(snapshot->nodes.size() == 2 * kSide && snapshot->arcs.size() == kSide * kSide, "large fallback Cartesian fixture changed graph shape");
  require(validateCsrGraphSnapshot(*snapshot).empty(), "large fallback Cartesian snapshot failed strict validation");
}

void verifyMaliciousAdjacencyOffsets()
{
  const auto verify = [](bool outgoing, std::uint64_t bad_offset, std::string_view label) {
    CsrGraphSnapshot snapshot = snapshotFor(NativeGraphFixtureCase::kUnboundFallback);
    const auto& ids = outgoing ? snapshot.out_arc_ids : snapshot.in_arc_ids;
    require(snapshot.nodes.size() == 2 && ids.size() == 1, std::string(label) + ": offset fixture must have two nodes and one adjacency entry");
    auto& offsets = outgoing ? snapshot.out_row_offsets : snapshot.in_row_offsets;
    offsets = {0, bad_offset, 1};
    std::vector<CsrGraphContractViolation> violations;
    try {
      violations = validateCsrGraphSnapshot(snapshot);
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string(label) + ": validator threw: " + error.what());
    } catch (...) {
      throw std::runtime_error(std::string(label) + ": validator threw a non-standard exception");
    }
    const std::string family = outgoing ? "out" : "in";
    requireExactViolation(violations, family + "_row_offsets[1].value", "row offset exceeds adjacency entry count", label);
  };
  verify(true, 100, "out offset 100");
  verify(false, 100, "in offset 100");
  verify(true, std::numeric_limits<std::uint64_t>::max(), "out offset UINT64_MAX");
  verify(false, std::numeric_limits<std::uint64_t>::max(), "in offset UINT64_MAX");
}

}  // namespace

int main()
{
  try {
    verifyMissingAndMapOwnerFailures();
    verifyAdjacencyFailures();
    verifyCellIdentityFailures();
    verifySignatureFailures();
    verifyFallbackCartesianFailure();
    verifySnapshotValidation();
    verifyParallelOrdinalValidation();
    verifySignatureSnapshotValidation();
    verifyInstanceAndNodeValidation();
    verifyLargeInstanceOwnership();
    verifyLargeFallbackCartesianProduct();
    verifyMaliciousAdjacencyOffsets();
  } catch (const std::exception& error) {
    std::cerr << "CsrGraphExportFailureTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
