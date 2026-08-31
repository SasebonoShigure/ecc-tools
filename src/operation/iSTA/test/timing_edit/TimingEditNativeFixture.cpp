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
#include "TimingEditNativeFixture.hpp"

#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "Database.hpp"
#include "LogicExpressionTerm.hpp"
#include "Net.hpp"
#include "Pin.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingCellPort.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingPoint.hpp"

namespace ista::timing_edit::test {
namespace {

Pin makePort(const std::string& name, PinDirection direction)
{
  Pin pin;
  pin.set_pin_name(name);
  pin.set_full_name(name);
  pin.set_net_name("n0");
  pin.set_direction(direction);
  pin.set_is_port(true);
  return pin;
}

LogicExpressionTerm expressionTerm(LogicOperationType operation, std::string port_name = {})
{
  LogicExpressionTerm term;
  term.set_operation_type(operation);
  term.set_port_name(port_name);
  return term;
}

TimingCellPort makeCellPort(const std::string& name, bool input, bool output)
{
  TimingCellPort port;
  port.set_port_name(name);
  port.set_is_input(input);
  port.set_is_output(output);
  port.set_is_clock(false);
  return port;
}

TimingCellArc makeCellArc(const std::string& source, TimingArcSense sense)
{
  TimingArc child;
  child.set_library_name("slow.lib");
  child.set_arc_idx(0);
  child.set_sense(sense);
  child.set_trigger_trans_type(TransType::kNone);
  child.set_check_trans_type(TransType::kNone);

  TimingCellArc arc;
  arc.set_source_port(source);
  arc.set_sink_port("Y");
  arc.set_delay(0.05);
  arc.set_delay_min(0.04);
  arc.set_delay_max(0.06);
  arc.set_is_timing_graph_arc(true);
  arc.set_is_clock_arc(false);
  arc.set_is_disable_arc(false);
  arc.set_is_clear_preset_arc(false);
  arc.get_timing_arc_list().push_back(std::move(child));
  return arc;
}

TimingCell makeBufferCell()
{
  TimingCell cell;
  cell.set_cell_name("BUF_X1");
  cell.set_library_name("slow.lib");
  cell.get_port_map().emplace("A", makeCellPort("A", true, false));
  TimingCellPort output = makeCellPort("Y", false, true);
  output.get_function_expression().set_term_list({expressionTerm(LogicOperationType::kPort, "A")});
  cell.get_port_map().emplace("Y", std::move(output));
  cell.get_cell_arc_list().push_back(makeCellArc("A", TimingArcSense::kPositive));
  return cell;
}

TimingCell makeInverterCell()
{
  TimingCell cell;
  cell.set_cell_name("INV_X1");
  cell.set_library_name("slow.lib");
  cell.get_port_map().emplace("A", makeCellPort("A", true, false));
  TimingCellPort output = makeCellPort("Y", false, true);
  output.get_function_expression().set_term_list(
      {expressionTerm(LogicOperationType::kPort, "A"), expressionTerm(LogicOperationType::kNot)});
  cell.get_port_map().emplace("Y", std::move(output));
  cell.get_cell_arc_list().push_back(makeCellArc("A", TimingArcSense::kNegative));
  return cell;
}

TimingCell makeOrCell()
{
  TimingCell cell;
  cell.set_cell_name("OR2_X1");
  cell.set_library_name("slow.lib");
  cell.get_port_map().emplace("A", makeCellPort("A", true, false));
  cell.get_port_map().emplace("B", makeCellPort("B", true, false));
  TimingCellPort output = makeCellPort("Y", false, true);
  output.get_function_expression().set_term_list({expressionTerm(LogicOperationType::kPort, "A"),
                                                   expressionTerm(LogicOperationType::kPort, "B"),
                                                   expressionTerm(LogicOperationType::kOr)});
  cell.get_port_map().emplace("Y", std::move(output));
  cell.get_cell_arc_list().push_back(makeCellArc("A", TimingArcSense::kPositive));
  return cell;
}

void addBaselineArc(Database& database)
{
  Arc arc;
  arc.set_arc_name("n0:in->out");
  arc.set_source_pin("in");
  arc.set_sink_pin("out");
  arc.set_owner_name("n0");
  arc.set_type(ArcType::kNet);
  database.get_arc_list().push_back(std::move(arc));
  const std::size_t index = database.get_arc_list().size() - 1;
  database.get_outgoing_arc_list_map()["in"].push_back(index);
  database.get_incoming_arc_list_map()["out"].push_back(index);
}

void populate(Database& database, bool parallel_baseline_arcs)
{
  database.set_design_name("stage3_edit_fixture");
  database.get_pin_map().emplace("in", makePort("in", PinDirection::kInput));
  database.get_pin_map().emplace("out", makePort("out", PinDirection::kOutput));

  Net net;
  net.set_net_name("n0");
  net.set_pin_name_list({"in", "out"});
  net.set_driver_pin("in");
  net.set_driver_pin_list({"in"});
  net.set_load_pin_list({"out"});
  database.get_net_map().emplace("n0", std::move(net));

  database.get_timing_point_map().emplace("in", TimingPoint{});
  database.get_timing_point_map().emplace("out", TimingPoint{});
  database.get_timing_point_map().at("in").set_level(1);
  database.get_timing_point_map().at("out").set_level(2);
  database.get_start_point_list() = {"in"};
  database.get_end_point_list() = {"out"};
  database.get_timing_order_list() = {"in", "out"};
  addBaselineArc(database);
  if (parallel_baseline_arcs) {
    addBaselineArc(database);
  }

  database.get_timing_library().get_cell_map().emplace("BUF_X1", makeBufferCell());
  database.get_timing_library().get_cell_map().emplace("INV_X1", makeInverterCell());
  database.get_timing_library().get_cell_map().emplace("OR2_X1", makeOrCell());
}

csr::NodeKey requireNodeKey(const Database& database, const std::string& full_name)
{
  const csr::CsrGraphExportResult exported = csr::exportCsrGraphSnapshot(database);
  const auto* graph = std::get_if<csr::CsrGraphSnapshot>(&exported);
  if (graph == nullptr) {
    throw std::logic_error("native edit fixture graph export failed");
  }
  for (const auto& node : graph->nodes) {
    if (node.key.full_name == full_name) {
      return node.key;
    }
  }
  throw std::logic_error("native edit fixture node is missing: " + full_name);
}

}  // namespace

NativeEditFixture::NativeEditFixture() : _database(std::make_unique<Database>()) {}
NativeEditFixture::~NativeEditFixture() = default;
NativeEditFixture::NativeEditFixture(NativeEditFixture&&) noexcept = default;
NativeEditFixture& NativeEditFixture::operator=(NativeEditFixture&&) noexcept = default;

Database& NativeEditFixture::database()
{
  return *_database;
}

const Database& NativeEditFixture::database() const
{
  return *_database;
}

NativeEditFixture makeNativeEditFixture(bool parallel_baseline_arcs)
{
  NativeEditFixture fixture;
  populate(fixture.database(), parallel_baseline_arcs);
  return fixture;
}

csr::StaAnalysisContext editTestAnalysisContext()
{
  return {"slow", "25", "__no_rc_corner__"};
}

LibraryCellRef nativeLibraryCellRef(const Database& database, const std::string& cell_name)
{
  const TimingCell& cell = database.get_timing_library().get_cell_map().at(cell_name);
  const TimingCellArc& arc = cell.get_cell_arc_list().front();
  std::vector<csr::LibraryArcSignature> signatures;
  signatures.reserve(arc.get_timing_arc_list().size());
  for (const TimingArc& child : arc.get_timing_arc_list()) {
    signatures.push_back(csr::detail::exportReferenceLibraryArcSignature(child));
  }
  return LibraryCellRef{cell.get_library_name(), cell.get_cell_name(), arc.get_source_port(), arc.get_sink_port(),
                        std::move(signatures)};
}

BaselineSemanticIdentity baselineIdentity(Database& database, TransactionGeneration generation,
                                          TimingEditSessionToken session_token)
{
  const CommittedEditIdentity current
      = NativeTimingEditPreparer::captureCurrentIdentity(database, generation, editTestAnalysisContext(), session_token);
  return BaselineSemanticIdentity{database.get_design_name(), current.current_fingerprints, current.canonical_pre_state};
}

CreationToken editTestCreationToken(std::uint64_t batch_ordinal, std::uint64_t accepted_ordinal)
{
  return CreationToken{kCreationTokenSchemaVersion, "stage3_case", "stage3_profile", 20260831, batch_ordinal,
                       accepted_ordinal};
}

DesignEditPlan bufferInsertionPlan(const Database& database, const BaselineSemanticIdentity& baseline,
                                   TransactionGeneration generation, CreationToken creation_token)
{
  BufferInsertionMutation edit;
  edit.original_net = ExistingNetRef{"n0"};
  edit.moved_sinks = {ExistingNodeRef{requireNodeKey(database, "out")}};
  edit.library_cell = nativeLibraryCellRef(database, "BUF_X1");
  edit.placement_inputs = UnplacedInputs{};
  edit.rc_inputs = NoParasiticRcInputs{};
  edit.creation_token = std::move(creation_token);
  return DesignEditPlan{.baseline = baseline, .expected_generation = generation, .edits = {std::move(edit)}};
}

}  // namespace ista::timing_edit::test
