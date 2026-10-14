// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingResultScatterInternal.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "Database.hpp"
#include "DualCsrState.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace ista::csr::detail {
namespace {
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
struct ArcTargets {
  std::vector<std::size_t> ids;
  std::size_t next{};
};
struct ArcBindingKey {
  std::string_view source, sink, owner;
  ArcKind kind;
  const TimingCellArc* child{};
  friend bool operator==(const ArcBindingKey&, const ArcBindingKey&) = default;
};
struct ArcBindingHash {
  std::size_t operator()(const ArcBindingKey& key) const noexcept
  {
    const auto name_hash = std::hash<std::string_view>{};
    std::size_t value = name_hash(key.source);
    const auto mix = [&](std::size_t next) { value ^= next + 0x9e3779b9U + (value << 6) + (value >> 2); };
    mix(name_hash(key.sink));
    mix(name_hash(key.owner));
    mix(static_cast<std::size_t>(key.kind));
    mix(std::hash<const TimingCellArc*>{}(key.child));
    return value;
  }
};

// Independent canonical proof for development; production uses the library
// binding established by export/edit construction, without exporting it again.
void verifyCanonicalTargets(Database& database, const timing_dual_csr::DualCsrState& graph,
                            const StableScatterTargets& targets)
{
  std::map<std::string, NodeId> node_ids;
  for (const auto& node : graph.nodes) {
    const auto& pin = database.get_pin_map().at(node.key.full_name);
    require(pin.get_full_name() == node.key.full_name && pin.get_is_port() == node.is_port
                && (node.is_port || (pin.get_pin_name() == node.key.pin_name
                                     && pin.get_instance_name() == node.key.instance_name))
                && pin.get_net_name() == node.net_name, "native view node identity differs from CSR");
    require(node_ids.emplace(node.key.full_name, node.id).second, "native view repeats a CSR node name");
  }
  std::unordered_map<std::string, std::string> pin_owners;
  for (const auto& [name, instance] : database.get_instance_map())
    for (const auto& pin : instance.get_pin_name_list())
      require(pin_owners.emplace(pin, name).second, "native view pin has multiple owners");
  const CanonicalArcKeyContext context{database, graph.nodes, node_ids, pin_owners};
  const auto hash = [](const ArcKeyBase* key) {
    const auto name_hash = std::hash<std::string_view>{};
    return name_hash(key->source.full_name) ^ (name_hash(key->sink.full_name) << 1);
  };
  const auto equal = [](const ArcKeyBase* left, const ArcKeyBase* right) {
    return !arcKeyBaseLess(*left, *right) && !arcKeyBaseLess(*right, *left);
  };
  std::unordered_map<const ArcKeyBase*, ArcTargets, decltype(hash), decltype(equal)> groups(0, hash, equal);
  groups.reserve(database.get_arc_list().size());
  for (std::size_t id = 0; id < graph.arc_slots.size(); ++id)
    if (graph.arc_slots[id].active) groups[&graph.arc_slots[id].payload.key.base].ids.push_back(id);
  for (auto& [key, group] : groups) {
    static_cast<void>(key);
    const auto ordinal = [&](std::size_t id) { return graph.arc_slots[id].payload.key.parallel_ordinal; };
    std::ranges::sort(group.ids, {}, ordinal);
    for (std::size_t i = 0; i < group.ids.size(); ++i)
      require(ordinal(group.ids[i]) == i, "CSR parallel arc identity is incomplete");
  }
  const auto& arcs = database.get_arc_list();
  for (std::size_t rank = 0; rank < arcs.size(); ++rank) {
    const auto key = projectCanonicalArcKeyBase(context, arcs[rank]);
    const auto found = groups.find(&key);
    require(found != groups.end() && found->second.next < found->second.ids.size(),
            "native view arc identity differs from CSR");
    const auto id = found->second.ids[found->second.next++];
    require(targets.arcs[id].native_rank == rank && targets.arcs[id].arc == &arcs[rank],
            "native result addressing differs from canonical arc identity");
  }
}
}  // namespace

StableScatterTargets resolveDualCsrTimingTargets(Database& database,
    const timing_dual_csr::DualCsrState& graph)
{
  StableScatterTargets targets;
  targets.timing_order = &database.get_timing_order_list();
  targets.nodes.reserve(graph.nodes.size());
  targets.arcs.resize(graph.arc_slots.size());
  require(database.get_pin_map().size() == graph.nodes.size(), "native view node count differs from CSR");
  for (const auto& node : graph.nodes)
    targets.nodes.push_back({&database.get_timing_point_map().at(node.key.full_name), &node.key.full_name});

  // Both views share this session's immutable Liberty. Native physical ranks
  // may change; resolve them within the guarded query and retain only stable IDs.
  std::unordered_map<ArcBindingKey, ArcTargets, ArcBindingHash> groups;
  groups.reserve(database.get_arc_list().size());
  std::size_t count = 0;
  for (std::size_t id = 0; id < graph.arc_slots.size(); ++id) {
    const auto& slot = graph.arc_slots[id];
    if (!slot.active) continue;
    const auto& arc = slot.payload;
    const auto& key = arc.key.base;
    const TimingCellArc* child = nullptr;
    if (arc.native_cell_arc_ordinal) {
      const auto& instance = database.get_instance_map().at(key.owner_name);
      const auto& cell = database.get_timing_library().get_cell_map().at(instance.get_cell_name());
      child = &cell.get_cell_arc_list().at(*arc.native_cell_arc_ordinal);
    }
    groups[{key.source.full_name, key.sink.full_name, key.owner_name, arc.kind, child}].ids.push_back(id);
    ++count;
  }
  auto& arcs = database.get_arc_list();
  require(count == arcs.size(), "native view has arcs absent from CSR");
  for (auto& [key, group] : groups) {
    static_cast<void>(key);
    std::ranges::sort(group.ids, {}, [&](std::size_t id) { return graph.arc_slots[id].payload.key.parallel_ordinal; });
  }
  for (std::size_t rank = 0; rank < arcs.size(); ++rank) {
    auto& arc = arcs[rank];
    require(arc.get_type() == ArcType::kCell || arc.get_type() == ArcType::kNet, "native view arc has no timing kind");
    const ArcBindingKey key{arc.get_source_pin(), arc.get_sink_pin(), arc.get_owner_name(),
                           arc.get_type() == ArcType::kCell ? ArcKind::kCell : ArcKind::kNet, arc.get_timing_cell_arc()};
    const auto found = groups.find(key);
    require(found != groups.end() && found->second.next < found->second.ids.size(),
            "native timing target is missing from committed CSR");
    const auto id = found->second.ids[found->second.next++];
    targets.arcs[id] = {&arc, rank};
  }
  if constexpr (incregpusta_debug_checks_enabled)
    verifyCanonicalTargets(database, graph, targets);
  return targets;
}

PreparedTimingResult prepareDualCsrTimingResult(Database& database,
    const timing_dual_csr::DualCsrState& graph, const CsrTimingValues& values)
{
  const auto targets = resolveDualCsrTimingTargets(database, graph);
  const auto& layout = graph.reference_payload;
  auto prepared = prepareResolvedTimingResult(targets,
      {layout.analysis_order, layout.path_source_order, layout.transition_order, layout.clock_tags}, values);
  auto* result = std::get_if<PreparedTimingResult>(&prepared);
  require(result != nullptr, "current CSR timing cannot be mapped to the native view");
  return std::move(*result);
}
}  // namespace ista::csr::detail
