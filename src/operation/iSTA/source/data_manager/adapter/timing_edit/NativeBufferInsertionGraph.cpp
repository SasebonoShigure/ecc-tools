// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "NativeBufferInsertionInternal.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include "CanonicalGraphConstruction.hpp"
#include "NativeTargetedEditInternal.hpp"

namespace ista::timing_edit::detail {
namespace {
void demand(bool value, const char* message)
{
  if (!value) throw std::invalid_argument(message);
}
}  // namespace

PreparedNativeGraphPatch prepareNativeBufferGraph(Database& database, PreparedNativeBufferInsertions& raw,
    std::span<const BufferInsertionMutation> edits, TimingEditResolverView resolver)
{
  PreparedNativeGraphPatch graph;
  graph.arc_count_before = database.get_arc_list().size();
  if (edits.size() > (database.get_arc_list().max_size() - graph.arc_count_before) / 2)
    throw std::length_error("targeted append exceeds native arc capacity");
  graph.appends.reserve(edits.size() * 2);
  const auto pin_for = [&](const std::string& name) -> const Pin& {
    const auto found = raw.pins.find(name);
    return found == raw.pins.end() ? database.get_pin_map().at(name) : found->second;
  };
  const auto cache_for = [&](Net& net, const std::vector<std::string>& members) {
    PreparedNetDriverLoad cache;
    cache.net = &net;
    for (const auto& name : members) {
      const auto& pin = pin_for(name);
      if (pin.get_is_port() ? pin.get_direction() == PinDirection::kInput : pin.get_direction() == PinDirection::kOutput) {
        cache.driver = name;
        cache.drivers.push_back(name);
      } else {
        cache.loads.push_back(name);
      }
    }
    demand(cache.drivers.size() == 1, "targeted source requires exactly one driver after edit");
    std::ranges::sort(cache.loads);
    return cache;
  };
  for (const auto& [name, members] : raw.memberships)
    graph.net_caches.push_back(cache_for(database.get_net_map().at(name), members));
  for (auto& [name, net] : raw.nets) {
    static_cast<void>(name);
    auto cache = cache_for(net, net.get_pin_name_list());
    cache.swap();
  }
  const auto driver_for = [&](const std::string& name) -> const std::string& {
    const auto found = raw.nets.find(name);
    return (found == raw.nets.end() ? database.get_net_map().at(name) : found->second).get_driver_pin();
  };
  const auto ensure_rows = [&](const std::string& name) {
    const auto prepare = [&](const auto& native, auto& rows, auto& appended) {
      if (rows.contains(name)) return;
      const auto found = native.find(name);
      if (found == native.end()) {
        rows.emplace(name, std::vector<std::size_t>{});
        appended.push_back(name);
      } else {
        rows.emplace(name, found->second);
      }
    };
    prepare(database.get_outgoing_arc_list_map(), graph.outgoing, graph.new_outgoing);
    prepare(database.get_incoming_arc_list_map(), graph.incoming, graph.new_incoming);
  };
  for (const auto& name : raw.pin_keys) {
    graph.points.emplace(name, TimingPoint{});
    graph.new_points.push_back(name);
    ensure_rows(name);
  }
  for (const auto& [name, net] : raw.connections) {
    const auto& old_net = database.get_pin_map().at(name).get_net_name();
    std::optional<std::size_t> slot;
    for (const auto rank : database.get_incoming_arc_list_map().at(name)) {
      const auto& arc = database.get_arc_list().at(rank);
      if (arc.get_type() != ArcType::kNet || arc.get_sink_pin() != name || arc.get_owner_name() != old_net) continue;
      demand(!slot, "targeted moved sink has ambiguous native arc");
      slot = rank;
    }
    demand(slot.has_value(), "targeted moved sink has no native arc");
    const auto& old = database.get_arc_list().at(*slot);
    const auto& driver = driver_for(net);
    ensure_rows(old.get_source_pin());
    ensure_rows(driver);
    ensure_rows(name);
    auto& outgoing = graph.outgoing.at(old.get_source_pin());
    const auto found = std::ranges::find(outgoing, *slot);
    demand(found != outgoing.end(), "targeted replacement is absent from its outgoing row");
    outgoing.erase(found);
    graph.outgoing.at(driver).push_back(*slot);
    graph.replacements.emplace(*slot, canonical_graph::makeArc(
        driver, name, ArcType::kNet, net, {}, {}, false, false, nullptr));
  }
  const auto append_arc = [&](Arc arc) {
    ensure_rows(arc.get_source_pin());
    ensure_rows(arc.get_sink_pin());
    const auto slot = graph.arc_count_before + graph.appends.size();
    graph.outgoing.at(arc.get_source_pin()).push_back(slot);
    graph.incoming.at(arc.get_sink_pin()).push_back(slot);
    graph.appends.push_back(std::move(arc));
  };
  for (const auto& edit : edits) {
    const auto& names = resolver.materialization(edit.creation_token);
    auto& children = database.get_timing_library().get_cell_map().at(edit.library_cell.cell_name).get_cell_arc_list();
    const auto child = std::ranges::find_if(children, [](const auto& arc) { return arc.get_is_timing_graph_arc(); });
    demand(child != children.end(), "targeted missing library graph arc");
    append_arc(canonical_graph::makeArc(names.input_pin_name, names.output_pin_name, ArcType::kCell, names.instance_name,
        child->get_source_port(), child->get_sink_port(), child->get_is_clock_arc(), child->get_is_disable_arc(), &*child));
    const auto& net = raw.pins.at(names.input_pin_name).get_net_name();
    // Accepted data-only edits cannot create a sequential-to-clock net arc.
    append_arc(canonical_graph::makeArc(driver_for(net), names.input_pin_name, ArcType::kNet, net, {}, {}, false, false, nullptr));
  }
  const auto arc_for = [&](std::size_t slot) -> const Arc& {
    if (slot >= graph.arc_count_before) return graph.appends.at(slot - graph.arc_count_before);
    const auto found = graph.replacements.find(slot);
    return found == graph.replacements.end() ? database.get_arc_list().at(slot) : found->second;
  };
  const auto key = [&](std::size_t slot) {
    const auto& arc = arc_for(slot);
    const bool net = arc.get_type() == ArcType::kNet;
    std::size_t ordinal = slot;
    if (const auto* child = arc.get_timing_cell_arc()) {
      const auto created = raw.instances.find(arc.get_owner_name());
      const auto& instance = created == raw.instances.end() ? database.get_instance_map().at(arc.get_owner_name()) : created->second;
      const auto& children = database.get_timing_library().get_cell_map().at(instance.get_cell_name()).get_cell_arc_list();
      const auto found = std::ranges::find_if(children, [&](const auto& value) { return &value == child; });
      demand(found != children.end(), "targeted adjacency contains a foreign library arc");
      ordinal = static_cast<std::size_t>(found - children.begin());
    }
    return std::tuple{net, std::string_view{arc.get_owner_name()}, net ? 0 : ordinal,
                      std::string_view{arc.get_source_pin()}, std::string_view{arc.get_sink_pin()}, slot};
  };
  for (auto* rows : {&graph.outgoing, &graph.incoming})
    for (auto& [name, row] : *rows) {
      static_cast<void>(name);
      std::ranges::sort(row, [&](auto left, auto right) { return key(left) < key(right); });
    }
  demand(graph.appends.size() <= database.get_arc_list().max_size() - graph.arc_count_before,
         "targeted append exceeds native arc capacity");
  // No Arc pointer survives growth; apply/undo use physical indices.
  reserveTargetedAppend(database.get_arc_list(), graph.arc_count_before + graph.appends.size());
  return graph;
}
}  // namespace ista::timing_edit::detail
