// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <type_traits>

#include "Arc.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "Database.hpp"
#include "NativeTargetedEditInternal.hpp"

namespace ista::timing_edit::detail {
PreparedNativeTargetedMetadata::PreparedNativeTargetedMetadata(
    NativeTargetedEditState& state, const AppendOnlySlotPlan& slots,
    const std::vector<TokenMaterialization>& materializations)
    : _state(state), _resolver_patch(state.resolver, state.resolver.prepareAssignmentsDelta(slots, materializations)),
      _generation(state.generation.value() + 1), _node_count(state.node_names.size()),
      _arc_count(state.arc_native_ranks.size()), _native_count(state.native_arc_ids.size()),
      _out_count(state.base_out_offsets.size()), _in_count(state.base_in_offsets.size())
{
  _names.resize(slots.after.node_slots - slots.before.node_slots);
  for (const auto& assignment : slots.nodes) {
    if (assignment.slot.value() < _node_count || assignment.tombstone)
      throw std::invalid_argument("targeted metadata requires appended live nodes");
    const auto& name = _resolver_patch.view().resolveNode(assignment.ref);
    _names.at(assignment.slot.value() - _node_count) = name;
    if (state.node_ids.contains(name) || !_node_ids.emplace(name, csr::NodeId{assignment.slot.value()}).second)
      throw std::invalid_argument("targeted metadata node name collision");
  }
  _ids.reserve(_node_ids.size());
  for (auto it = _node_ids.begin(); it != _node_ids.end(); ++it)
    _ids.push_back(it);
  for (const auto& assignment : slots.arcs) {
    const auto id = assignment.slot.value();
    if (id >= _arc_count) {
      _new_arc_refs.emplace_back(id, assignment.ref);
      continue;
    }
    const auto rank = state.arc_native_ranks.at(id);
    _old_ranks.emplace_back(id, rank);
    if (rank)
      _old_native_ids.emplace_back(*rank, state.native_arc_ids.at(*rank));
  }
  reserveTargetedAppend(state.node_names, slots.after.node_slots);
  reserveTargetedAppend(state.arc_native_ranks, slots.after.arc_slots);
  reserveTargetedAppend(state.arc_refs, slots.after.arc_slots);
  reserveTargetedAppend(state.native_arc_ids, _native_count + slots.nodes.size());
  reserveTargetedAppend(state.base_out_offsets, _out_count + slots.nodes.size());
  reserveTargetedAppend(state.base_in_offsets, _in_count + slots.nodes.size());
}

void PreparedNativeTargetedMetadata::apply() noexcept
{
  _resolver_patch.apply();
  const auto& resolver = _state.resolver;
  std::swap(_state.generation, _generation);
  _state.node_names.resize(resolver.highWater().node_slots);
  _state.arc_native_ranks.resize(resolver.highWater().arc_slots);
  _state.arc_refs.resize(resolver.highWater().arc_slots);
  for (const auto& [id, ref] : _new_arc_refs)
    _state.arc_refs[id] = &resolver.arcBindings().find(ref)->first;
  for (std::size_t i = 0; i < _names.size(); ++i)
    _state.node_names[_node_count + i].swap(_names[i]);
  _state.node_ids.merge(_node_ids);
  _applied = true;
}
void PreparedNativeTargetedMetadata::undo() noexcept
{
  if (!_applied)
    return;
  const auto erase_inserted = [](auto& target, const auto& inserted) {
    for (auto it : inserted)
      target.erase(it);
  };
  for (auto& [node, previous] : _parasitics)
    std::swap(_state.parasitics.at(node), previous);
  for (const auto node : _new_parasitics)
    _state.parasitics.erase(node);
  _parasitics.clear();
  _new_parasitics.clear();
  _resolver_patch.rollback();
  erase_inserted(_state.node_ids, _ids);
  std::swap(_state.generation, _generation);
  for (const auto& [rank, id] : _old_native_ids)
    _state.native_arc_ids[rank] = id;
  for (const auto& [id, rank] : _old_ranks)
    _state.arc_native_ranks[id] = rank;
  _state.node_names.resize(_node_count);
  _state.arc_native_ranks.resize(_arc_count);
  _state.arc_refs.resize(_arc_count);
  _state.native_arc_ids.resize(_native_count);
  _state.base_out_offsets.resize(_out_count);
  _state.base_in_offsets.resize(_in_count);
  _applied = false;
}

csr::NodeRecord targetedNode(const Database& database, const NativeTargetedEditState& state, const std::string& name,
                             const std::set<std::string>& starts, const std::set<std::string>& ends)
{
  const auto& pin = database.get_pin_map().at(name);
  std::unordered_map<std::string, std::string> owners;
  if (!pin.get_is_port())
    owners.emplace(name, pin.get_instance_name());
  auto node = csr::detail::projectCanonicalNodeRecord(database, name, pin, starts, ends, owners);
  node.id = state.node_ids.at(name);
  if (!node.is_port) {
    state.models->requireDatabase(database);
    const auto& cell = state.models->cell(node.instance_cell);
    csr::detail::applyCanonicalNodeLibraryRoles(cell, node);
  }
  return node;
}

csr::ArcOutputLoadMatrix targetedNetLoad(const Database& database, const NativeTargetedEditState& state, const std::string& name,
                             const std::set<std::string>& starts, const std::set<std::string>& ends)
{
  if (name.empty())
    return {};
  const auto& net = database.get_net_map().at(name);
  std::map<std::string, csr::detail::CanonicalLoadPinFacts> facts;
  for (const auto& pin : net.get_load_pin_list()) {
    const auto node = targetedNode(database, state, pin, starts, ends);
    csr::detail::CanonicalLoadPinFacts fact;
    fact.key = node.key;
    fact.direction = node.direction;
    fact.is_port = node.is_port;
    if (node.is_port) {
      const auto& constraints = database.get_timing_constraint().get_port_constraint_map();
      const auto constraint = constraints.find(pin);
      if (constraint != constraints.end() && constraint->second.get_has_load())
        fact.port_output_load = constraint->second.get_load();
    } else {
      fact.cell_pin_capacitance = csr::detail::projectCanonicalCellPinCapacitance(database, node.id, node.key);
    }
    facts.emplace(pin, std::move(fact));
  }
  return csr::detail::projectCanonicalNoParasiticNetLoad(database, name, facts);
}

csr::ArcSemanticPayload targetedArc(const Database& database, const NativeTargetedEditState& state, std::size_t rank,
                                    std::map<std::string, csr::ArcOutputLoadMatrix>& loads,
                                     const std::set<std::string>& starts, const std::set<std::string>& ends)
{
  const auto& arc = database.get_arc_list().at(rank);
  std::vector<csr::NodeRecord> nodes{targetedNode(database, state, arc.get_source_pin(), starts, ends), targetedNode(database, state, arc.get_sink_pin(), starts, ends)};
  std::map<std::string, csr::NodeId> local_ids{{arc.get_source_pin(), csr::NodeId{0}}, {arc.get_sink_pin(), csr::NodeId{1}}};
  std::unordered_map<std::string, std::string> owners;
  for (const auto& node : nodes)
    if (!node.is_port)
      owners.emplace(node.key.full_name, node.key.instance_name);
  const csr::detail::CanonicalArcKeyContext context{database, nodes, local_ids, owners};
  csr::ArcRecord record;
  record.key = {csr::detail::projectCanonicalArcKeyBase(context, arc), 0};
  // Data-only edits preserve parallel groups. Replaced native slots are rebound
  // to their new stable reference before projecting the updated graph.
  const auto* stable_ref = state.arc_refs.at(state.native_arc_ids.at(rank).value());
  if (stable_ref == nullptr)
    throw std::invalid_argument("targeted arc reference is absent");
  record.key.parallel_ordinal = std::visit([](const auto& ref) {
    using Ref = std::decay_t<decltype(ref)>;
    if constexpr (std::is_same_v<Ref, ExistingArcRef>)
      return ref.baseline_key.parallel_ordinal;
    else if constexpr (std::is_same_v<Ref, CreatedCellArcRef>)
      return ref.stable_equivalence_ordinal;
    else
      return ref.stable_ordinal;
  }, *stable_ref);
  record.source = nodes[0].id;
  record.sink = nodes[1].id;
  record.kind = record.key.base.kind;
  record.is_clock_arc = arc.get_is_clock_arc();
  record.is_disabled = arc.get_is_disable_arc();
  record.is_loop_disabled = arc.get_is_loop_disable();
  // A data-net load change refreshes the existing CTQ payload feeding Q.
  // It does not edit the clock network or add a clock arc.
  const bool clock_to_q = record.kind == csr::ArcKind::kCell && nodes[0].is_sequential && nodes[0].is_clock_pin
                          && nodes[1].is_sequential && !nodes[1].is_clock_pin
                          && nodes[0].key.instance_name == nodes[1].key.instance_name;
  if ((record.is_clock_arc && !clock_to_q) || (nodes[1].is_sequential && nodes[1].is_clock_pin))
    throw std::invalid_argument("targeted buffer edit touches a clock arc");
  const bool clock_eligible = state.clock_arcs->contains(state.native_arc_ids.at(rank));
  record.eligibility = csr::detail::projectCanonicalArcEligibility(record, nodes[1], false, clock_eligible);
  csr::detail::projectCanonicalArcTransfer(database, *state.models, record, arc);
  if (database.get_parasitic_library().get_net_map().contains(nodes[1].net_name)) {
    record.parasitic = state.parasitics.at(record.sink);
    const auto& tree = record.parasitic.tree;
    if (!tree || tree->net_name != nodes[1].net_name || record.parasitic.terminal >= tree->terminals.size()
        || tree->terminals[record.parasitic.terminal] != record.sink
        || (record.kind == csr::ArcKind::kCell ? record.sink : record.source) != tree->terminals.front())
      throw std::invalid_argument("targeted RC payload has a stale driver or terminal");
  } else if (record.kind == csr::ArcKind::kCell) {
    auto load = loads.find(nodes[1].net_name);
    if (load == loads.end())
      load = loads.emplace(nodes[1].net_name, targetedNetLoad(database, state, nodes[1].net_name, starts, ends)).first;
    record.output_load = load->second;
  }
  return {record.key,
          record.source,
          record.sink,
          record.kind,
          record.is_clock_arc,
          record.is_disabled,
          record.is_loop_disabled,
          record.eligibility,
          std::move(record.transfer),
          record.output_load, record.native_cell_arc_ordinal, std::move(record.parasitic)};
}

std::vector<csr::ArcId> targetedRow(const Database& database, const NativeTargetedEditState& state, csr::NodeId node, bool incoming)
{
  std::vector<csr::ArcId> result;
  const auto& rows = incoming ? database.get_incoming_arc_list_map() : database.get_outgoing_arc_list_map();
  const auto found = rows.find(state.node_names.at(node.value()));
  if (found != rows.end())
    for (const auto rank : found->second)
      result.push_back(state.native_arc_ids.at(rank));
  return result;
}


}  // namespace ista::timing_edit::detail
