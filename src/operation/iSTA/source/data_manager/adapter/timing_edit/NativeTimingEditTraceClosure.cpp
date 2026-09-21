// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeTimingEditPreparer.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Database.hpp"
#include "NativeBufferCellValidation.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditValidation.hpp"

namespace ista::timing_edit {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("native timing edit prepare: " + std::move(message));
}

bool isDriver(const Pin& pin)
{
  return pin.get_is_port() ? pin.get_direction() == PinDirection::kInput
                           : pin.get_direction() == PinDirection::kOutput;
}

template <typename Value>
void requireExactMapKeys(const std::map<std::string, Value>& actual,
                         const std::set<std::string>& expected,
                         std::string_view kind)
{
  if (actual.size() != expected.size()) {
    invalid(std::string(kind)
            + " raw object count differs from baseline plus committed materializations");
  }
  for (const std::string& name : expected) {
    if (!actual.contains(name)) {
      invalid(std::string(kind) + " raw object set is missing an expected name");
    }
  }
}

std::set<std::string> requireUniqueNames(const std::vector<std::string>& names,
                                         std::string_view field)
{
  std::set<std::string> unique(names.begin(), names.end());
  if (unique.size() != names.size()) {
    invalid(std::string(field) + " contains a duplicate native name");
  }
  return unique;
}

void requireRawTraceClosure(const Database& database,
                            const TimingEditResolverState& resolver,
                            const BaselineSemanticIdentity& baseline,
                            std::span<const EditBatch> committed_trace,
                            std::span<const BufferInsertionMutation> pending_edits)
{
  std::map<CreationToken, const BufferInsertionMutation*> edits_by_token;
  std::set<std::string> expected_instances(baseline.canonical_pre_state.instances.begin(),
                                           baseline.canonical_pre_state.instances.end());
  std::set<std::string> expected_nets(baseline.canonical_pre_state.nets.begin(),
                                      baseline.canonical_pre_state.nets.end());
  std::set<std::string> expected_pins;
  for (const csr::NodeKey& node : baseline.canonical_pre_state.nodes) {
    if (!expected_pins.insert(node.full_name).second) {
      invalid("baseline node evidence contains duplicate native pin names");
    }
  }

  std::map<std::string, std::string> expected_net_by_affected_pin;
  const auto count = committed_trace.size() + (pending_edits.empty() ? 0 : 1);
  for (std::size_t generation = 0; generation < count; ++generation) {
    if (generation < committed_trace.size()
        && committed_trace[generation].expected_generation != TransactionGeneration(generation))
      invalid("committed trace envelope is inconsistent while checking raw closure");
    const std::span<const BufferInsertionMutation> edits
        = generation < committed_trace.size() ? committed_trace[generation].edits : pending_edits;
    for (const BufferInsertionMutation& edit : edits) {
      if (!edits_by_token.emplace(edit.creation_token, &edit).second) {
        invalid("committed trace reuses a creation token");
      }
      const TokenMaterialization& materialization
          = resolver.materialization(edit.creation_token);
      if (materialization
          != defaultTokenMaterialization(edit.creation_token, edit.library_cell)) {
        invalid("resolver materialization does not exactly match its committed semantic edit");
      }
      if (!expected_instances.insert(materialization.instance_name).second
          || !expected_nets.insert(materialization.output_net_name).second
          || !expected_pins.insert(materialization.input_pin_name).second
          || !expected_pins.insert(materialization.output_pin_name).second) {
        invalid("committed materialization collides with a baseline or earlier raw object");
      }

      const std::string source_net = resolver.resolveNet(edit.original_net);
      expected_net_by_affected_pin[materialization.input_pin_name] = source_net;
      expected_net_by_affected_pin[materialization.output_pin_name]
          = materialization.output_net_name;
      for (const StableNodeRef& moved_sink : edit.moved_sinks) {
        expected_net_by_affected_pin[resolver.resolveNode(moved_sink)]
            = materialization.output_net_name;
      }
    }
  }

  if (edits_by_token.size() != resolver.materializations().size()) {
    invalid("resolver materialization set differs from the complete committed trace");
  }
  for (const auto& [token, materialization] : resolver.materializations()) {
    static_cast<void>(materialization);
    if (!edits_by_token.contains(token)) {
      invalid("resolver contains a materialization absent from the committed trace");
    }
  }

  requireExactMapKeys(database.get_instance_map(), expected_instances, "instance");
  requireExactMapKeys(database.get_net_map(), expected_nets, "net");
  requireExactMapKeys(database.get_pin_map(), expected_pins, "pin");

  for (const auto& [token, materialization] : resolver.materializations()) {
    const BufferInsertionMutation& edit = *edits_by_token.at(token);
    const TimingCell& cell = requireLegalNativeBufferCell(database, edit.library_cell);
    const auto instance_position
        = database.get_instance_map().find(materialization.instance_name);
    const auto input_position = database.get_pin_map().find(materialization.input_pin_name);
    const auto output_position
        = database.get_pin_map().find(materialization.output_pin_name);
    const auto output_net_position
        = database.get_net_map().find(materialization.output_net_name);
    if (instance_position == database.get_instance_map().end()
        || input_position == database.get_pin_map().end()
        || output_position == database.get_pin_map().end()
        || output_net_position == database.get_net_map().end()) {
      invalid("committed materialization is missing a raw native object");
    }

    const Instance& instance = instance_position->second;
    std::vector<std::string> expected_instance_pins{materialization.input_pin_name,
                                                    materialization.output_pin_name};
    std::ranges::sort(expected_instance_pins);
    if (instance.get_instance_name() != materialization.instance_name
        || instance.get_cell_name() != edit.library_cell.cell_name
        || instance.get_pin_name_list() != expected_instance_pins
        || instance.get_output_pin_name() != materialization.output_pin_name
        || instance.get_is_sequential() != cell.get_is_sequential()
        || instance.get_is_clock_gating() != cell.get_is_clock_gating()
        || instance.get_has_clear_arc() != cell.get_has_clear_arc()
        || instance.get_has_preset_arc() != cell.get_has_preset_arc()
        || instance.get_is_io_cell()) {
      invalid("materialized raw instance does not exactly match its library-backed semantic edit");
    }

    const Pin& input_pin = input_position->second;
    const Pin& output_pin = output_position->second;
    if (input_pin.get_full_name() != materialization.input_pin_name
        || input_pin.get_pin_name() != edit.library_cell.input_port
        || input_pin.get_instance_name() != materialization.instance_name
        || input_pin.get_direction() != PinDirection::kInput || input_pin.get_is_port()
        || output_pin.get_full_name() != materialization.output_pin_name
        || output_pin.get_pin_name() != edit.library_cell.output_port
        || output_pin.get_instance_name() != materialization.instance_name
        || output_pin.get_direction() != PinDirection::kOutput || output_pin.get_is_port()) {
      invalid("materialized raw pins do not exactly match their selected library ports");
    }
    const auto* placed = std::get_if<PlacedInputs>(&edit.placement_inputs);
    const double expected_x = placed == nullptr ? 0.0 : placed->x_microns;
    const double expected_y = placed == nullptr ? 0.0 : placed->y_microns;
    if (input_pin.get_x() != expected_x || input_pin.get_y() != expected_y
        || output_pin.get_x() != expected_x || output_pin.get_y() != expected_y) {
      invalid("materialized raw pin placement differs from the committed semantic edit");
    }
  }

  for (const auto& [name, instance] : database.get_instance_map()) {
    if (instance.get_instance_name() != name) {
      invalid("instance map key and embedded name differ");
    }
    const std::set<std::string> member_pins
        = requireUniqueNames(instance.get_pin_name_list(), "instance.pin_name_list");
    for (const std::string& pin_name : member_pins) {
      const auto pin = database.get_pin_map().find(pin_name);
      if (pin == database.get_pin_map().end() || pin->second.get_is_port()
          || pin->second.get_instance_name() != name) {
        invalid("instance-to-pin raw membership is not bidirectional");
      }
    }
  }

  for (const auto& [name, net] : database.get_net_map()) {
    if (net.get_net_name() != name) {
      invalid("net map key and embedded name differ");
    }
    static_cast<void>(requireUniqueNames(net.get_pin_name_list(), "net.pin_name_list"));
    std::vector<std::string> drivers;
    std::vector<std::string> loads;
    for (const std::string& pin_name : net.get_pin_name_list()) {
      const auto pin = database.get_pin_map().find(pin_name);
      if (pin == database.get_pin_map().end() || pin->second.get_net_name() != name) {
        invalid("net-to-pin raw membership is not bidirectional");
      }
      (isDriver(pin->second) ? drivers : loads).push_back(pin_name);
    }
    std::ranges::sort(drivers);
    std::ranges::sort(loads);
    const std::string expected_driver = drivers.empty() ? std::string{} : drivers.front();
    if (net.get_driver_pin() != expected_driver || net.get_driver_pin_list() != drivers
        || net.get_load_pin_list() != loads) {
      invalid("net driver/load closure does not exactly match raw pin membership");
    }
  }

  for (const auto& [name, pin] : database.get_pin_map()) {
    if (pin.get_full_name() != name) {
      invalid("pin map key and embedded full name differ");
    }
    if (!pin.get_is_port()) {
      const auto instance = database.get_instance_map().find(pin.get_instance_name());
      if (instance == database.get_instance_map().end()
          || std::ranges::count(instance->second.get_pin_name_list(), name) != 1) {
        invalid("pin-to-instance raw membership is not bidirectional");
      }
    }
    if (!pin.get_net_name().empty()) {
      const auto net = database.get_net_map().find(pin.get_net_name());
      if (net == database.get_net_map().end()
          || std::ranges::count(net->second.get_pin_name_list(), name) != 1) {
        invalid("pin-to-net raw membership is not bidirectional");
      }
    }
  }

  std::map<std::string, std::set<std::string>> expected_created_net_members;
  for (const auto& [token, materialization] : resolver.materializations()) {
    static_cast<void>(token);
    expected_created_net_members.emplace(materialization.output_net_name,
                                         std::set<std::string>{});
  }
  for (const auto& [pin_name, net_name] : expected_net_by_affected_pin) {
    const auto pin = database.get_pin_map().find(pin_name);
    if (pin == database.get_pin_map().end() || pin->second.get_net_name() != net_name) {
      invalid("final affected-pin connectivity differs from committed trace order");
    }
    const auto created_net = expected_created_net_members.find(net_name);
    if (created_net != expected_created_net_members.end()) {
      created_net->second.insert(pin_name);
    }
  }
  for (const auto& [net_name, expected_members] : expected_created_net_members) {
    const auto net = database.get_net_map().find(net_name);
    if (net == database.get_net_map().end()
        || requireUniqueNames(net->second.get_pin_name_list(),
                              "materialized_output_net.pin_name_list")
               != expected_members) {
      invalid("materialized output-net membership differs from the complete committed trace");
    }
  }
}

}  // namespace

void NativeTimingEditPreparer::requireExactCommittedTraceClosure(
    const Database& active_database, const csr::CsrReferenceInput& current_input,
    const TimingEditResolverState& resolver, const BaselineSemanticIdentity& baseline,
    std::span<const EditBatch> committed_trace,
                            std::span<const BufferInsertionMutation> pending_edits)
{
  requireRawTraceClosure(active_database, resolver, baseline, committed_trace, pending_edits);
  NativeTimingEditPreparer::requireExactResolverCoverage(active_database, current_input,
                                                         resolver);
}

}  // namespace ista::timing_edit
