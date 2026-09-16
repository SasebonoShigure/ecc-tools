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
#include "IndependentOracleObservation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "Database.hpp"

namespace ista::timing_edit::oracle {
namespace {

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error("independent oracle observation: " + std::move(message));
}

std::string doubleBits(double value)
{
  std::ostringstream stream;
  stream << std::hex << std::setw(16) << std::setfill('0') << std::bit_cast<std::uint64_t>(value);
  return stream.str();
}

std::string field(std::string_view value)
{
  return std::to_string(value.size()) + ":" + std::string(value);
}

template <typename... Values>
std::string record(std::string_view tag, const Values&... values)
{
  std::string result(tag);
  ((result += "|" + field(values)), ...);
  return result;
}

std::string integer(std::int64_t value)
{
  return std::to_string(value);
}

std::string boolean(bool value)
{
  return value ? "1" : "0";
}

std::string tokenInstance(const CreationToken& value)
{
  return "__ista_buf_b" + std::to_string(value.batch_ordinal) + "_e" + std::to_string(value.accepted_ordinal);
}

std::string tokenNet(const CreationToken& value)
{
  return "__ista_net_b" + std::to_string(value.batch_ordinal) + "_e" + std::to_string(value.accepted_ordinal);
}

using TokenPorts = std::map<CreationToken, std::pair<std::string, std::string>>;

std::string resolveNode(const StableNodeRef& reference, const TokenPorts& ports)
{
  return std::visit(
      [&](const auto& value) -> std::string {
        using Ref = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Ref, ExistingNodeRef>) {
          return value.baseline_key.full_name;
        } else {
          const auto found = ports.find(value.creation_token);
          if (found == ports.end()) {
            fail("created node reference precedes its token materialization");
          }
          return tokenInstance(value.creation_token) + ":"
                 + (value.role == CreatedNodeRole::kInput ? found->second.first : found->second.second);
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
          return tokenNet(value.creation_token);
        }
      },
      reference);
}

template <typename Record>
std::map<std::string, Record> keyed(const std::vector<Record>& values)
{
  std::map<std::string, Record> result;
  for (const Record& value : values) {
    if (!result.emplace(value.map_name, value).second) {
      fail("raw observation contains duplicate map keys");
    }
  }
  return result;
}

template <typename Record>
std::vector<Record> valuesOf(const std::map<std::string, Record>& values)
{
  std::vector<Record> result;
  result.reserve(values.size());
  for (const auto& [key, value] : values) {
    static_cast<void>(key);
    result.push_back(value);
  }
  return result;
}

bool rawDriver(const RawPinObservation& pin)
{
  const auto direction = static_cast<PinDirection>(pin.direction);
  return pin.is_port ? direction == PinDirection::kInput : direction == PinDirection::kOutput;
}

void refreshNetCaches(std::map<std::string, RawNetObservation>& nets,
                      const std::map<std::string, RawPinObservation>& pins)
{
  for (auto& [name, net] : nets) {
    static_cast<void>(name);
    net.driver_pin.clear();
    net.driver_pins.clear();
    net.load_pins.clear();
    for (const std::string& pin_name : net.pin_names) {
      const auto pin = pins.find(pin_name);
      if (pin == pins.end()) {
        fail("expected raw net refers to a missing pin");
      }
      if (rawDriver(pin->second)) {
        net.driver_pin = pin_name;
        net.driver_pins.push_back(pin_name);
      } else {
        net.load_pins.push_back(pin_name);
      }
    }
    std::ranges::sort(net.driver_pins);
    std::ranges::sort(net.load_pins);
  }
}

std::uint64_t nextInstanceId(const std::map<std::string, RawInstanceObservation>& instances)
{
  if (instances.empty()) {
    return 0;
  }
  std::uint64_t maximum = 0;
  for (const auto& [name, instance] : instances) {
    static_cast<void>(name);
    maximum = std::max(maximum, instance.instance_id);
  }
  if (maximum == std::numeric_limits<std::uint64_t>::max()) {
    fail("expected raw instance identifier capacity is exhausted");
  }
  return maximum + 1;
}

void applyExpectedEdit(std::map<std::string, RawInstanceObservation>& instances,
                       std::map<std::string, RawPinObservation>& pins,
                       std::map<std::string, RawNetObservation>& nets,
                       const BufferInsertionMutation& edit, TokenPorts& ports)
{
  const std::string instance_name = tokenInstance(edit.creation_token);
  const std::string output_net_name = tokenNet(edit.creation_token);
  const std::string input_pin_name = instance_name + ":" + edit.library_cell.input_port;
  const std::string output_pin_name = instance_name + ":" + edit.library_cell.output_port;
  if (instances.contains(instance_name) || pins.contains(input_pin_name) || pins.contains(output_pin_name)
      || nets.contains(output_net_name)) {
    fail("expected token materialization collides");
  }

  const std::string source_net_name = resolveNet(edit.original_net);
  auto source_net = nets.find(source_net_name);
  if (source_net == nets.end()) {
    fail("expected source net is absent");
  }
  std::vector<std::string> moved;
  for (const StableNodeRef& reference : edit.moved_sinks) {
    const std::string sink = resolveNode(reference, ports);
    const auto pin = pins.find(sink);
    if (pin == pins.end() || pin->second.net_name != source_net_name
        || std::ranges::count(source_net->second.pin_names, sink) != 1) {
      fail("expected moved sink is not a member of the source net");
    }
    source_net->second.pin_names.erase(std::ranges::find(source_net->second.pin_names, sink));
    moved.push_back(sink);
  }
  source_net->second.pin_names.push_back(input_pin_name);

  std::vector<std::string> instance_pins{input_pin_name, output_pin_name};
  std::ranges::sort(instance_pins);
  instances.emplace(instance_name,
                    RawInstanceObservation{instance_name, nextInstanceId(instances), instance_name,
                                           edit.library_cell.cell_name, std::move(instance_pins), {}, output_pin_name,
                                           {}, false, false, false, false, false});

  double x = 0.0;
  double y = 0.0;
  if (const auto* placed = std::get_if<PlacedInputs>(&edit.placement_inputs)) {
    x = placed->x_microns;
    y = placed->y_microns;
  }
  pins.emplace(input_pin_name,
               RawPinObservation{input_pin_name, edit.library_cell.input_port, input_pin_name, instance_name,
                                 source_net_name, static_cast<std::int32_t>(PinDirection::kInput), x, y, false});
  pins.emplace(output_pin_name,
               RawPinObservation{output_pin_name, edit.library_cell.output_port, output_pin_name, instance_name,
                                 output_net_name, static_cast<std::int32_t>(PinDirection::kOutput), x, y, false});
  std::vector<std::string> output_members{output_pin_name};
  output_members.insert(output_members.end(), moved.begin(), moved.end());
  nets.emplace(output_net_name,
               RawNetObservation{output_net_name, output_net_name, {}, {}, {}, std::move(output_members)});
  for (const std::string& sink : moved) {
    pins.at(sink).net_name = output_net_name;
  }
  ports.emplace(edit.creation_token, std::pair{edit.library_cell.input_port, edit.library_cell.output_port});
}

template <typename Record>
void compareRecords(const std::vector<Record>& expected, const std::vector<Record>& observed,
                    std::string_view family, std::vector<std::string>& mismatches)
{
  const auto expected_by_key = keyed(expected);
  const auto observed_by_key = keyed(observed);
  std::set<std::string> keys;
  for (const auto& [key, value] : expected_by_key) {
    static_cast<void>(value);
    keys.insert(key);
  }
  for (const auto& [key, value] : observed_by_key) {
    static_cast<void>(value);
    keys.insert(key);
  }
  for (const std::string& key : keys) {
    const auto lhs = expected_by_key.find(key);
    const auto rhs = observed_by_key.find(key);
    if (lhs == expected_by_key.end() || rhs == observed_by_key.end() || !(lhs->second == rhs->second)) {
      mismatches.push_back("raw." + std::string(family) + "." + key);
    }
  }
}

void appendStringVector(std::vector<std::string>& records, std::string_view tag, std::string_view owner,
                        const std::vector<std::string>& values)
{
  for (std::size_t index = 0; index < values.size(); ++index) {
    records.push_back(record(tag, std::string(owner), integer(static_cast<std::int64_t>(index)), values[index]));
  }
}

template <typename OuterMap>
void appendLaneDoubleMap(std::vector<std::string>& records, std::string_view owner, std::string_view family,
                         const OuterMap& values)
{
  for (const auto& [analysis, transitions] : values) {
    for (const auto& [transition, value] : transitions) {
      records.push_back(record("point_lane", std::string(owner), std::string(family),
                               integer(static_cast<std::int32_t>(analysis)),
                               integer(static_cast<std::int32_t>(transition)), doubleBits(value)));
    }
  }
}

template <typename OuterMap>
void appendLaneStringMap(std::vector<std::string>& records, std::string_view owner, std::string_view family,
                         const OuterMap& values)
{
  for (const auto& [analysis, transitions] : values) {
    for (const auto& [transition, value] : transitions) {
      records.push_back(record("point_lane", std::string(owner), std::string(family),
                               integer(static_cast<std::int32_t>(analysis)),
                               integer(static_cast<std::int32_t>(transition)), value));
    }
  }
}

template <typename OuterMap>
void appendLaneTransitionMap(std::vector<std::string>& records, std::string_view owner, std::string_view family,
                             const OuterMap& values)
{
  for (const auto& [analysis, transitions] : values) {
    for (const auto& [transition, value] : transitions) {
      records.push_back(record("point_lane", std::string(owner), std::string(family),
                               integer(static_cast<std::int32_t>(analysis)),
                               integer(static_cast<std::int32_t>(transition)),
                               integer(static_cast<std::int32_t>(value))));
    }
  }
}

void appendPointRecords(std::vector<std::string>& records, const Database& database)
{
  for (const auto& [name, point] : database.get_timing_point_map()) {
    records.push_back(record("point", name, doubleBits(point.get_arrival()), doubleBits(point.get_required()),
                             doubleBits(point.get_slack()), doubleBits(point.get_launch_time()), integer(point.get_level()),
                             point.get_predecessor(), std::string(point.get_clock_name()),
                             integer(static_cast<std::int64_t>(point.get_predecessor_arc_idx())),
                             boolean(point.get_is_clock_point())));
    appendLaneDoubleMap(records, name, "clock_arrival", point.get_clock_arrival_map());
    appendLaneDoubleMap(records, name, "clock_slew", point.get_clock_slew_map());
    appendLaneDoubleMap(records, name, "physical_clock_arrival", point.get_physical_clock_arrival_map());
    appendLaneDoubleMap(records, name, "physical_clock_slew", point.get_physical_clock_slew_map());
    appendLaneStringMap(records, name, "clock_predecessor", point.get_clock_predecessor_map());
    appendLaneDoubleMap(records, name, "clock_predecessor_delay", point.get_clock_predecessor_arc_delay_map());
    appendLaneTransitionMap(records, name, "clock_predecessor_transition", point.get_clock_predecessor_trans_type_map());
    appendLaneStringMap(records, name, "physical_clock_predecessor", point.get_physical_clock_predecessor_map());
    appendLaneDoubleMap(records, name, "physical_clock_predecessor_delay", point.get_physical_clock_predecessor_arc_delay_map());
    appendLaneTransitionMap(records, name, "physical_clock_predecessor_transition", point.get_physical_clock_predecessor_trans_type_map());
    appendLaneDoubleMap(records, name, "data_slew", point.get_data_slew_map());
    for (const auto& [analysis, sources] : point.get_path_state_map()) {
      for (const auto& [source, transitions] : sources) {
        for (const auto& [transition, clocks] : transitions) {
          for (const auto& [clock, state] : clocks) {
            records.push_back(record("path", name, integer(static_cast<std::int32_t>(analysis)),
                                     integer(static_cast<std::int32_t>(source)), integer(static_cast<std::int32_t>(transition)),
                                     clock, doubleBits(state.get_arrival()), doubleBits(state.get_slew()),
                                     doubleBits(state.get_launch_time()), state.get_start_point(), state.get_predecessor(),
                                     state.get_clock_name(), state.get_crpr_clock_pin(),
                                     integer(static_cast<std::int64_t>(state.get_predecessor_arc_idx())),
                                     doubleBits(state.get_predecessor_arc_delay()),
                                     integer(static_cast<std::int32_t>(state.get_trans_type())),
                                     integer(static_cast<std::int32_t>(state.get_predecessor_trans_type())),
                                     integer(static_cast<std::int32_t>(state.get_crpr_clock_trans_type()))));
          }
        }
      }
    }
  }
}

void appendArcRecords(std::vector<std::string>& records, const Database& database)
{
  for (std::size_t rank = 0; rank < database.get_arc_list().size(); ++rank) {
    const Arc& arc = database.get_arc_list()[rank];
    records.push_back(record("arc", integer(static_cast<std::int64_t>(rank)), arc.get_source_pin(), arc.get_sink_pin(),
                             arc.get_owner_name(), arc.get_library_source_port(), arc.get_library_sink_port(),
                             integer(static_cast<std::int32_t>(arc.get_type())), doubleBits(arc.get_delay()),
                             doubleBits(arc.get_delay_min()), doubleBits(arc.get_delay_max()), boolean(arc.get_is_clock_arc()),
                             boolean(arc.get_is_disable_arc()), boolean(arc.get_is_loop_disable()),
                             boolean(arc.get_timing_cell_arc() != nullptr)));
    for (const auto& [analysis, inputs] : arc.get_trans_delay_map()) {
      for (const auto& [input, value] : inputs) {
        records.push_back(record("arc_trans_delay", integer(static_cast<std::int64_t>(rank)),
                                 integer(static_cast<std::int32_t>(analysis)), integer(static_cast<std::int32_t>(input)),
                                 doubleBits(value)));
      }
    }
    auto append_matrix = [&](std::string_view tag, const auto& matrix) {
      for (const auto& [analysis, inputs] : matrix) {
        for (const auto& [input, outputs] : inputs) {
          for (const auto& [output, value] : outputs) {
            records.push_back(record(tag, integer(static_cast<std::int64_t>(rank)),
                                     integer(static_cast<std::int32_t>(analysis)), integer(static_cast<std::int32_t>(input)),
                                     integer(static_cast<std::int32_t>(output)), doubleBits(value)));
          }
        }
      }
    };
    append_matrix("arc_input_output_delay", arc.get_input_output_delay_map());
    append_matrix("arc_graph_delay", arc.get_graph_delay_map());
    for (const auto& [arc_index, analyses] : arc.get_timing_arc_delay_map()) {
      for (const auto& [analysis, inputs] : analyses) {
        for (const auto& [input, outputs] : inputs) {
          for (const auto& [output, value] : outputs) {
            records.push_back(record("timing_arc_delay", integer(static_cast<std::int64_t>(rank)), integer(arc_index),
                                     integer(static_cast<std::int32_t>(analysis)), integer(static_cast<std::int32_t>(input)),
                                     integer(static_cast<std::int32_t>(output)), doubleBits(value)));
          }
        }
      }
    }
    for (const auto& [arc_index, analysis, input, output] : arc.get_timing_arc_delay_state().fallback_key_set) {
      records.push_back(record("timing_arc_fallback", integer(static_cast<std::int64_t>(rank)), integer(arc_index),
                               integer(static_cast<std::int32_t>(analysis)), integer(static_cast<std::int32_t>(input)),
                               integer(static_cast<std::int32_t>(output))));
    }
    for (const auto& [input, output] : arc.get_trans_type_map()) {
      records.push_back(record("arc_transition", integer(static_cast<std::int64_t>(rank)),
                               integer(static_cast<std::int32_t>(input)), integer(static_cast<std::int32_t>(output))));
    }
  }
}

void appendAdjacency(std::vector<std::string>& records, std::string_view tag,
                     const std::map<std::string, std::vector<std::size_t>>& rows)
{
  for (const auto& [name, ranks] : rows) {
    for (std::size_t ordinal = 0; ordinal < ranks.size(); ++ordinal) {
      records.push_back(record(tag, name, integer(static_cast<std::int64_t>(ordinal)),
                               integer(static_cast<std::int64_t>(ranks[ordinal]))));
    }
  }
}

bool validIndex(std::size_t index, std::size_t extent)
{
  return index == std::numeric_limits<std::size_t>::max() || index < extent;
}

template <typename Map>
bool containsNan(const Map& values)
{
  for (const auto& [outer, inner] : values) {
    static_cast<void>(outer);
    for (const auto& [key, value] : inner) {
      static_cast<void>(key);
      if (std::isnan(value)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

RawDesignObservation captureRawDesign(Database& database)
{
  RawDesignObservation result;
  result.design_name = database.get_design_name();
  for (const auto& [name, instance] : database.get_instance_map()) {
    result.instances.push_back({name, instance.get_instance_id(), instance.get_instance_name(), instance.get_cell_name(),
                                instance.get_pin_name_list(), instance.get_clock_pin_name(), instance.get_output_pin_name(),
                                instance.get_data_pin_name(), instance.get_is_sequential(), instance.get_is_clock_gating(),
                                instance.get_has_clear_arc(), instance.get_has_preset_arc(), instance.get_is_io_cell()});
  }
  for (const auto& [name, pin] : database.get_pin_map()) {
    result.pins.push_back({name, pin.get_pin_name(), pin.get_full_name(), pin.get_instance_name(), pin.get_net_name(),
                           static_cast<std::int32_t>(pin.get_direction()), pin.get_x(), pin.get_y(), pin.get_is_port()});
  }
  for (const auto& [name, net] : database.get_net_map()) {
    result.nets.push_back({name, net.get_net_name(), net.get_driver_pin(), net.get_driver_pin_list(),
                           net.get_load_pin_list(), net.get_pin_name_list()});
  }
  return result;
}

RawDesignObservation projectExpectedRawAfterPrefix(const RawDesignObservation& baseline, const TimingEditTrace& trace,
                                                    std::size_t prefix_batches)
{
  if (prefix_batches == 0 || prefix_batches > trace.batches.size()) {
    fail("expected raw prefix is out of range");
  }
  auto instances = keyed(baseline.instances);
  auto pins = keyed(baseline.pins);
  auto nets = keyed(baseline.nets);
  TokenPorts ports;
  for (std::size_t index = 0; index < prefix_batches; ++index) {
    for (const BufferInsertionMutation& edit : trace.batches[index].edits) {
      applyExpectedEdit(instances, pins, nets, edit, ports);
    }
  }
  refreshNetCaches(nets, pins);
  return {baseline.design_name, valuesOf(instances), valuesOf(pins), valuesOf(nets)};
}

std::vector<std::string> compareRawDesign(const RawDesignObservation& expected,
                                          const RawDesignObservation& observed)
{
  std::vector<std::string> mismatches;
  if (expected.design_name != observed.design_name) {
    mismatches.push_back("raw.design_name");
  }
  compareRecords(expected.instances, observed.instances, "instance", mismatches);
  compareRecords(expected.pins, observed.pins, "pin", mismatches);
  compareRecords(expected.nets, observed.nets, "net", mismatches);
  return mismatches;
}

std::vector<std::string> validateDerivedDatabase(const Database& database,
                                                 const IndependentReplayReceipt& receipt)
{
  std::vector<std::string> errors;
  const auto& points = database.get_timing_point_map();
  const auto& pins = database.get_pin_map();
  const auto& arcs = database.get_arc_list();
  if (points.size() != pins.size()) {
    errors.push_back("timing point/pin cardinality differs");
  }
  for (const auto& [name, pin] : pins) {
    static_cast<void>(pin);
    if (!points.contains(name)) {
      errors.push_back("missing timing point: " + name);
    }
  }

  std::map<std::string, std::vector<std::size_t>> expected_out;
  std::map<std::string, std::vector<std::size_t>> expected_in;
  std::size_t finite_arrivals = 0;
  std::size_t finite_requireds = 0;
  std::size_t finite_slacks = 0;
  std::size_t path_states = 0;
  for (std::size_t rank = 0; rank < arcs.size(); ++rank) {
    const Arc& arc = arcs[rank];
    if (!points.contains(arc.get_source_pin()) || !points.contains(arc.get_sink_pin())) {
      errors.push_back("arc endpoint missing at rank " + std::to_string(rank));
    }
    if ((arc.get_type() == ArcType::kCell) != (arc.get_timing_cell_arc() != nullptr)) {
      errors.push_back("arc Liberty pointer/type mismatch at rank " + std::to_string(rank));
    }
    expected_out[arc.get_source_pin()].push_back(rank);
    expected_in[arc.get_sink_pin()].push_back(rank);
    if (std::isnan(arc.get_delay()) || std::isnan(arc.get_delay_min()) || std::isnan(arc.get_delay_max())
        || containsNan(arc.get_trans_delay_map())) {
      errors.push_back("arc numeric state contains NaN at rank " + std::to_string(rank));
    }
  }
  auto nonempty = [](const auto& rows) {
    std::map<std::string, std::vector<std::size_t>> result;
    for (const auto& [name, ranks] : rows) {
      if (!ranks.empty()) {
        result.emplace(name, ranks);
      }
    }
    return result;
  };
  if (nonempty(database.get_outgoing_arc_list_map()) != expected_out
      || nonempty(database.get_incoming_arc_list_map()) != expected_in) {
    errors.push_back("bidirectional adjacency differs from arc vector");
  }

  std::set<std::string> ordered(database.get_timing_order_list().begin(), database.get_timing_order_list().end());
  if (ordered.size() != points.size() || database.get_timing_order_list().size() != points.size()) {
    errors.push_back("timing order does not cover every point exactly once");
  }
  for (const auto& [name, point] : points) {
    if (!ordered.contains(name) || point.get_level() <= 0 || !validIndex(point.get_predecessor_arc_idx(), arcs.size())) {
      errors.push_back("timing point order/level/predecessor differs: " + name);
    }
    finite_arrivals += std::isfinite(point.get_arrival()) ? 1U : 0U;
    finite_requireds += std::isfinite(point.get_required()) ? 1U : 0U;
    finite_slacks += std::isfinite(point.get_slack()) ? 1U : 0U;
    if (std::isnan(point.get_arrival()) || std::isnan(point.get_required()) || std::isnan(point.get_slack())
        || std::isnan(point.get_launch_time()) || containsNan(point.get_data_slew_map())) {
      errors.push_back("timing point numeric state contains NaN: " + name);
    }
    for (const auto& [analysis, sources] : point.get_path_state_map()) {
      static_cast<void>(analysis);
      for (const auto& [source, transitions] : sources) {
        static_cast<void>(source);
        for (const auto& [transition, clocks] : transitions) {
          static_cast<void>(transition);
          for (const auto& [clock, state] : clocks) {
            static_cast<void>(clock);
            ++path_states;
            if (!validIndex(state.get_predecessor_arc_idx(), arcs.size()) || std::isnan(state.get_arrival())
                || std::isnan(state.get_slew()) || std::isnan(state.get_launch_time())
                || std::isnan(state.get_predecessor_arc_delay())) {
              errors.push_back("path state numeric/predecessor differs at point: " + name);
            }
          }
        }
      }
    }
  }
  if (finite_arrivals == 0 || finite_requireds == 0 || finite_slacks == 0 || path_states == 0) {
    errors.push_back("native electrical timing coverage is empty");
  }
  if (receipt.materialized_tokens.empty() || receipt.electrical_loads.empty()) {
    errors.push_back("replay receipt lacks materialization or electrical loads");
  }
  for (const ElectricalLoadObservation& load : receipt.electrical_loads) {
    if (!std::isfinite(load.load) || load.load < 0.0 || !pins.contains(load.output_pin)) {
      errors.push_back("electrical load observation is invalid: " + load.output_pin);
    }
  }
  for (const IndependentTokenNames& names : receipt.materialized_tokens) {
    if (!pins.contains(names.input_pin) || !pins.contains(names.output_pin) || !points.contains(names.input_pin)
        || !points.contains(names.output_pin)) {
      errors.push_back("materialized buffer is absent from rebuilt graph: " + names.instance);
    }
  }
  return errors;
}

IndependentOracleObservation captureOracleObservation(Database& database,
                                                      const IndependentReplayReceipt& receipt)
{
  IndependentOracleObservation result;
  result.raw = captureRawDesign(database);
  std::vector<std::string>& records = result.derived_field_records;
  appendPointRecords(records, database);
  appendArcRecords(records, database);
  appendAdjacency(records, "outgoing", database.get_outgoing_arc_list_map());
  appendAdjacency(records, "incoming", database.get_incoming_arc_list_map());
  appendStringVector(records, "start", "database", database.get_start_point_list());
  appendStringVector(records, "end", "database", database.get_end_point_list());
  appendStringVector(records, "order", "database", database.get_timing_order_list());
  for (const ElectricalLoadObservation& load : receipt.electrical_loads) {
    records.push_back(record("electrical_load", load.output_pin, integer(load.analysis), integer(load.transition),
                             doubleBits(load.load)));
  }
  const TASummary& summary = database.get_summary().ta_summary;
  records.push_back(record("timing_summary", integer(static_cast<std::int64_t>(summary.timing_path_num)),
                           integer(static_cast<std::int64_t>(summary.checked_end_point_num)),
                           integer(static_cast<std::int64_t>(summary.unconstrained_end_point_num)),
                           integer(static_cast<std::int64_t>(summary.violating_end_point_num)), doubleBits(summary.worst_slack),
                           doubleBits(summary.total_negative_slack), summary.worst_end_point));
  return result;
}

std::string canonicalOracleObservationText(const IndependentOracleObservation& observation)
{
  std::vector<std::string> records;
  records.push_back(record("schema", std::string("ista-timing-edit-independent-oracle/1.0.0")));
  records.push_back(record("design", observation.raw.design_name));
  for (const RawInstanceObservation& instance : observation.raw.instances) {
    records.push_back(record("instance", instance.map_name, integer(static_cast<std::int64_t>(instance.instance_id)),
                             instance.instance_name, instance.cell_name, instance.clock_pin, instance.output_pin,
                             instance.data_pin, boolean(instance.is_sequential), boolean(instance.is_clock_gating),
                             boolean(instance.has_clear), boolean(instance.has_preset), boolean(instance.is_io_cell)));
    appendStringVector(records, "instance_pin", instance.map_name, instance.pin_names);
  }
  for (const RawPinObservation& pin : observation.raw.pins) {
    records.push_back(record("pin", pin.map_name, pin.pin_name, pin.full_name, pin.instance_name, pin.net_name,
                             integer(pin.direction), doubleBits(pin.x), doubleBits(pin.y), boolean(pin.is_port)));
  }
  for (const RawNetObservation& net : observation.raw.nets) {
    records.push_back(record("net", net.map_name, net.net_name, net.driver_pin));
    appendStringVector(records, "net_driver", net.map_name, net.driver_pins);
    appendStringVector(records, "net_load", net.map_name, net.load_pins);
    appendStringVector(records, "net_pin", net.map_name, net.pin_names);
  }
  records.insert(records.end(), observation.derived_field_records.begin(), observation.derived_field_records.end());
  std::string output;
  for (const std::string& value : records) {
    output += value;
    output.push_back('\n');
  }
  return output;
}

}  // namespace ista::timing_edit::oracle
