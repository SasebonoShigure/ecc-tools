#include <algorithm>
#include <cmath>
#include <map>

#include "CsrReferenceNativeModelValidatorInternal.hpp"
#include "CsrReferenceValidatorDetail.hpp"
#include "CudaDualCsrInternal.cuh"

namespace ista::csr {
using namespace cuda_detail;
void CudaDualCsrSession::Impl::validatePatch(const Impl& prior, const CudaDualCsrPatch& patch)
{
  demand(!prior.poisoned && !prior.storage->poisoned && !prior.storage->pending && !prior.origin && !prior.code, "CUDA targeted owner is not committed and healthy");
  demand(patch.session_token == prior.token && patch.expected_generation == prior.generation && patch.before == prior.high_water,
         "CUDA targeted session/generation/high-water mismatch");
  demand(patch.expected_generation.value() != absent && patch.target_generation.value() == patch.expected_generation.value() + 1,
         "CUDA targeted generation overflow or discontinuity");
  demand(patch.after.node_slots == checkedAdd(patch.before.node_slots, patch.node_appends.size())
             && patch.after.arc_slots == checkedAdd(patch.before.arc_slots, patch.arc_appends.size()) && patch.after.net_slots >= patch.before.net_slots,
         "CUDA targeted append high-water mismatch");
  checkedAdd(checkedProduct(patch.after.node_slots, 8), 1);
  demand(patch.node_appends.size() == patch.appended_base_out_rows.size() && patch.node_appends.size() == patch.appended_base_in_rows.size(),
         "CUDA targeted appended row count mismatch");
  for (Index i = 0; i < patch.node_appends.size(); ++i) {
    const Index id = patch.before.node_slots + i;
    demand(patch.node_appends[i].id.value() == id && patch.appended_base_out_rows[i].source.value() == id && patch.appended_base_in_rows[i].sink.value() == id,
           "CUDA targeted node rows are not contiguous");
  }
  std::set<Index> arc_writes, out_slots, in_slots, aux_sources;
  auto arc = [&](Index id) { demand(id < patch.after.arc_slots, "CUDA targeted arc outside high-water"); };
  auto payload = [&](const ArcSemanticPayload& value) {
    demand(detail::knownEnum(value.kind), "CUDA arc kind is unknown");
    const bool eligible = value.eligibility.levelization || value.eligibility.data_arrival || value.eligibility.data_slew || value.eligibility.clock;
    demand(!(value.is_disabled || value.is_loop_disabled) || !eligible, "CUDA disabled arc remains eligible");
    demand(value.eligibility.levelization || !(value.eligibility.data_arrival || value.eligibility.data_slew || value.eligibility.clock),
           "CUDA propagation arc is not levelization eligible");
    std::vector<CsrContractViolation> errors;
    detail::validateTransfer(value.transfer, errors, "patch.transfer");
    demand(errors.empty(), "CUDA malformed transfer/table payload");
    demand(value.source.value() < patch.after.node_slots && value.sink.value() < patch.after.node_slots, "CUDA payload endpoint outside high-water");
    if (value.output_load)
      for (const auto& row : *value.output_load)
        for (double load : row)
          demand(std::isfinite(load) && load >= 0.0, "CUDA invalid output load");
    if (value.kind == ArcKind::kNet) {
      CellTransferModel canonical;
      canonical.delay = canonical.delay_min = canonical.delay_max = 0.0;
      demand(value.transfer == canonical && !value.is_clock_arc, "CUDA noncanonical net transfer");
    } else {
      demand(value.transfer.is_clock_arc == value.is_clock_arc && value.transfer.is_disabled == value.is_disabled, "CUDA transfer flags differ from arc");
    }
  };
  for (Index i = 0; i < patch.arc_appends.size(); ++i) {
    const auto& record = patch.arc_appends[i];
    payload(record.payload);
    demand(record.id.value() == patch.before.arc_slots + i && record.source.value() < patch.after.node_slots && record.sink.value() < patch.after.node_slots
               && record.payload.source == record.source && record.payload.sink == record.sink,
           "CUDA targeted appended arc binding mismatch");
  }
  auto physical = [&](Index node, Index position, bool incoming) {
    const auto& offsets = incoming ? prior.host->in_offsets : prior.host->out_offsets;
    demand(node < prior.view.node_count && position >= offsets[node] && position < offsets[node + 1], "CUDA targeted physical slot outside row");
    demand((incoming ? in_slots : out_slots).insert(position).second, "CUDA targeted duplicate physical write");
  };
  for (const auto& x : patch.removes) {
    physical(x.source.value(), x.physical_slot, false);
    arc(x.expected_arc.value());
  }
  for (const auto& x : patch.changes) {
    physical(x.source.value(), x.physical_slot, false);
    if (x.expected_arc)
      arc(x.expected_arc->value());
    arc(x.replacement_arc.value());
  }
  for (const auto& x : patch.reverse_changes) {
    physical(x.sink.value(), x.physical_slot, true);
    arc(x.expected_arc.value());
    arc(x.replacement_arc.value());
  }
  for (const auto& x : patch.logical_deactivations) {
    demand(x.id.value() < prior.view.arc_count && x.expected_active && arc_writes.insert(x.id.value()).second, "CUDA targeted deactivation conflict");
  }
  for (const auto& x : patch.payload_changes) {
    payload(x.expected);
    payload(x.replacement);
    demand(x.id.value() < prior.view.arc_count && arc_writes.insert(x.id.value()).second, "CUDA targeted payload write conflict");
    demand(x.expected.source == x.replacement.source && x.expected.sink == x.replacement.sink, "CUDA payload patch cannot change arc identity");
  }
  for (const auto& x : patch.aux_rows) {
    demand(x.source.value() < patch.after.node_slots && aux_sources.insert(x.source.value()).second, "CUDA targeted duplicate or invalid Aux source");
    const auto length = x.source.value() < prior.view.node_count ? prior.storage->graph.aux_storage[x.source.value()].count : 0;
    demand(x.expected_arc_ids.size() == length, "CUDA targeted Aux expected length mismatch");
    for (auto id : x.expected_arc_ids)
      arc(id.value());
    for (auto id : x.replacement_arc_ids)
      arc(id.value());
  }
  for (const auto& x : patch.appended_base_out_rows)
    for (auto id : x.arc_ids)
      arc(id.value());
  for (const auto& x : patch.appended_base_in_rows)
    for (auto id : x.arc_ids)
      arc(id.value());
  // Account only the changed incidences. Every active logical arc has exactly
  // one outgoing and one incoming occurrence; actual old slots are checked on
  // device before publication. A new payload alone is not an executable edge.
  struct Incidence
  {
    unsigned removed[2]{}, added[2]{};
  };
  std::map<Index, Incidence> incidences;
  std::set<Index> deactivated;
  for (const auto& x : patch.logical_deactivations)
    deactivated.insert(x.id.value());
  auto occurrence = [&](Index id, Index row, bool incoming, bool added) {
    arc(id);
    const bool appended = id >= prior.view.arc_count;
    demand(added || !appended, "CUDA expected incidence refers to new arc");
    const auto binding
        = appended ? ArcBinding{patch.arc_appends[id - prior.view.arc_count].source.value(), patch.arc_appends[id - prior.view.arc_count].sink.value(), true}
                   : prior.host->bindings[id];
    demand(binding.active && (incoming ? binding.sink : binding.source) == row && (!added || !deactivated.contains(id)),
           "CUDA incidence endpoint or active binding mismatch");
    auto& count = added ? incidences[id].added[incoming] : incidences[id].removed[incoming];
    demand(++count == 1, "CUDA duplicate logical arc incidence");
  };
  for (const auto& x : patch.removes)
    occurrence(x.expected_arc.value(), x.source.value(), false, false);
  for (const auto& x : patch.changes) {
    if (x.expected_arc)
      occurrence(x.expected_arc->value(), x.source.value(), false, false);
    occurrence(x.replacement_arc.value(), x.source.value(), false, true);
  }
  for (const auto& x : patch.reverse_changes) {
    occurrence(x.expected_arc.value(), x.sink.value(), true, false);
    occurrence(x.replacement_arc.value(), x.sink.value(), true, true);
  }
  for (const auto& x : patch.aux_rows) {
    for (auto id : x.expected_arc_ids)
      occurrence(id.value(), x.source.value(), false, false);
    for (auto id : x.replacement_arc_ids)
      occurrence(id.value(), x.source.value(), false, true);
  }
  for (const auto& x : patch.appended_base_out_rows)
    for (auto id : x.arc_ids)
      occurrence(id.value(), x.source.value(), false, true);
  for (const auto& x : patch.appended_base_in_rows)
    for (auto id : x.arc_ids)
      occurrence(id.value(), x.sink.value(), true, true);
  for (const auto& x : patch.arc_appends)
    incidences.try_emplace(x.id.value());
  for (auto id : deactivated)
    incidences.try_emplace(id);
  for (const auto& [id, counts] : incidences) {
    const int delta = id >= prior.view.arc_count ? 1 : deactivated.contains(id) ? -1 : 0;
    for (unsigned direction = 0; direction < 2; ++direction)
      demand(static_cast<int>(counts.added[direction]) - static_cast<int>(counts.removed[direction]) == delta,
             "CUDA patch does not preserve complete arc incidence");
  }
  std::set<std::pair<Index, std::string>> seed_keys;
  for (const auto& seed : patch.appended_start_seeds) {
    demand(seed_keys.emplace(seed.seed_node.value(), seed.clock_tag).second, "CUDA duplicate appended seed");
    demand(seed.seed_node.value() >= prior.view.node_count && seed.seed_node.value() < patch.after.node_slots
               && seed.path_start_node.value() < patch.after.node_slots && !seed.crpr_node && !seed.crpr_transition,
           "CUDA append seed binding or CRPR unsupported");
    demand(seed.availability == StartSeedAvailability::kMaterialized && (!seed.path_source || *seed.path_source == PathSource::kInput),
           "CUDA append seed availability/source unsupported");
    demand(std::find(prior.host->layout.clock_tags.begin(), prior.host->layout.clock_tags.end(), seed.clock_tag) != prior.host->layout.clock_tags.end(),
           "CUDA appended seed changes clock domain");
    for (unsigned a = 0; a < 2; ++a)
      for (unsigned t = 0; t < 2; ++t) {
        demand(seed.slew[a][t] && std::isfinite(*seed.slew[a][t]) && *seed.slew[a][t] >= 0.0, "CUDA invalid seed slew");
        demand(!seed.path_source || (seed.arrival[a][t] && std::isfinite(*seed.arrival[a][t])), "CUDA invalid seed arrival");
      }
  }
  std::set<Index> endpoint_nodes;
  for (const auto& endpoint : patch.appended_endpoints) {
    demand(
        endpoint.node.value() >= prior.view.node_count && endpoint.node.value() < patch.after.node_slots && endpoint_nodes.insert(endpoint.node.value()).second,
        "CUDA duplicate or invalid appended endpoint");
    demand(std::isfinite(endpoint.clock_period) && endpoint.clock_period >= 0.0, "CUDA invalid endpoint period");
    demand(std::find(prior.host->layout.clock_tags.begin(), prior.host->layout.clock_tags.end(), endpoint.clock_name) != prior.host->layout.clock_tags.end(),
           "CUDA appended endpoint changes clock domain");
    for (const auto& delay : endpoint.output_delay)
      demand(!delay || std::isfinite(*delay), "CUDA invalid endpoint output delay");
  }
  if (patch.roots)
    demand(std::ranges::equal(patch.roots->expected, prior.host->roots, {}, &NodeId::value), "CUDA targeted host->roots mismatch");
  if (patch.ends)
    demand(std::ranges::equal(patch.ends->expected, prior.host->ends, {}, &NodeId::value), "CUDA targeted host->ends mismatch");
}

PackedPatch& CudaDualCsrSession::Impl::prepare(const Impl& prior, const CudaDualCsrPatch& patch)
{
  auto& packed = storage->patch_uploads;
  packed.removes.clear();
  packed.changes.clear();
  packed.reverse_changes.clear();
  packed.payloads.clear();
  packed.deactivations.clear();
  packed.appends.clear();
  packed.aux_patches.clear();
  packed.aux_values.clear();
  packed.appended_out.clear();
  packed.appended_in.clear();
  packed.endpoints.clear();
  packed.slots.clear();
  packed.arcs.clear();
  auto& models = storage->models;
  auto& undo = storage->host_undo;
  demand(!undo.target, "CUDA host patch already pending");
  view.child_state_count = prior.view.child_state_count;
  std::set<Index> touched;
  std::vector<Index> changed_nodes;
  const auto touch = [&](Index id) {
    if (id < prior.view.arc_count)
      touched.insert(id);
  };
  for (const auto& x : patch.removes)
    packed.removes.push_back({x.source.value(), x.physical_slot, x.expected_arc.value(), absent});
  for (const auto& x : patch.changes) {
    packed.changes.push_back({x.source.value(), x.physical_slot, x.expected_arc ? x.expected_arc->value() : absent, x.replacement_arc.value()});
    touch(x.replacement_arc.value());
  }
  for (const auto& x : patch.reverse_changes)
    packed.reverse_changes.push_back({x.sink.value(), x.physical_slot, x.expected_arc.value(), x.replacement_arc.value()});
  for (const auto& x : patch.logical_deactivations) {
    const auto& binding = host->bindings[x.id.value()];
    demand(binding.active && binding.source == x.source.value() && binding.sink == x.sink.value(), "deactivation binding mismatch");
    packed.deactivations.push_back({x.id.value(), x.source.value(), x.sink.value()});
    if (binding.path_transitions)
      changed_nodes.push_back(binding.sink);
    touch(x.id.value());
  }
  for (const auto& x : patch.payload_changes) {
    PayloadPatch record{x.id.value(), packArc(x.expected, models), packArc(x.replacement, models)};
    const auto& current = host->layout.arcs[record.id];
    record.replacement.child_state_begin = current.child_begin;
    if (current.child_count != record.replacement.child_count) {
      record.replacement.child_state_begin = view.child_state_count;
      view.child_state_count = checkedAdd(view.child_state_count, record.replacement.child_count);
    }
    packed.payloads.push_back(record);
    const auto transitions = pathTransitions(x.replacement.kind, x.replacement.transfer, x.replacement.eligibility.data_arrival);
    if (transitions != host->bindings[record.id].path_transitions)
      changed_nodes.push_back(host->bindings[record.id].sink);
    touch(record.id);
  }
  for (const auto& x : patch.arc_appends) {
    auto arc = packArc(x.payload, models);
    arc.child_state_begin = view.child_state_count;
    view.child_state_count = checkedAdd(view.child_state_count, arc.child_count);
    packed.appends.push_back(arc);
    if (pathTransitions(x.payload.kind, x.payload.transfer, x.payload.eligibility.data_arrival))
      changed_nodes.push_back(arc.sink);
  }
  for (const auto& row : patch.appended_base_out_rows)
    for (auto id : row.arc_ids)
      touch(id.value());
  storage->aux_bindings.clear();
  storage->aux_bindings.reserve(patch.aux_rows.size());
  for (const auto& row : patch.aux_rows) {
    AuxPatch record;
    record.source = row.source.value();
    record.expected_offset = packed.aux_values.size();
    record.expected_count = row.expected_arc_ids.size();
    for (auto id : row.expected_arc_ids)
      packed.aux_values.push_back(id.value());
    record.replacement_offset = packed.aux_values.size();
    record.replacement_count = row.replacement_arc_ids.size();
    for (auto id : row.replacement_arc_ids) {
      packed.aux_values.push_back(id.value());
      touch(id.value());
    }
    packed.aux_patches.push_back(record);
    storage->aux_bindings.push_back({record.source, {}, record.replacement_count});
  }
  const auto replacementIds = [&](const auto& change) -> std::optional<std::vector<Index>> {
    if (!change)
      return std::nullopt;
    std::vector<Index> values;
    values.reserve(change->replacement.size());
    std::set<Index> unique;
    for (auto id : change->replacement) {
      demand(id.value() < patch.after.node_slots && unique.insert(id.value()).second, "invalid or duplicate root or end");
      values.push_back(id.value());
    }
    return values;
  };
  auto roots = replacementIds(patch.roots), ends = replacementIds(patch.ends);
  std::vector<Index> out_tail, in_tail;
  const auto appendOffsets = [](const auto& rows, Index offset, auto& tail) {
    tail.reserve(rows.size());
    for (const auto& row : rows) {
      offset = checkedAdd(offset, row.arc_ids.size());
      tail.push_back(offset);
    }
  };
  appendOffsets(patch.appended_base_out_rows, host->out_offsets.back(), out_tail);
  appendOffsets(patch.appended_base_in_rows, host->in_offsets.back(), in_tail);
  std::map<Index, PathAdjacency> adjacency_appends;
  for (const auto& item : patch.arc_appends) {
    adjacency_appends[item.source.value()].outgoing.push_back(item.id.value());
    adjacency_appends[item.sink.value()].incoming.push_back(item.id.value());
  }
  std::vector<NodeSeed> node_seeds;
  std::vector<PathSeed> path_seeds;
  std::vector<std::string> clock_names;
  for (const auto& seed : patch.appended_start_seeds) {
    changed_nodes.push_back(seed.seed_node.value());
    const auto old_name = std::ranges::find(host->layout.clock_names, seed.clock_tag);
    Index name = old_name - host->layout.clock_names.begin();
    if (old_name == host->layout.clock_names.end()) {
      auto found = std::ranges::find(clock_names, seed.clock_tag);
      if (found == clock_names.end()) {
        clock_names.push_back(seed.clock_tag);
        found = clock_names.end() - 1;
      }
      name += found - clock_names.begin();
    }
    NodeState node;
    node.clock_name = name;
    node.arrival = seed.path_source ? *seed.arrival[0][0] : 0.0;
    for (unsigned analysis = 0; analysis < 2; ++analysis)
      for (unsigned transition = 0; transition < 2; ++transition)
        node.slew[analysis * 2 + transition] = {*seed.slew[analysis][transition], true};
    auto position = std::ranges::lower_bound(node_seeds, seed.seed_node.value(), {}, &NodeSeed::node);
    if (position != node_seeds.end() && position->node == seed.seed_node.value())
      position->state = node;
    else
      node_seeds.insert(position, {seed.seed_node.value(), node});
    if (!seed.path_source)
      continue;
    const auto tag = static_cast<Index>(std::ranges::find(host->layout.clock_tags, seed.clock_tag) - host->layout.clock_tags.begin());
    for (Index analysis = 0; analysis < 2; ++analysis)
      for (Index transition = 0; transition < 2; ++transition) {
        PathState state;
        state.present = true;
        state.arrival = *seed.arrival[analysis][transition];
        state.slew = *seed.slew[analysis][transition];
        state.start = seed.path_start_node.value();
        path_seeds.push_back({(seed.seed_node.value() * 2 + analysis) * 4 + transition, tag, state});
      }
  }
  // Finish every host capacity change and capture actual old values before apply.
  const auto reserve = []<class T>(std::vector<T>& values, Index count) {
    if (count > values.capacity())
      values.reserve(std::max(count, checkedAdd(values.capacity(), values.capacity() / 2)));
  };
  reserve(host->out_offsets, checkedAdd(patch.after.node_slots, 1));
  reserve(host->in_offsets, checkedAdd(patch.after.node_slots, 1));
  reserve(host->bindings, patch.after.arc_slots);
  reserve(host->adjacency, patch.after.node_slots);
  undo.adjacency.clear();
  for (const auto& [node, row] : adjacency_appends)
    if (node < prior.view.node_count) {
      auto& current = host->adjacency[node];
      reserve(current.incoming, checkedAdd(current.incoming.size(), row.incoming.size()));
      reserve(current.outgoing, checkedAdd(current.outgoing.size(), row.outgoing.size()));
      undo.adjacency.push_back({node, current.incoming.size(), current.outgoing.size()});
    }
  reserve(host->layout.arcs, patch.after.arc_slots);
  reserve(host->layout.child_indices, view.child_state_count);
  reserve(host->layout.clock_names, checkedAdd(host->layout.clock_names.size(), clock_names.size()));
  reserve(host->node_seeds, checkedAdd(host->node_seeds.size(), node_seeds.size()));
  reserve(host->path_seeds, checkedAdd(host->path_seeds.size(), path_seeds.size()));
  undo.bindings.clear();
  undo.layouts.clear();
  undo.child_indices.clear();
  for (Index id : touched) {
    undo.bindings.emplace_back(id, host->bindings[id]);
    undo.layouts.emplace_back(id, host->layout.arcs[id]);
  }
  for (const auto& item : packed.payloads) {
    const auto& current = host->layout.arcs[item.id];
    if (current.child_count == item.replacement.child_count)
      for (Index i = 0; i < current.child_count; ++i)
        undo.child_indices.emplace_back(current.child_begin + i, host->layout.child_indices[current.child_begin + i]);
  }
  undo.nodes = prior.view.node_count;
  undo.arcs = prior.view.arc_count;
  undo.children = host->layout.child_indices.size();
  undo.names = host->layout.clock_names.size();
  undo.node_seeds = host->node_seeds.size();
  undo.path_seeds = host->path_seeds.size();
  undo.target = host;
  undo.roots = std::move(roots);
  undo.ends = std::move(ends);
  if (undo.roots)
    host->roots.swap(*undo.roots);
  if (undo.ends)
    host->ends.swap(*undo.ends);
  host->out_offsets.insert(host->out_offsets.end(), out_tail.begin(), out_tail.end());
  host->in_offsets.insert(host->in_offsets.end(), in_tail.begin(), in_tail.end());
  host->layout.child_indices.resize(view.child_state_count);
  host->adjacency.resize(patch.after.node_slots);
  for (auto& [node, row] : adjacency_appends) {
    auto& current = host->adjacency[node];
    if (node >= prior.view.node_count)
      current = std::move(row);
    else {
      current.incoming.insert(current.incoming.end(), row.incoming.begin(), row.incoming.end());
      current.outgoing.insert(current.outgoing.end(), row.outgoing.begin(), row.outgoing.end());
    }
  }
  for (const auto& x : packed.deactivations) {
    host->bindings[x.id].active = false;
    host->layout.arcs[x.id].active = false;
  }
  for (std::size_t i = 0; i < packed.payloads.size(); ++i) {
    const auto& item = packed.payloads[i];
    const auto& source = patch.payload_changes[i].replacement;
    const auto& replacement = item.replacement;
    host->layout.arcs[item.id] = {replacement.child_state_begin, replacement.child_count, true};
    for (Index child = 0; child < replacement.child_count; ++child)
      host->layout.child_indices[replacement.child_state_begin + child] = source.transfer.children()[child].signature.arc_index;
    host->bindings[item.id].levelization = replacement.levelization;
    host->bindings[item.id].path_transitions = pathTransitions(source.kind, source.transfer, source.eligibility.data_arrival);
  }
  for (std::size_t i = 0; i < packed.appends.size(); ++i) {
    const auto& arc = packed.appends[i];
    const auto& source = patch.arc_appends[i].payload;
    for (Index child = 0; child < arc.child_count; ++child)
      host->layout.child_indices[arc.child_state_begin + child] = source.transfer.children()[child].signature.arc_index;
    host->bindings.push_back({arc.source, arc.sink, true, arc.levelization, EdgeStorageKind::kBaseOut,
                              pathTransitions(source.kind, source.transfer, source.eligibility.data_arrival)});
    host->layout.arcs.push_back({arc.child_state_begin, arc.child_count, true});
  }
  for (const auto& x : patch.changes)
    host->bindings[x.replacement_arc.value()].storage_kind = EdgeStorageKind::kBaseOut;
  for (const auto& row : patch.appended_base_out_rows)
    for (auto id : row.arc_ids)
      host->bindings[id.value()].storage_kind = EdgeStorageKind::kBaseOut;
  for (const auto& row : patch.aux_rows)
    for (auto id : row.replacement_arc_ids)
      host->bindings[id.value()].storage_kind = EdgeStorageKind::kAuxOut;
  for (auto& name : clock_names)
    host->layout.clock_names.push_back(std::move(name));
  host->node_seeds.insert(host->node_seeds.end(), node_seeds.begin(), node_seeds.end());
  host->path_seeds.insert(host->path_seeds.end(), path_seeds.begin(), path_seeds.end());
  if (!changed_nodes.empty() || patch.after.node_slots != prior.view.node_count) {
    domain_index = 1 - prior.domain_index;
    updatePathDomain(host->bindings, host->adjacency, host->path_seeds, storage->domains[prior.domain_index].host,
                     changed_nodes, storage->domains[domain_index].host);
  }
  view.path_count = storage->domains[domain_index].host.tags.size();
  table_count = models.tables.size();
  view.node_count = patch.after.node_slots;
  view.arc_count = patch.after.arc_slots;
  view.child_count = models.children.size();
  view.tag_count = prior.view.tag_count;
  view.clock_count = prior.view.clock_count;
  view.root_count = host->roots.size();
  view.end_count = host->ends.size();
  view.endpoint_count = checkedAdd(prior.view.endpoint_count, patch.appended_endpoints.size());
  return packed;
}
}  // namespace ista::csr
