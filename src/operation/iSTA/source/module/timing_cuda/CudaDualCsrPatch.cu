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
  demand(!prior.poisoned && !prior.origin && !prior.code, "CUDA targeted owner is not committed and healthy");
  demand(patch.session_token == prior.token && patch.expected_generation == prior.generation && patch.before == prior.high_water,
         "CUDA targeted session/generation/high-water mismatch");
  demand(patch.expected_generation.value() != absent && patch.target_generation.value() == patch.expected_generation.value() + 1,
         "CUDA targeted generation overflow or discontinuity");
  demand(patch.after.node_slots == checkedAdd(patch.before.node_slots, patch.node_appends.size())
             && patch.after.arc_slots == checkedAdd(patch.before.arc_slots, patch.arc_appends.size()) && patch.after.net_slots >= patch.before.net_slots,
         "CUDA targeted append high-water mismatch");
  checkedProduct(checkedProduct(patch.after.node_slots, 8), prior.view.tag_count);
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
    const auto& offsets = incoming ? prior.in_offsets : prior.out_offsets;
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
    const auto length = x.source.value() < prior.aux_lengths.size() ? prior.aux_lengths[x.source.value()] : 0;
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
                   : prior.bindings[id];
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
    demand(std::find(prior.layout.clock_tags.begin(), prior.layout.clock_tags.end(), seed.clock_tag) != prior.layout.clock_tags.end(),
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
    demand(std::find(prior.layout.clock_tags.begin(), prior.layout.clock_tags.end(), endpoint.clock_name) != prior.layout.clock_tags.end(),
           "CUDA appended endpoint changes clock domain");
    for (const auto& delay : endpoint.output_delay)
      demand(!delay || std::isfinite(*delay), "CUDA invalid endpoint output delay");
  }
  if (patch.roots)
    demand(patch.roots->expected == std::vector<NodeId>(prior.roots.begin(), prior.roots.end()), "CUDA targeted roots mismatch");
  if (patch.ends)
    demand(patch.ends->expected == std::vector<NodeId>(prior.ends.begin(), prior.ends.end()), "CUDA targeted ends mismatch");
}

PackedPatch CudaDualCsrSession::Impl::prepare(const Impl& prior, const CudaDualCsrPatch& patch)
{
  PackedPatch packed;
  out_offsets = prior.out_offsets;
  in_offsets = prior.in_offsets;
  aux_lengths = prior.aux_lengths;
  aux_lengths.resize(patch.after.node_slots);
  bindings = prior.bindings;
  layout = prior.layout;
  layout.is_clock_point.resize(patch.after.node_slots, 0);
  auto resizeClock = [&](ClockResultSoa& clock) {
    const auto size = checkedProduct(patch.after.node_slots, 4);
    auto resize = [size]<class T>(OptionalSoa<T>& field) {
      field.values.resize(size);
      field.presence.resize(size, 0);
    };
    resize(clock.arrivals);
    resize(clock.slews);
    resize(clock.predecessor_nodes);
    resize(clock.predecessor_arc_delays);
    clock.predecessor_transitions.values.resize(size, Transition::kNone);
    clock.predecessor_transitions.presence.resize(size, 0);
  };
  resizeClock(layout.effective_clock);
  roots = prior.roots;
  ends = prior.ends;
  if (patch.roots) {
    roots.clear();
    for (auto id : patch.roots->replacement) {
      demand(id.value() < patch.after.node_slots, "invalid root");
      roots.push_back(id.value());
    }
  }
  if (patch.ends) {
    ends.clear();
    for (auto id : patch.ends->replacement) {
      demand(id.value() < patch.after.node_slots, "invalid end");
      ends.push_back(id.value());
    }
  }
  for (const auto& row : patch.appended_base_out_rows)
    out_offsets.push_back(checkedAdd(out_offsets.back(), row.arc_ids.size()));
  for (const auto& row : patch.appended_base_in_rows)
    in_offsets.push_back(checkedAdd(in_offsets.back(), row.arc_ids.size()));
  for (const auto& x : patch.removes)
    packed.removes.push_back({x.source.value(), x.physical_slot, x.expected_arc.value(), absent});
  for (const auto& x : patch.changes)
    packed.changes.push_back({x.source.value(), x.physical_slot, x.expected_arc ? x.expected_arc->value() : absent, x.replacement_arc.value()});
  for (const auto& x : patch.reverse_changes)
    packed.reverse_changes.push_back({x.sink.value(), x.physical_slot, x.expected_arc.value(), x.replacement_arc.value()});
  for (const auto& x : patch.logical_deactivations) {
    demand(bindings[x.id.value()].active && bindings[x.id.value()].source == x.source.value() && bindings[x.id.value()].sink == x.sink.value(),
           "deactivation binding mismatch");
    packed.deactivations.push_back({x.id.value(), x.source.value(), x.sink.value()});
    bindings[x.id.value()].active = false;
    layout.arcs[x.id.value()].active = false;
  }
  for (const auto& x : patch.payload_changes) {
    PayloadPatch record;
    record.id = x.id.value();
    record.expected = packArc(x.expected, packed.expected_children, packed.expected_tables);
    record.replacement = packArc(x.replacement, packed.children, packed.tables, prior.view.child_count, prior.table_count);
    packed.payloads.push_back(record);
    bindings[record.id].levelization = record.replacement.levelization;
    layout.arcs[record.id] = {record.replacement.child_begin, record.replacement.child_count, true};
  }
  for (const auto& x : patch.arc_appends) {
    auto arc = packArc(x.payload, packed.children, packed.tables, prior.view.child_count, prior.table_count);
    packed.appends.push_back(arc);
    bindings.push_back({arc.source, arc.sink, true, arc.levelization});
    layout.arcs.push_back({arc.child_begin, arc.child_count, true});
  }
  // Incidence validation guarantees one final outgoing occurrence per active
  // arc. Keep its real storage membership without mirroring physical slots.
  for (const auto& x : patch.changes)
    bindings[x.replacement_arc.value()].storage_kind = EdgeStorageKind::kBaseOut;
  for (const auto& row : patch.appended_base_out_rows)
    for (auto id : row.arc_ids)
      bindings[id.value()].storage_kind = EdgeStorageKind::kBaseOut;
  for (const auto& row : patch.aux_rows)
    for (auto id : row.replacement_arc_ids)
      bindings[id.value()].storage_kind = EdgeStorageKind::kAuxOut;
  for (const auto& child : packed.children)
    layout.child_indices.push_back(child.arc_index);
  for (const auto& x : patch.aux_rows) {
    const Index source = x.source.value();
    const Index old_row = source < prior.aux_mapping.size() ? prior.aux_mapping[source] : absent;
    packed.aux_expected.push_back({old_row == absent ? 0 : prior.aux_offsets[old_row], packed.expected_aux_arcs.size(), x.expected_arc_ids.size()});
    for (auto id : x.expected_arc_ids)
      packed.expected_aux_arcs.push_back(id.value());
    aux_lengths[source] = x.replacement_arc_ids.size();
  }
  aux_offsets = {0};
  aux_mapping.assign(patch.after.node_slots, absent);
  for (Index source = 0; source < aux_lengths.size(); ++source)
    if (aux_lengths[source]) {
      aux_mapping[source] = aux_offsets.size() - 1;
      aux_offsets.push_back(checkedAdd(aux_offsets.back(), aux_lengths[source]));
    }
  table_count = checkedAdd(prior.table_count, packed.tables.size());
  view.node_count = patch.after.node_slots;
  view.arc_count = patch.after.arc_slots;
  view.child_count = checkedAdd(prior.view.child_count, packed.children.size());
  view.tag_count = prior.view.tag_count;
  view.clock_count = prior.view.clock_count;
  view.root_count = roots.size();
  view.end_count = ends.size();
  view.endpoint_count = checkedAdd(prior.view.endpoint_count, patch.appended_endpoints.size());
  view.aux_row_count = aux_offsets.size() - 1;
  return packed;
}
}  // namespace ista::csr
