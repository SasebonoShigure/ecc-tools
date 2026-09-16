// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>

#include "Arc.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "Database.hpp"
#include "NativeTargetedEditInternal.hpp"

namespace ista::timing_edit::detail {
csr::NodeRecord targetedNode(const Database& database, const NativeTargetedEditState& state, const std::string& name)
{
  const auto& pin = database.get_pin_map().at(name);
  std::set<std::string> starts, ends;
  if (std::ranges::find(database.get_start_point_list(), name) != database.get_start_point_list().end())
    starts.insert(name);
  if (std::ranges::find(database.get_end_point_list(), name) != database.get_end_point_list().end())
    ends.insert(name);
  std::unordered_map<std::string, std::string> owners;
  if (!pin.get_is_port())
    owners.emplace(name, pin.get_instance_name());
  auto node = csr::detail::projectCanonicalNodeRecord(database, name, pin, starts, ends, owners);
  node.id = state.node_ids.at(name);
  if (!node.is_port) {
    const auto cell = csr::detail::projectCanonicalReferencedLibraryCell(database, node.instance_cell);
    csr::detail::applyCanonicalNodeLibraryRoles(cell, node);
  }
  return node;
}

namespace {
csr::ArcOutputLoadMatrix netLoad(const Database& database, const NativeTargetedEditState& state, const std::string& name)
{
  if (name.empty())
    return {};
  const auto& net = database.get_net_map().at(name);
  std::map<std::string, csr::detail::CanonicalLoadPinFacts> facts;
  for (const auto& pin : net.get_load_pin_list()) {
    const auto node = targetedNode(database, state, pin);
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
}  // namespace

csr::ArcSemanticPayload targetedArc(const Database& database, const NativeTargetedEditState& state, std::size_t rank,
                                    std::map<std::string, csr::ArcOutputLoadMatrix>& loads)
{
  const auto& arc = database.get_arc_list().at(rank);
  std::vector<csr::NodeRecord> nodes{targetedNode(database, state, arc.get_source_pin()), targetedNode(database, state, arc.get_sink_pin())};
  std::map<std::string, csr::NodeId> local_ids{{arc.get_source_pin(), csr::NodeId{0}}, {arc.get_sink_pin(), csr::NodeId{1}}};
  std::unordered_map<std::string, std::string> owners;
  for (const auto& node : nodes)
    if (!node.is_port)
      owners.emplace(node.key.full_name, node.key.instance_name);
  const csr::detail::CanonicalArcKeyContext context{database, nodes, local_ids, owners};
  csr::ArcRecord record;
  record.key = {csr::detail::projectCanonicalArcKeyBase(context, arc), 0};
  for (const auto other_rank : database.get_outgoing_arc_list_map().at(arc.get_source_pin())) {
    if (other_rank >= rank)
      continue;
    const auto& other = database.get_arc_list().at(other_rank);
    if (other.get_sink_pin() == arc.get_sink_pin() && csr::detail::projectCanonicalArcKeyBase(context, other) == record.key.base)
      ++record.key.parallel_ordinal;
  }
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
  record.transfer = csr::detail::projectCanonicalArcTransfer(record, arc);
  auto load = loads.find(nodes[1].net_name);
  if (load == loads.end())
    load = loads.emplace(nodes[1].net_name, netLoad(database, state, nodes[1].net_name)).first;
  record.output_load = load->second;
  return {record.key,
          record.source,
          record.sink,
          record.kind,
          record.is_clock_arc,
          record.is_disabled,
          record.is_loop_disabled,
          record.eligibility,
          std::move(record.transfer),
          record.output_load};
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

void canonicalizeTargetedNativeOrder(Database& database, NativeTargetedEditState& state)
{
  auto& arcs = database.get_arc_list();
  std::map<const TimingCellArc*, std::size_t> ordinals;
  for (const auto& [name, cell] : database.get_timing_library().get_cell_map()) {
    static_cast<void>(name);
    const auto& children = cell.get_cell_arc_list();
    for (std::size_t i = 0; i < children.size(); ++i)
      ordinals.emplace(&children[i], i);
  }
  std::vector<std::size_t> order(arcs.size());
  std::iota(order.begin(), order.end(), 0);
  auto key = [&](std::size_t rank) {
    const auto& arc = arcs[rank];
    const bool net = arc.get_type() == ArcType::kNet;
    const auto ordinal = arc.get_timing_cell_arc() == nullptr ? rank : ordinals.at(arc.get_timing_cell_arc());
    return std::tuple{net, arc.get_owner_name(), net ? 0 : ordinal, arc.get_source_pin(), arc.get_sink_pin()};
  };
  std::ranges::stable_sort(order, [&](auto a, auto b) { return key(a) < key(b); });
  std::vector<std::size_t> new_rank(arcs.size());
  std::vector<Arc> reordered;
  std::vector<csr::ArcId> ids;
  reordered.reserve(arcs.size());
  ids.reserve(arcs.size());
  for (const auto old : order) {
    new_rank[old] = reordered.size();
    ids.push_back(state.native_arc_ids.at(old));
    reordered.push_back(std::move(arcs[old]));
  }
  arcs.swap(reordered);
  state.native_arc_ids.swap(ids);
  for (auto* rows : {&database.get_outgoing_arc_list_map(), &database.get_incoming_arc_list_map()}) {
    for (auto& [name, row] : *rows) {
      static_cast<void>(name);
      for (auto& rank : row)
        rank = new_rank.at(rank);
      std::ranges::sort(row);
    }
  }
  for (auto& rank : state.arc_native_ranks)
    if (rank)
      *rank = new_rank.at(*rank);
  auto rebind = [&](auto& value) {
    const auto old = value.get_predecessor_arc_idx();
    if (old != std::numeric_limits<std::size_t>::max())
      value.set_predecessor_arc_idx(new_rank.at(old));
  };
  for (auto& [name, point] : database.get_timing_point_map()) {
    static_cast<void>(name);
    rebind(point);
    for (auto& [analysis, sources] : point.get_path_state_map())
      for (auto& [source, transitions] : sources)
        for (auto& [transition, tags] : transitions)
          for (auto& [tag, path] : tags)
            rebind(path);
  }
}
}  // namespace ista::timing_edit::detail
