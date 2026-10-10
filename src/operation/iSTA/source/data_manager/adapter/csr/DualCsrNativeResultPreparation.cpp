// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingResultScatterInternal.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "Database.hpp"
#include "DualCsrState.hpp"
#include <stdexcept>

namespace ista::csr::detail {
namespace {
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
}  // namespace

PreparedTimingResult prepareDualCsrTimingResult(Database& database,
    const timing_dual_csr::DualCsrState& graph, const csr::CsrTimingValues& values)
{
  csr::detail::StableScatterTargets targets;
  targets.timing_order = &database.get_timing_order_list();
  targets.nodes.reserve(graph.nodes.size());
  targets.arcs.resize(graph.arc_slots.size());
  require(database.get_pin_map().size() == graph.nodes.size(), "native view node count differs from CSR");
  std::map<std::string, csr::NodeId> node_ids;
  for (const auto& node : graph.nodes) {
    const auto& pin = database.get_pin_map().at(node.key.full_name);
    require(pin.get_full_name() == node.key.full_name && pin.get_is_port() == node.is_port
                && (node.is_port || (pin.get_pin_name() == node.key.pin_name
                                     && pin.get_instance_name() == node.key.instance_name))
                && pin.get_net_name() == node.net_name, "native view node identity differs from CSR");
    require(node_ids.emplace(node.key.full_name, node.id).second, "native view repeats a CSR node name");
    targets.nodes.push_back({&database.get_timing_point_map().at(node.key.full_name), &node.key.full_name});
  }
  std::unordered_map<std::string, std::string> pin_owners;
  for (const auto& [name, instance] : database.get_instance_map())
    for (const auto& pin : instance.get_pin_name_list())
      require(pin_owners.emplace(pin, name).second, "native view pin has multiple owners");
  const csr::detail::CanonicalArcKeyContext key_context{database, graph.nodes, node_ids, pin_owners};
  std::map<csr::ArcKeyBase, std::vector<std::size_t>, decltype(&csr::arcKeyBaseLess)> ranks(csr::arcKeyBaseLess);
  auto& arcs = database.get_arc_list();
  for (std::size_t rank = 0; rank < arcs.size(); ++rank)
    ranks[csr::detail::projectCanonicalArcKeyBase(key_context, arcs[rank])].push_back(rank);
  std::size_t count = 0;
  for (std::size_t id = 0; id < graph.arc_slots.size(); ++id) {
    const auto& slot = graph.arc_slots[id];
    if (!slot.active) continue;
    const auto& key = slot.payload.key;
    const auto found = ranks.find(key.base);
    require(found != ranks.end() && key.parallel_ordinal < found->second.size(), "native view arc identity differs from CSR");
    const auto rank = found->second[key.parallel_ordinal];
    targets.arcs[id] = {&arcs[rank], rank};
    ++count;
  }
  require(count == arcs.size(), "native view has arcs absent from CSR");
  const auto& layout = graph.reference_payload;
  auto prepared = csr::detail::prepareResolvedTimingResult(targets,
      {layout.analysis_order, layout.path_source_order, layout.transition_order, layout.clock_tags}, values);
  auto* result = std::get_if<csr::detail::PreparedTimingResult>(&prepared);
  require(result != nullptr, "current CSR timing cannot be mapped to the native view");
  return std::move(*result);
}
}  // namespace ista::csr::detail
