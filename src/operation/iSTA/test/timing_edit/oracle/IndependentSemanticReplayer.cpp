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
#include "IndependentSemanticReplayer.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ClockPropagator.hpp"
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "GraphBuilder.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingPropagator.hpp"

namespace ista::timing_edit::oracle {
namespace {

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error("independent semantic replayer: " + std::move(message));
}

std::string violationSummary(const TimingEditContractViolations& violations)
{
  if (violations.empty()) {
    return {};
  }
  return violations.front().field + ": " + violations.front().message;
}

IndependentTokenNames namesFor(const CreationToken& token_value, const LibraryCellRef& cell)
{
  const std::string stem = "b" + std::to_string(token_value.batch_ordinal) + "_e" + std::to_string(token_value.accepted_ordinal);
  IndependentTokenNames names;
  names.token = token_value;
  names.instance = "__ista_buf_" + stem;
  names.output_net = "__ista_net_" + stem;
  names.input_pin = names.instance + ":" + cell.input_port;
  names.output_pin = names.instance + ":" + cell.output_port;
  return names;
}

std::string resolveNode(const StableNodeRef& reference, const IndependentReplayReceipt& receipt)
{
  return std::visit(
      [&](const auto& value) -> std::string {
        using Ref = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Ref, ExistingNodeRef>) {
          return value.baseline_key.full_name;
        } else {
          const auto found = std::ranges::find_if(receipt.materialized_tokens, [&](const IndependentTokenNames& names) {
            return names.token == value.creation_token;
          });
          if (found == receipt.materialized_tokens.end()) {
            fail("created node reference precedes its token materialization");
          }
          return value.role == CreatedNodeRole::kInput ? found->input_pin : found->output_pin;
        }
      },
      reference);
}

std::string resolveNet(const StableNetRef& reference)
{
  return std::visit(
      [](const auto& value) -> std::string {
        using Ref = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Ref, ExistingNetRef>) {
          return value.baseline_name;
        } else {
          return "__ista_net_b" + std::to_string(value.creation_token.batch_ordinal) + "_e"
                 + std::to_string(value.creation_token.accepted_ordinal);
        }
      },
      reference);
}

bool isDriver(const Pin& pin)
{
  return pin.get_is_port() ? pin.get_direction() == PinDirection::kInput : pin.get_direction() == PinDirection::kOutput;
}

void eraseOnce(std::vector<std::string>& values, const std::string& value)
{
  if (std::ranges::count(values, value) != 1) {
    fail("moved sink does not occur exactly once on its source net");
  }
  values.erase(std::ranges::find(values, value));
}

std::uint64_t nextInstanceId(const Database& database)
{
  if (database.get_instance_map().empty()) {
    return 0;
  }
  std::uint64_t maximum = 0;
  for (const auto& [name, instance] : database.get_instance_map()) {
    static_cast<void>(name);
    maximum = std::max(maximum, instance.get_instance_id());
  }
  if (maximum == std::numeric_limits<std::uint64_t>::max()) {
    fail("native instance identifier capacity is exhausted");
  }
  return maximum + 1;
}

const TimingCell& requireBufferCell(const Database& database, const LibraryCellRef& reference)
{
  const auto cell = database.get_timing_library().get_cell_map().find(reference.cell_name);
  if (cell == database.get_timing_library().get_cell_map().end() || cell->second.get_library_name() != reference.library_name) {
    fail("selected Liberty cell is absent or belongs to another library");
  }
  const TimingCell& value = cell->second;
  if (value.get_is_sequential() || value.get_is_clock_gating() || value.get_is_macro() || value.get_has_clear_arc()
      || value.get_has_preset_arc()) {
    fail("selected cell is not a plain combinational buffer");
  }
  const auto input = value.get_port_map().find(reference.input_port);
  const auto output = value.get_port_map().find(reference.output_port);
  if (input == value.get_port_map().end() || !input->second.get_is_input() || input->second.get_is_output()
      || input->second.get_is_clock() || output == value.get_port_map().end() || !output->second.get_is_output()
      || output->second.get_is_input() || output->second.get_is_clock()) {
    fail("selected buffer ports have incompatible directions");
  }

  std::size_t native_signatures = 0;
  for (const TimingCellArc& cell_arc : value.get_cell_arc_list()) {
    if (cell_arc.get_source_port() != reference.input_port || cell_arc.get_sink_port() != reference.output_port) {
      continue;
    }
    if (!cell_arc.get_is_timing_graph_arc() || cell_arc.get_is_clock_arc() || cell_arc.get_is_disable_arc()
        || cell_arc.get_is_clear_preset_arc() || cell_arc.get_timing_arc_list().empty()) {
      fail("selected buffer has a non-combinational graph arc");
    }
    for (const TimingArc& timing_arc : cell_arc.get_timing_arc_list()) {
      if (timing_arc.get_sense() != TimingArcSense::kPositive || !timing_arc.get_sdf_cond().empty()) {
        fail("selected buffer Liberty arc is not unconditional positive-unate");
      }
      ++native_signatures;
    }
  }
  if (native_signatures == 0 || native_signatures != reference.ordered_arc_signatures.size()) {
    fail("selected buffer Liberty signature count differs from the typed contract");
  }
  return value;
}

const PlacedInputs* placement(const PlacementInputs& inputs)
{
  const auto* placed = std::get_if<PlacedInputs>(&inputs);
  if (placed != nullptr && (!std::isfinite(placed->x_microns) || !std::isfinite(placed->y_microns))) {
    fail("placement coordinates are not finite");
  }
  return placed;
}

void requireNoParasitics(const Database& database, const BufferInsertionMutation& edit, const std::string& source_net)
{
  if (!std::holds_alternative<NoParasiticRcInputs>(edit.rc_inputs)) {
    fail("only explicit no-parasitic replay is supported");
  }
  if (database.get_parasitic_library().get_net_map().contains(source_net)) {
    fail("source net has parasitics and cannot be split by the Stage 3 oracle");
  }
}

void requireNamesAbsent(const Database& database, const IndependentTokenNames& names)
{
  if (database.get_instance_map().contains(names.instance) || database.get_net_map().contains(names.output_net)
      || database.get_pin_map().contains(names.input_pin) || database.get_pin_map().contains(names.output_pin)) {
    fail("CreationToken materialization collides with existing raw design names");
  }
}

void applyEdit(Database& database, const BufferInsertionMutation& edit, std::uint64_t instance_id,
               IndependentReplayReceipt& receipt)
{
  const IndependentTokenNames names = namesFor(edit.creation_token, edit.library_cell);
  requireNamesAbsent(database, names);
  const TimingCell& cell = requireBufferCell(database, edit.library_cell);
  const std::string source_net_name = resolveNet(edit.original_net);
  auto source_net = database.get_net_map().find(source_net_name);
  if (source_net == database.get_net_map().end()) {
    fail("source net is absent: " + source_net_name);
  }
  requireNoParasitics(database, edit, source_net_name);

  std::string driver;
  for (const std::string& pin_name : source_net->second.get_pin_name_list()) {
    const auto pin = database.get_pin_map().find(pin_name);
    if (pin == database.get_pin_map().end()) {
      fail("source net contains a missing pin");
    }
    if (isDriver(pin->second)) {
      if (!driver.empty()) {
        fail("source net has multiple drivers");
      }
      driver = pin_name;
    }
  }
  if (driver.empty()) {
    fail("source net has no driver");
  }

  std::vector<std::string> moved;
  std::set<std::string> unique;
  for (const StableNodeRef& reference : edit.moved_sinks) {
    const std::string sink_name = resolveNode(reference, receipt);
    const auto sink = database.get_pin_map().find(sink_name);
    if (!unique.insert(sink_name).second || sink == database.get_pin_map().end()
        || sink->second.get_net_name() != source_net_name || isDriver(sink->second) || sink_name == driver) {
      fail("typed moved sink is not a unique load of the source net");
    }
    moved.push_back(sink_name);
  }

  Instance instance;
  instance.set_instance_id(instance_id);
  instance.set_instance_name(names.instance);
  instance.set_cell_name(edit.library_cell.cell_name);
  std::vector<std::string> instance_pins{names.input_pin, names.output_pin};
  std::ranges::sort(instance_pins);
  instance.set_pin_name_list(instance_pins);
  instance.set_output_pin_name(names.output_pin);
  instance.set_is_sequential(cell.get_is_sequential());
  instance.set_is_clock_gating(cell.get_is_clock_gating());
  instance.set_has_clear_arc(cell.get_has_clear_arc());
  instance.set_has_preset_arc(cell.get_has_preset_arc());
  instance.set_is_io_cell(false);

  const PlacedInputs* placed = placement(edit.placement_inputs);
  auto make_pin = [&](std::string full_name, std::string port, PinDirection direction) {
    Pin pin;
    pin.set_pin_name(port);
    pin.set_full_name(full_name);
    pin.set_instance_name(names.instance);
    pin.set_direction(direction);
    pin.set_is_port(false);
    if (placed != nullptr) {
      pin.set_x(placed->x_microns);
      pin.set_y(placed->y_microns);
    }
    return pin;
  };
  Pin input_pin = make_pin(names.input_pin, edit.library_cell.input_port, PinDirection::kInput);
  Pin output_pin = make_pin(names.output_pin, edit.library_cell.output_port, PinDirection::kOutput);
  input_pin.set_net_name(source_net_name);
  output_pin.set_net_name(names.output_net);

  for (const std::string& sink : moved) {
    eraseOnce(source_net->second.get_pin_name_list(), sink);
  }
  source_net->second.get_pin_name_list().push_back(names.input_pin);

  Net output_net;
  output_net.set_net_name(names.output_net);
  std::vector<std::string> output_members{names.output_pin};
  output_members.insert(output_members.end(), moved.begin(), moved.end());
  output_net.set_pin_name_list(output_members);

  database.get_instance_map().emplace(names.instance, std::move(instance));
  database.get_pin_map().emplace(names.input_pin, std::move(input_pin));
  database.get_pin_map().emplace(names.output_pin, std::move(output_pin));
  database.get_net_map().emplace(names.output_net, std::move(output_net));
  for (const std::string& sink : moved) {
    database.get_pin_map().at(sink).set_net_name(names.output_net);
  }
  receipt.materialized_tokens.push_back(names);
}

void resetDerivedState(Database& database)
{
  database.get_arc_list().clear();
  database.get_outgoing_arc_list_map().clear();
  database.get_incoming_arc_list_map().clear();
  database.get_start_point_list().clear();
  database.get_end_point_list().clear();
  database.get_timing_order_list().clear();
  database.get_timing_point_map().clear();
  database.get_timing_path_group_list().clear();
  database.get_summary().ta_summary = TASummary{};
  database.get_power_activity_map().clear();
  database.get_instance_power_map().clear();
  database.get_power_summary() = PowerSummary{};
  for (auto& [name, net] : database.get_net_map()) {
    static_cast<void>(name);
    net.get_driver_pin().clear();
    net.get_driver_pin_list().clear();
    net.get_load_pin_list().clear();
  }
}

void destroyTransient() noexcept
{
  TimingAnalyzer::destroyInst();
  TimingPropagator::destroyInst();
  ClockPropagator::destroyInst();
  GraphBuilder::destroyInst();
}

void quiesceDelay()
{
  DelayCalculator::initInst();
  try {
    STADC.destroy();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    DelayCalculator::destroyInst();
    DelayCalculator::initInst();
    std::rethrow_exception(failure);
  }
  DelayCalculator::destroyInst();
}

template <typename Stage, typename Work>
void runStage(Work work)
{
  Stage::initInst();
  try {
    work();
  } catch (...) {
    Stage::destroyInst();
    throw;
  }
  Stage::destroyInst();
}

std::vector<ElectricalLoadObservation> captureLoads(Database& database)
{
  std::vector<ElectricalLoadObservation> result;
  constexpr AnalysisType analyses[]{AnalysisType::kMax, AnalysisType::kMin};
  constexpr TransType transitions[]{TransType::kRise, TransType::kFall};
  for (auto& [name, net] : database.get_net_map()) {
    static_cast<void>(name);
    if (net.get_driver_pin().empty()) {
      continue;
    }
    for (AnalysisType analysis : analyses) {
      for (TransType transition : transitions) {
        std::string output_pin = net.get_driver_pin();
        const double load = STADC.getPowerOutputLoad(output_pin, analysis, transition);
        if (!std::isfinite(load) || load < 0.0) {
          fail("native electrical output load is invalid");
        }
        result.push_back({std::move(output_pin), static_cast<std::int32_t>(analysis), static_cast<std::int32_t>(transition), load});
      }
    }
  }
  return result;
}

void rebuildAndTime(Database& database, IndependentReplayReceipt& receipt)
{
  destroyTransient();
  quiesceDelay();
  resetDerivedState(database);
  try {
    DelayCalculator::initInst();
    STADC.init();
    runStage<GraphBuilder>([] { STAGB.build(); });
    runStage<ClockPropagator>([] { STACP.propagate(); });
    runStage<TimingPropagator>([] { STATP.propagate(); });
    runStage<TimingAnalyzer>([] { STATA.analyze(); });
    receipt.electrical_loads = captureLoads(database);
    destroyTransient();
    STADC.destroy();
    DelayCalculator::destroyInst();
    DelayCalculator::initInst();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    destroyTransient();
    try {
      DelayCalculator::initInst();
      STADC.destroy();
    } catch (...) {
    }
    DelayCalculator::destroyInst();
    try {
      DelayCalculator::initInst();
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

void applyPlanRaw(Database& database, const DesignEditPlan& plan, IndependentReplayReceipt& receipt)
{
  const TimingEditContractViolations violations = validateDesignEditPlan(plan);
  if (!violations.empty()) {
    fail("typed DesignEditPlan is invalid: " + violationSummary(violations));
  }
  if (database.get_design_name() != plan.baseline.design_name) {
    fail("active design name differs from the permanent baseline identity");
  }
  std::uint64_t instance_id = nextInstanceId(database);
  for (const BufferInsertionMutation& edit : plan.edits) {
    applyEdit(database, edit, instance_id, receipt);
    if (instance_id == std::numeric_limits<std::uint64_t>::max()) {
      fail("native instance identifier capacity is exhausted within batch");
    }
    ++instance_id;
  }
}

}  // namespace

IndependentReplayReceipt IndependentSemanticReplayer::replayPlan(Database& database, const DesignEditPlan& plan)
{
  IndependentReplayReceipt receipt;
  receipt.prefix_batches = 1;
  applyPlanRaw(database, plan, receipt);
  rebuildAndTime(database, receipt);
  return receipt;
}

IndependentReplayReceipt IndependentSemanticReplayer::replayPrefix(Database& database, const TimingEditTrace& trace,
                                                                    std::size_t prefix_batches)
{
  const TimingEditContractViolations violations = validateTimingEditTrace(trace);
  if (!violations.empty()) {
    fail("typed TimingEditTrace is invalid: " + violationSummary(violations));
  }
  if (prefix_batches == 0 || prefix_batches > trace.batches.size()) {
    fail("requested prefix is outside the typed trace");
  }
  if (trace.workload_kind != TraceWorkloadKind::kRealDesign || database.get_design_name() != trace.baseline.design_name) {
    fail("fresh replay requires the matching real-design baseline");
  }

  IndependentReplayReceipt receipt;
  receipt.prefix_batches = prefix_batches;
  for (std::size_t index = 0; index < prefix_batches; ++index) {
    const TimingEditTraceBatch& batch = trace.batches[index];
    DesignEditPlan plan{.baseline = trace.baseline, .expected_generation = batch.expected_pre_identity.generation,
                        .edits = batch.edits};
    applyPlanRaw(database, plan, receipt);
  }
  rebuildAndTime(database, receipt);
  return receipt;
}

}  // namespace ista::timing_edit::oracle
