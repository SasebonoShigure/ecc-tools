#include "CudaDualCsrInternal.cuh"

#include <algorithm>

namespace ista::csr {
using namespace cuda_detail;

void CudaDualCsrSession::Impl::validateRcPatch(const Impl& prior, const CudaDualCsrPatch& patch)
{
  const auto& state = prior.host->rc;
  demand(prior.view.rc_net_count || patch.rc_bindings.empty(), "CUDA RC edits require an RC baseline");
  std::map<Index, const CudaRcBindingChange*> nodes;
  std::map<Index, std::shared_ptr<const CsrParasiticTree>> owners, old_owners;
  for (const auto& delta : patch.rc_bindings) {
    const auto id = delta.node.value();
    demand(id < patch.after.node_slots && nodes.emplace(id, &delta).second, "CUDA duplicate or invalid RC terminal");
    const auto current = state.nodes.find(id);
    demand(delta.expected == (current == state.nodes.end() ? CsrParasiticBinding{} : current->second.binding),
           "CUDA RC terminal expected owner mismatch");
    demand(id >= prior.view.node_count || current != state.nodes.end(), "CUDA RC patch cannot introduce RC on an existing no-RC terminal");
    const auto& next = delta.replacement;
    demand(next.tree && next.terminal < next.tree->terminals.size() && next.tree->terminals[next.terminal] == delta.node,
           "CUDA replacement RC terminal identity mismatch");
    const auto driver = next.tree->terminals.front().value();
    demand(driver < patch.after.node_slots, "CUDA RC driver outside graph");
    const auto [found, added] = owners.emplace(driver, next.tree);
    demand(added || found->second == next.tree, "CUDA RC driver has conflicting replacement owners");
    if (delta.expected.tree)
      old_owners.emplace(delta.expected.tree->terminals.front().value(), delta.expected.tree);
  }
  for (const auto& [driver, owner] : owners) {
    demand(driver >= prior.view.node_count || (driver < state.net_by_node.size() && state.net_by_node[driver] != absent),
           "CUDA RC replacement changes existing driver identity");
    for (std::size_t terminal = 0; terminal < owner->terminals.size(); ++terminal) {
      const auto found = nodes.find(owner->terminals[terminal].value());
      demand(found != nodes.end() && found->second->replacement == CsrParasiticBinding{owner, terminal},
             "CUDA RC replacement omits or duplicates a terminal");
    }
  }
  for (const auto& [driver, owner] : old_owners) {
    demand(owners.contains(driver), "CUDA RC patch removes an existing driver");
    for (const auto node : owner->terminals)
      demand(nodes.contains(node.value()), "CUDA RC patch omits an old terminal");
  }
  std::set<Index> rewritten;
  const auto payload = [&](const ArcSemanticPayload& value, bool replacement) {
    const auto id = value.sink.value();
    const auto change = nodes.find(id);
    CsrParasiticBinding expected;
    if (replacement && change != nodes.end()) expected = change->second->replacement;
    else if (const auto current = state.nodes.find(id); current != state.nodes.end()) expected = current->second.binding;
    demand(value.parasitic == expected, "CUDA RC arc/terminal owner mismatch");
    if (!expected.tree) return;
    demand(!value.output_load && expected.tree->terminals.front() == (value.kind == ArcKind::kNet ? value.source : value.sink),
           "CUDA RC arc has wrong driver or duplicate load");
    if (value.kind == ArcKind::kCell && !value.transfer.children().empty())
      demand(value.native_cell_arc_ordinal.has_value(), "CUDA RC cell lacks native ordinal");
  };
  for (const auto& item : patch.payload_changes) {
    payload(item.expected, false);
    payload(item.replacement, true);
    rewritten.insert(item.id.value());
  }
  for (const auto& item : patch.arc_appends) payload(item.payload, true);
  for (const auto& item : patch.logical_deactivations) rewritten.insert(item.id.value());
  for (const auto& [node, delta] : nodes) {
    static_cast<void>(delta);
    if (node >= prior.view.node_count) continue;
    for (const auto id : prior.host->adjacency[node].incoming)
      if (prior.host->bindings[id].active && prior.host->bindings[id].rc_binding != absent)
        demand(rewritten.contains(id), "CUDA RC update omits an active arc at an affected terminal");
  }
}

void CudaDualCsrSession::Impl::prepareRcPatch(const Impl& prior, const CudaDualCsrPatch& patch, PackedPatch& packed)
{
  auto& pending = storage->rc_patch;
  pending.clear();
  pending.old_net_count = prior.view.rc_net_count;
  view.rc_net_count = prior.view.rc_net_count;
  view.rc_port_derate = prior.view.rc_port_derate;
  std::copy(std::begin(prior.view.rc_port_lower), std::end(prior.view.rc_port_lower), view.rc_port_lower);
  std::copy(std::begin(prior.view.rc_port_upper), std::end(prior.view.rc_port_upper), view.rc_port_upper);
  if (!view.rc_net_count && patch.rc_bindings.empty()) return;
  auto& rc = host->rc;
  auto& undo = storage->host_undo;
  std::map<Index, HostRcNode> next_nodes;
  std::map<Index, Index> drivers;
  for (const auto& change : patch.rc_bindings) {
    const Index node = change.node.value(), driver = change.replacement.tree->terminals.front().value();
    HostRcNode value;
    if (node < prior.view.node_count) value = rc.nodes.at(node);
    else {
      const auto& source = patch.node_appends.at(node - prior.view.node_count).payload;
      value.port = source.is_port;
      if (!source.is_port) {
        const auto found = patch.rc_load_cells.find(source.instance_cell);
        demand(found != patch.rc_load_cells.end() && found->second.get().present
                   && found->second.get().cell_name == source.instance_cell, "CUDA new RC terminal has no immutable cell model");
        value.load_cell = storage->models.internLoadCell(found->second.get());
      }
    }
    value.binding = change.replacement;
    next_nodes.emplace(node, std::move(value));
    if (!drivers.contains(driver)) {
      const auto index = driver < prior.view.node_count ? rc.net_by_node.at(driver) : view.rc_net_count;
      if (driver >= prior.view.node_count) view.rc_net_count = checkedAdd(view.rc_net_count, 1);
      drivers.emplace(driver, index);
    }
  }
  for (const auto& [driver, index] : drivers)
    pending.nets[index].owner = next_nodes.at(driver).binding.tree;
  Index binding_count = rc.bindings.size();
  std::map<Index, Index> initialize_ordinals;
  const auto encode = [&](Index id, const ArcSemanticPayload& source, Arc& target) {
    const Index old = id < prior.view.arc_count ? host->bindings.at(id).rc_binding : absent;
    if (!source.parasitic.tree) {
      target.rc_binding = absent;
      return;
    }
    const auto driver = source.parasitic.tree->terminals.front().value();
    const auto found = drivers.find(driver);
    demand(found != drivers.end(), "CUDA RC payload has no changed-net binding");
    const auto net_index = found->second;
    auto& net = pending.nets.at(net_index);
    auto binding = packRcBinding(net, net_index, source.parasitic.terminal, false,
                                  source.native_cell_arc_ordinal.value_or(0), target.child_begin, target.child_count);
    if (target.net) {
      binding.input_port = next_nodes.at(driver).port;
      binding.load_cell = next_nodes.at(target.sink).load_cell;
    } else if (target.child_count) {
      net.initialize_arcs.push_back(id);
      initialize_ordinals.emplace(id, *source.native_cell_arc_ordinal);
    }
    target.rc_binding = old == absent ? binding_count : old;
    if (old == absent) binding_count = checkedAdd(binding_count, 1);
    pending.bindings.push_back({target.rc_binding, binding});
  };
  for (std::size_t i = 0; i < packed.payloads.size(); ++i) {
    auto& item = packed.payloads[i];
    item.expected.rc_binding = host->bindings.at(item.id).rc_binding;
    encode(item.id, patch.payload_changes[i].replacement, item.replacement);
  }
  for (std::size_t i = 0; i < packed.appends.size(); ++i)
    encode(prior.view.arc_count + i, patch.arc_appends[i].payload, packed.appends[i]);
  for (const auto& [driver, index] : drivers) {
    const auto seed = std::ranges::lower_bound(host->node_seeds, driver, {}, &NodeSeed::node);
    if (seed == host->node_seeds.end() || seed->node != driver || seed->state.sequential.kind != SeedKind::output) continue;
    const auto& sequential = seed->state.sequential;
    demand(sequential.rc_binding != absent, "CUDA deferred Q has no committed RC binding");
    pending.bindings.push_back({sequential.rc_binding,
        packRcBinding(pending.nets.at(index), index, 0, true, 0, sequential.child_begin, sequential.child_count)});
  }
  std::set<Index> unique;
  for (const auto& update : pending.bindings) {
    demand(unique.insert(update.index).second, "CUDA duplicate RC binding write");
    if (update.index < rc.bindings.size()) {
      undo.rc_bindings.push_back({update.index, rc.bindings[update.index]});
    }
  }
  for (auto& [index, net] : pending.nets) {
    static_cast<void>(index);
    std::ranges::sort(net.initialize_arcs, [&](Index a, Index b) {
      return std::pair{initialize_ordinals.at(a), a} < std::pair{initialize_ordinals.at(b), b};
    });
    finishRcNet(net);
    packRcTree(*net.owner, net.tree);
  }
  for (const auto& [node, value] : next_nodes) {
    static_cast<void>(value);
    if (const auto current = rc.nodes.find(node); current != rc.nodes.end())
      undo.rc_nodes.emplace(node, current->second);
    else undo.new_rc_nodes.push_back(node);
  }
  undo.rc_binding_count = rc.bindings.size();
  undo.rc_node_count = rc.net_by_node.size();
  if (binding_count > rc.bindings.capacity())
    rc.bindings.reserve(std::max(binding_count, rc.bindings.capacity() + rc.bindings.capacity() / 2));
  if (patch.after.node_slots > rc.net_by_node.capacity())
    rc.net_by_node.reserve(std::max<Index>(patch.after.node_slots, rc.net_by_node.capacity() + rc.net_by_node.capacity() / 2));
  // All host RC allocations precede this publication; general graph undo is
  // already active. No expected descriptor is used as rollback data.
  undo.rc_applied = true;
  for (auto& [node, value] : next_nodes)
    if (const auto current = rc.nodes.find(node); current != rc.nodes.end()) current->second = std::move(value);
  rc.nodes.merge(next_nodes);
  rc.bindings.resize(binding_count);
  for (const auto& update : pending.bindings) rc.bindings[update.index] = update.value;
  rc.net_by_node.resize(patch.after.node_slots, absent);
  for (const auto& [driver, index] : drivers) rc.net_by_node[driver] = index;
  for (const auto& item : packed.payloads) host->bindings[item.id].rc_binding = item.replacement.rc_binding;
  for (std::size_t i = 0; i < packed.appends.size(); ++i)
    host->bindings[prior.view.arc_count + i].rc_binding = packed.appends[i].rc_binding;
}
}  // namespace ista::csr
