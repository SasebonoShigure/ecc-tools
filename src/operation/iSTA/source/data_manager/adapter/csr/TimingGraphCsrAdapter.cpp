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
#include "TimingGraphCsrAdapter.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrGraphValidator.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "TimingGraphCsrAdapterInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::detail {
namespace {

using OrderedPinOwners = std::unordered_map<std::string, std::string>;

std::set<std::string> validateOrderedNodes(const std::vector<std::string>& names, const std::map<std::string, ista::Pin>& pins)
{
  std::set<std::string> result;
  for (const auto& name : names) {
    if (pins.find(name) == pins.end() || !result.emplace(name).second)
      malformed();
  }
  return result;
}

void validateNamedMaps(const ista::Database& database)
{
  for (const auto& [name, instance] : database.get_instance_map()) {
    if (name.empty() || name != instance.get_instance_name() || instance.get_cell_name().empty())
      malformed();
  }
  for (const auto& [name, net] : database.get_net_map()) {
    if (name.empty() || name != net.get_net_name())
      malformed();
  }
  for (const auto& [name, cell] : database.get_timing_library().get_cell_map()) {
    if (name.empty() || name != cell.get_cell_name())
      malformed();
  }
}

void validateTimingPointCoverage(const ista::Database& database)
{
  const auto& pins = database.get_pin_map();
  const auto& points = database.get_timing_point_map();
  if (pins.size() != points.size())
    malformed();
  for (const auto& [name, pin] : pins) {
    if (name != pin.get_full_name() || points.find(name) == points.end())
      malformed();
  }
  for (const auto& [name, point] : points) {
    static_cast<void>(point);
    if (pins.find(name) == pins.end())
      malformed();
  }
}

void validateRoleName(const std::string& role, const std::string& instance_name, const OrderedPinOwners& owners)
{
  if (role.empty())
    return;
  const auto owner = owners.find(role);
  if (owner == owners.end() || owner->second != instance_name)
    malformed();
}

OrderedPinOwners indexOrderedPinOwners(const ista::Database& database)
{
  OrderedPinOwners owners;
  owners.reserve(database.get_pin_map().size());
  for (const auto& [name, instance] : database.get_instance_map()) {
    for (const auto& pin_name : instance.get_pin_name_list()) {
      const auto pin = database.get_pin_map().find(pin_name);
      if (pin == database.get_pin_map().end() || pin->second.get_is_port() || pin->second.get_instance_name() != name
          || !owners.emplace(pin_name, name).second) {
        malformed();
      }
    }
    validateRoleName(instance.get_clock_pin_name(), name, owners);
    validateRoleName(instance.get_data_pin_name(), name, owners);
    validateRoleName(instance.get_output_pin_name(), name, owners);
  }
  return owners;
}

void assignCanonicalNodes(GraphBuildState& state, std::vector<NodeRecord> nodes)
{
  std::ranges::sort(nodes, [](const NodeRecord& lhs, const NodeRecord& rhs) { return nodeKeyLess(lhs.key, rhs.key); });
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (index > 0 && nodes[index - 1].key == nodes[index].key)
      collision();
    nodes[index].id = NodeId{index};
    if (!state.node_ids_by_full_name.emplace(nodes[index].key.full_name, NodeId{index}).second)
      collision();
    state.snapshot.node_ids_by_key.push_back({nodes[index].key, NodeId{index}});
  }
  state.snapshot.nodes = std::move(nodes);
}

InstanceRecord makeInstanceRecord(const GraphBuildState& state, const std::string& map_name, const ista::Instance& instance)
{
  return projectCanonicalInstanceRecord(state.database, map_name, instance, state.node_ids_by_full_name);
}

}  // namespace

PinDirection convertPinDirection(ista::PinDirection direction)
{
  switch (direction) {
    case ista::PinDirection::kNone:
      return PinDirection::kNone;
    case ista::PinDirection::kInput:
      return PinDirection::kInput;
    case ista::PinDirection::kOutput:
      return PinDirection::kOutput;
    case ista::PinDirection::kInout:
      return PinDirection::kInout;
  }
  malformed();
}

void exportNodesAndInstances(GraphBuildState& state)
{
  validateNamedMaps(state.database);
  validateTimingPointCoverage(state.database);
  state.ordered_pin_owners = indexOrderedPinOwners(state.database);
  const auto starts = validateOrderedNodes(state.database.get_start_point_list(), state.database.get_pin_map());
  const auto ends = validateOrderedNodes(state.database.get_end_point_list(), state.database.get_pin_map());

  std::vector<NodeRecord> nodes;
  nodes.reserve(state.database.get_pin_map().size());
  for (const auto& [name, pin] : state.database.get_pin_map())
    nodes.push_back(projectCanonicalNodeRecord(state.database, name, pin, starts, ends,
                                               state.ordered_pin_owners));
  assignCanonicalNodes(state, std::move(nodes));

  for (const auto& [name, instance] : state.database.get_instance_map())
    state.snapshot.instances.push_back(makeInstanceRecord(state, name, instance));
}

}  // namespace ista::csr::detail

namespace ista::csr {

CsrGraphExportResult exportCsrGraphSnapshot(const ista::Database& database)
{
  detail::noteNativeFullGraphExport();
  try {
    detail::GraphBuildState state(database);
    detail::exportNodesAndInstances(state);
    detail::exportArcs(state);
    detail::exportAdjacencyAndOrder(state);
    const auto violations = validateCsrGraphSnapshot(state.snapshot);
    if (!violations.empty())
      throw detail::GraphExportAbort(violations.front().code);
    return std::move(state.snapshot);
  } catch (const detail::GraphExportAbort& error) {
    return CsrGraphExportFailure{error.code()};
  }
}

}  // namespace ista::csr
