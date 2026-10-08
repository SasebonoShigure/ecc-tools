// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalTimingEdit.hpp"
#include "IdbTimingPinName.hpp"
#include "TimingEditValidation.hpp"
#include "Database.hpp"
#include <map>
#include <stdexcept>

namespace ista {
using izh::PreparedPhysicalBatch;
PreparedPhysicalBatch preparePhysicalTimingEdit(idb::IdbDesign& design,
    const ista::timing_edit::EditBatch& edits, ista::timing_edit::TimingEditResolverView resolver,
    std::span<const PhysicalBufferLocation> locations, std::int32_t dbu_per_um)
{
  using namespace ista::timing_edit;
  if (dbu_per_um <= 0 || locations.size() != edits.edits.size() || !design.get_layout())
    throw std::invalid_argument("physical batch location count or DBU is invalid");
  std::map<CreationToken, const PhysicalBufferLocation*> positions;
  for (const auto& location : locations)
    if (!positions.emplace(location.token, &location).second)
      throw std::invalid_argument("physical batch repeats a location token");
  std::vector<izh::PhysicalBufferInsertion> physical;
  physical.reserve(edits.edits.size());
  for (const auto& edit : edits.edits) {
    const auto found = positions.find(edit.creation_token);
    const auto* placement = std::get_if<PlacedInputs>(&edit.placement_inputs);
    const auto* rc = std::get_if<ParasiticRcInputs>(&edit.rc_inputs);
    if (found == positions.end() || !placement || !rc || rc->parasitic_policy != ParasiticEditPolicy::kPreRouteTree)
      throw std::invalid_argument("physical batch requires a located pre-route edit");
    const auto& location = *found->second;
    if (placement->x_microns != location.x_dbu / static_cast<double>(dbu_per_um)
        || placement->y_microns != location.y_dbu / static_cast<double>(dbu_per_um))
      throw std::invalid_argument("physical location disagrees with semantic placement");
    auto* net = design.get_net_list()->find_net(resolver.resolveNet(edit.original_net));
    if (!net) throw std::invalid_argument("physical source net does not exist");
    std::map<std::string, idb::IdbPin*> pins;
    for (auto* list : {net->get_io_pins(), net->get_instance_pin_list()})
      for (auto* pin : list->get_pin_list())
        if (!pin || !pins.emplace(ista::idbTimingPinName(*pin), pin).second)
          throw std::invalid_argument("physical net has missing or ambiguous terminal identity");
    std::vector<idb::IdbPin*> moved;
    moved.reserve(edit.moved_sinks.size());
    for (const auto& sink : edit.moved_sinks) {
      const auto pin = pins.find(resolver.resolveNode(sink));
      if (pin == pins.end()) throw std::invalid_argument("physical moved sink is stale");
      moved.push_back(pin->second);
    }
    const auto names = defaultTokenMaterialization(edit.creation_token, edit.library_cell);
    physical.push_back({net, design.get_layout()->get_cell_master_list()->find_cell_master(edit.library_cell.cell_name),
        std::move(moved), names.instance_name, names.output_net_name, edit.library_cell.input_port, edit.library_cell.output_port,
        location.x_dbu, location.y_dbu, location.orientation});
  }
  return PreparedPhysicalBatch(design, physical);
}
void copyCreatedPhysicalPinCoordinates(const PreparedPhysicalBatch& physical, ista::Database& database)
{
  if (!physical.applied()) throw std::logic_error("physical coordinate copy requires an applied batch");
  for (const auto& instance : physical.instances()) {
    for (auto* pin : instance->get_pin_list()->get_pin_list()) {
      if (!pin->get_net()) continue;  // Unconnected power/ground ports have no timing node.
      const auto found = database.get_pin_map().find(ista::idbTimingPinName(*pin));
      if (found == database.get_pin_map().end() || found->second.get_instance_name() != instance->get_name()
          || found->second.get_net_name() != pin->get_net_name())
        throw std::invalid_argument("new physical/native pin binding mismatch");
      found->second.set_x(pin->get_average_coordinate()->get_x());
      found->second.set_y(pin->get_average_coordinate()->get_y());
    }
  }
}
}  // namespace ista
