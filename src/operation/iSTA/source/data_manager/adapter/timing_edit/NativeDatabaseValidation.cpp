// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDatabaseValidation.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "Database.hpp"

namespace ista::timing_edit {
namespace {

NativeDatabaseValidationResult failure(NativeDatabaseValidationCode code, std::string object, std::string detail)
{
  return NativeDatabaseValidationFailure{code, std::move(object), std::move(detail)};
}

bool validPredecessor(std::size_t index, std::size_t arc_count)
{
  return index == std::numeric_limits<std::size_t>::max() || index < arc_count;
}

bool isDriver(const Pin& pin)
{
  return pin.get_is_port() ? pin.get_direction() == PinDirection::kInput : pin.get_direction() == PinDirection::kOutput;
}

NativeDatabaseValidationResult validateNamedObjects(const Database& database)
{
  for (const auto& [name, instance] : database.get_instance_map()) {
    if (name != instance.get_instance_name()) {
      return failure(NativeDatabaseValidationCode::kNamedObjectMismatch, name, "instance map key does not equal instance name");
    }
    std::set<std::string> pins;
    for (const auto& pin_name : instance.get_pin_name_list()) {
      if (!pins.insert(pin_name).second) {
        return failure(NativeDatabaseValidationCode::kMembershipMismatch, name, "instance pin membership contains a duplicate");
      }
      const auto pin = database.get_pin_map().find(pin_name);
      if (pin == database.get_pin_map().end() || pin->second.get_is_port() || pin->second.get_instance_name() != name) {
        return failure(NativeDatabaseValidationCode::kMembershipMismatch, name, "instance pin membership is not bidirectional");
      }
    }
  }

  std::size_t connected_pins = 0;
  for (const auto& [name, pin] : database.get_pin_map()) {
    if (name != pin.get_full_name()) {
      return failure(NativeDatabaseValidationCode::kNamedObjectMismatch, name, "pin map key does not equal full name");
    }
    if (!pin.get_is_port()) {
      const auto instance = database.get_instance_map().find(pin.get_instance_name());
      if (instance == database.get_instance_map().end()
          || std::ranges::find(instance->second.get_pin_name_list(), name) == instance->second.get_pin_name_list().end()) {
        return failure(NativeDatabaseValidationCode::kMembershipMismatch, name, "pin is missing from its instance");
      }
    }
    if (!pin.get_net_name().empty()) {
      const auto net = database.get_net_map().find(pin.get_net_name());
      if (net == database.get_net_map().end()) {
        return failure(NativeDatabaseValidationCode::kMembershipMismatch, name, "pin is missing from its net");
      }
      ++connected_pins;
    }
  }

  std::size_t listed_pins = 0;
  for (const auto& [name, net] : database.get_net_map()) {
    if (name != net.get_net_name()) {
      return failure(NativeDatabaseValidationCode::kNamedObjectMismatch, name, "net map key does not equal net name");
    }
    std::set<std::string> pins;
    for (const auto& pin_name : net.get_pin_name_list()) {
      if (!pins.insert(pin_name).second) {
        return failure(NativeDatabaseValidationCode::kMembershipMismatch, name, "net pin membership contains a duplicate");
      }
      const auto pin = database.get_pin_map().find(pin_name);
      if (pin == database.get_pin_map().end() || pin->second.get_net_name() != name) {
        return failure(NativeDatabaseValidationCode::kMembershipMismatch, name, "net pin membership is not bidirectional");
      }
    }

    listed_pins += pins.size();

    std::set<std::string> expected_drivers;
    std::set<std::string> expected_loads;
    for (const auto& pin_name : pins) {
      const Pin& pin = database.get_pin_map().at(pin_name);
      (isDriver(pin) ? expected_drivers : expected_loads).insert(pin_name);
    }
    const std::set<std::string> cached_drivers(net.get_driver_pin_list().begin(), net.get_driver_pin_list().end());
    const std::set<std::string> cached_loads(net.get_load_pin_list().begin(), net.get_load_pin_list().end());
    if (cached_drivers.size() != net.get_driver_pin_list().size() || cached_loads.size() != net.get_load_pin_list().size()
        || cached_drivers != expected_drivers || cached_loads != expected_loads) {
      return failure(NativeDatabaseValidationCode::kMembershipMismatch, name,
                     "net driver/load caches are not the exact direction-classified raw membership");
    }
    if (expected_drivers.size() > 1) {
      return failure(NativeDatabaseValidationCode::kMembershipMismatch, name, "net has multiple direction-classified drivers");
    }
    const std::string expected_driver = expected_drivers.empty() ? std::string{} : *expected_drivers.begin();
    if (net.get_driver_pin() != expected_driver) {
      return failure(NativeDatabaseValidationCode::kMembershipMismatch, name, "net singular driver cache is inconsistent");
    }
  }
  // Unique forward memberships plus equal cardinality prove reverse coverage without rescanning each net per pin.
  if (listed_pins != connected_pins) {
    return failure(NativeDatabaseValidationCode::kMembershipMismatch, "net_map", "connected pins are missing from net membership");
  }
  return std::nullopt;
}

NativeDatabaseValidationResult validateTimingPoints(const Database& database)
{
  const auto& pins = database.get_pin_map();
  const auto& points = database.get_timing_point_map();
  if (pins.size() != points.size()) {
    return failure(NativeDatabaseValidationCode::kTimingPointCoverage, "timing_point_map", "timing point count differs from pin count");
  }
  auto pin = pins.begin();
  auto point = points.begin();
  for (; pin != pins.end(); ++pin, ++point) {
    if (pin->first != point->first) {
      return failure(NativeDatabaseValidationCode::kTimingPointCoverage, point->first, "timing point keys are not the exact pin keys");
    }
  }
  return std::nullopt;
}

bool pointerBelongsToCell(const TimingCellArc* pointer, const TimingCell& cell)
{
  return std::ranges::any_of(cell.get_cell_arc_list(), [pointer](const TimingCellArc& arc) { return &arc == pointer; });
}

NativeDatabaseValidationResult validateArcs(const Database& database)
{
  const auto& arcs = database.get_arc_list();
  const auto& pins = database.get_pin_map();
  for (std::size_t index = 0; index < arcs.size(); ++index) {
    const auto& arc = arcs[index];
    const auto source = pins.find(arc.get_source_pin());
    const auto sink = pins.find(arc.get_sink_pin());
    if (source == pins.end() || sink == pins.end()) {
      return failure(NativeDatabaseValidationCode::kInvalidArcEndpoint, std::to_string(index), "arc endpoint does not resolve to a pin");
    }
    if (arc.get_type() == ArcType::kCell) {
      const auto instance = database.get_instance_map().find(arc.get_owner_name());
      if (instance == database.get_instance_map().end() || source->second.get_instance_name() != arc.get_owner_name()
          || sink->second.get_instance_name() != arc.get_owner_name()) {
        return failure(NativeDatabaseValidationCode::kInvalidCellArcPointer, std::to_string(index), "cell arc owner is inconsistent");
      }
      const auto cell = database.get_timing_library().get_cell_map().find(instance->second.get_cell_name());
      const TimingCellArc* pointer = arc.get_timing_cell_arc();
      if (cell == database.get_timing_library().get_cell_map().end() || pointer == nullptr || !pointerBelongsToCell(pointer, cell->second)
          || pointer->get_source_port() != arc.get_library_source_port() || pointer->get_sink_port() != arc.get_library_sink_port()) {
        return failure(NativeDatabaseValidationCode::kInvalidCellArcPointer, std::to_string(index),
                       "cell arc pointer is not owned by the candidate TimingLibrary");
      }
    } else if (arc.get_type() == ArcType::kNet) {
      if (arc.get_timing_cell_arc() != nullptr) {
        return failure(NativeDatabaseValidationCode::kInvalidNetArcPointer, std::to_string(index), "net arc retains a TimingCellArc pointer");
      }
    } else {
      return failure(NativeDatabaseValidationCode::kInvalidArcEndpoint, std::to_string(index), "arc kind is none");
    }
  }
  return std::nullopt;
}

NativeDatabaseValidationResult validateAdjacency(const Database& database)
{
  const auto& arcs = database.get_arc_list();
  std::vector<std::size_t> outgoing_count(arcs.size(), 0);
  std::vector<std::size_t> incoming_count(arcs.size(), 0);
  for (const auto& [pin_name, indices] : database.get_outgoing_arc_list_map()) {
    if (!database.get_pin_map().contains(pin_name)) {
      return failure(NativeDatabaseValidationCode::kInvalidAdjacencyIndex, pin_name, "outgoing row does not resolve to a pin");
    }
    for (const std::size_t index : indices) {
      if (index >= arcs.size() || arcs[index].get_source_pin() != pin_name || ++outgoing_count[index] != 1) {
        return failure(NativeDatabaseValidationCode::kInvalidAdjacencyIndex, pin_name, "outgoing arc index is invalid or duplicated");
      }
    }
  }
  for (const auto& [pin_name, indices] : database.get_incoming_arc_list_map()) {
    if (!database.get_pin_map().contains(pin_name)) {
      return failure(NativeDatabaseValidationCode::kInvalidAdjacencyIndex, pin_name, "incoming row does not resolve to a pin");
    }
    for (const std::size_t index : indices) {
      if (index >= arcs.size() || arcs[index].get_sink_pin() != pin_name || ++incoming_count[index] != 1) {
        return failure(NativeDatabaseValidationCode::kInvalidAdjacencyIndex, pin_name, "incoming arc index is invalid or duplicated");
      }
    }
  }
  for (std::size_t index = 0; index < arcs.size(); ++index) {
    if (outgoing_count[index] != 1 || incoming_count[index] != 1) {
      return failure(NativeDatabaseValidationCode::kInvalidAdjacencyIndex, std::to_string(index), "arc is not present in both adjacency families");
    }
  }
  return std::nullopt;
}

NativeDatabaseValidationResult validatePredecessorsAndOrder(const Database& database)
{
  const std::size_t arc_count = database.get_arc_list().size();
  for (const auto& [pin_name, point] : database.get_timing_point_map()) {
    if (!validPredecessor(point.get_predecessor_arc_idx(), arc_count)) {
      return failure(NativeDatabaseValidationCode::kInvalidPredecessorIndex, pin_name, "timing point predecessor index is out of range");
    }
    for (const auto& [analysis, sources] : point.get_path_state_map()) {
      static_cast<void>(analysis);
      for (const auto& [source_type, transitions] : sources) {
        static_cast<void>(source_type);
        for (const auto& [transition, states] : transitions) {
          static_cast<void>(transition);
          for (const auto& [tag, state] : states) {
            if (!validPredecessor(state.get_predecessor_arc_idx(), arc_count)) {
              return failure(NativeDatabaseValidationCode::kInvalidPredecessorIndex, pin_name + "/" + tag,
                             "path state predecessor index is out of range");
            }
          }
        }
      }
    }
  }

  std::set<std::string> ordered;
  std::map<std::string, std::size_t> order_position;
  for (const auto& pin_name : database.get_timing_order_list()) {
    if (!database.get_timing_point_map().contains(pin_name) || !ordered.insert(pin_name).second) {
      return failure(NativeDatabaseValidationCode::kInvalidTimingOrder, pin_name, "timing order contains an invalid or duplicate point");
    }
    order_position.emplace(pin_name, order_position.size());
  }
  if (ordered.size() != database.get_timing_point_map().size()) {
    return failure(NativeDatabaseValidationCode::kInvalidTimingOrder, "timing_order_list",
                   "timing order must exactly cover all timing points once");
  }
  for (std::size_t index = 0; index < database.get_arc_list().size(); ++index) {
    const Arc& arc = database.get_arc_list()[index];
    if (arc.get_is_disable_arc() || arc.get_is_loop_disable()) {
      continue;
    }
    const std::size_t source_position = order_position.at(arc.get_source_pin());
    const std::size_t sink_position = order_position.at(arc.get_sink_pin());
    if (source_position >= sink_position) {
      return failure(NativeDatabaseValidationCode::kInvalidTimingOrder, std::to_string(index),
                     "enabled arc does not advance in timing order");
    }
    const TimingPoint& source = database.get_timing_point_map().at(arc.get_source_pin());
    const TimingPoint& sink = database.get_timing_point_map().at(arc.get_sink_pin());
    if (source.get_level() >= sink.get_level()) {
      return failure(NativeDatabaseValidationCode::kInvalidTimingOrder, std::to_string(index),
                     "enabled arc does not advance in timing level");
    }
  }
  for (const auto* list : {&database.get_start_point_list(), &database.get_end_point_list()}) {
    std::set<std::string> unique;
    for (const auto& pin_name : *list) {
      if (!database.get_timing_point_map().contains(pin_name) || !unique.insert(pin_name).second) {
        return failure(NativeDatabaseValidationCode::kInvalidTimingOrder, pin_name, "start/end list contains an invalid or duplicate point");
      }
    }
  }
  return std::nullopt;
}

}  // namespace

NativeDatabaseValidationResult validateNativeDatabase(const Database& database)
{
  for (const auto validator : {validateNamedObjects, validateTimingPoints, validateArcs, validateAdjacency, validatePredecessorsAndOrder}) {
    if (auto result = validator(database); result.has_value()) {
      return result;
    }
  }
  return std::nullopt;
}

void requireValidNativeDatabase(const Database& database)
{
  if (auto result = validateNativeDatabase(database); result.has_value()) {
    throw std::runtime_error("native Database validation failed at " + result->object + ": " + result->detail);
  }
}

}  // namespace ista::timing_edit
