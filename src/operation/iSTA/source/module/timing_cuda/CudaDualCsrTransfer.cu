#include <algorithm>
#include <cmath>
#include <map>

#include "CudaDualCsrInternal.cuh"

namespace ista::csr {
using namespace cuda_detail;
void CudaDualCsrSession::Impl::allocate()
{
  view.out_offsets = memory.allocate<Index>(out_offsets.size());
  view.out_arcs = memory.allocate<Index>(out_offsets.back());
  view.in_offsets = memory.allocate<Index>(in_offsets.size());
  view.in_arcs = memory.allocate<Index>(in_offsets.back());
  view.aux_offsets = memory.allocate<Index>(aux_offsets.size());
  view.aux_arcs = memory.allocate<Index>(aux_offsets.back());
  view.source_to_aux_row = memory.allocate<Index>(aux_mapping.size());
  view.roots = memory.allocate<Index>(roots.size());
  view.ends = memory.allocate<Index>(ends.size());
  view.endpoints = memory.allocate<Endpoint>(view.endpoint_count);
  view.arcs = memory.allocate<Arc>(view.arc_count);
  view.children = memory.allocate<Child>(view.child_count);
  view.table_data = memory.allocate<double>(table_count);
  seed_nodes = memory.allocate<NodeState>(view.node_count);
  seed_paths = memory.allocate<PathState>(checkedProduct(checkedProduct(view.node_count, 8), view.tag_count));
  view.nodes = memory.allocate<NodeState>(view.node_count);
  view.paths = memory.allocate<PathState>(checkedProduct(checkedProduct(view.node_count, 8), view.tag_count));
  view.arc_state = memory.allocate<ArcState>(view.arc_count);
  view.child_state = memory.allocate<ChildState>(view.child_count);
  view.indegrees = memory.allocate<Index>(view.node_count);
  view.order = memory.allocate<Index>(view.node_count);
  view.node_levels = memory.allocate<Index>(view.node_count);
  view.level_offsets = memory.allocate<Index>(checkedAdd(view.node_count, 1));
  view.level_offset_count = memory.allocate<Index>(1);
  view.status = memory.allocate<Status>(1);
  if constexpr (incregpusta_debug_checks_enabled) {
    view.storage_visits = memory.allocate<Index>(8);
    memory.zero(view.storage_visits, 8);
  }
  if (origin) {
    const auto& old = *origin;
    auto grew = [&](Index before, Index after) {
      if (after > before)
        ++telemetry.grown_buffers;
    };
    grew(old.out_offsets.size(), out_offsets.size());
    grew(old.out_offsets.back(), out_offsets.back());
    grew(old.in_offsets.size(), in_offsets.size());
    grew(old.in_offsets.back(), in_offsets.back());
    grew(old.aux_offsets.size(), aux_offsets.size());
    grew(old.aux_offsets.back(), aux_offsets.back());
    grew(old.aux_mapping.size(), aux_mapping.size());
    grew(old.roots.size(), roots.size());
    grew(old.ends.size(), ends.size());
    grew(old.view.endpoint_count, view.endpoint_count);
    grew(old.view.arc_count, view.arc_count);
    grew(old.view.arc_count, view.arc_count);
    grew(old.view.child_count, view.child_count);
    grew(old.view.child_count, view.child_count);
    grew(old.table_count, table_count);
    // Node seeds/results, indegrees, order, node-levels and level-offsets.
    for (unsigned buffer = 0; buffer < 6; ++buffer)
      grew(old.view.node_count, view.node_count);
    for (unsigned buffer = 0; buffer < 2; ++buffer)
      grew(checkedProduct(old.view.node_count, old.view.tag_count), checkedProduct(view.node_count, view.tag_count));
  }
  memory.zero(view.status, 1);
  memory.zero(view.level_offset_count, 1);
}

void CudaDualCsrSession::Impl::transferPatch(const Impl& prior, const CudaDualCsrPatch& patch, const PackedPatch& packed)
{
  memory.record(6);
  auto copy = [&]<class T>(const T* source, const T* destination, Index old_size, Index size) {
    demand(size >= old_size, "CUDA candidate buffer shrank below copied prefix");
    memory.copy(source, const_cast<T*>(destination), old_size);
  };
  copy(prior.view.out_arcs, view.out_arcs, prior.out_offsets.back(), out_offsets.back());
  copy(prior.view.in_arcs, view.in_arcs, prior.in_offsets.back(), in_offsets.back());
  copy(prior.view.arcs, view.arcs, prior.view.arc_count, view.arc_count);
  copy(prior.view.children, view.children, prior.view.child_count, view.child_count);
  copy(prior.view.table_data, view.table_data, prior.table_count, table_count);
  copy(prior.seed_nodes, seed_nodes, prior.view.node_count, view.node_count);
  copy(prior.seed_paths, seed_paths, checkedProduct(checkedProduct(prior.view.node_count, 8), view.tag_count),
       checkedProduct(checkedProduct(view.node_count, 8), view.tag_count));
  copy(prior.view.endpoints, view.endpoints, prior.view.endpoint_count, view.endpoint_count);
  std::map<Index, const timing_dual_csr::AuxOutRowDescriptor*> changed;
  for (const auto& row : patch.aux_rows)
    changed.emplace(row.source.value(), &row);
  for (Index source = 0; source < aux_mapping.size(); ++source)
    if (aux_mapping[source] != absent && !changed.contains(source)) {
      const Index old_row = prior.aux_mapping[source];
      demand(old_row != absent, "unchanged Aux row lost origin");
      memory.copy(prior.view.aux_arcs + prior.aux_offsets[old_row], const_cast<Index*>(view.aux_arcs) + aux_offsets[aux_mapping[source]], aux_lengths[source]);
    }
  memory.record(0);
  memory.upload(out_offsets, view.out_offsets);
  memory.upload(in_offsets, view.in_offsets);
  memory.upload(aux_offsets, view.aux_offsets);
  memory.upload(aux_mapping, view.source_to_aux_row);
  memory.upload(roots, view.roots);
  memory.upload(ends, view.ends);
  std::vector<std::vector<Index>> row_uploads;
  auto uploadIds = [&](const auto& ids, const Index* destination) {
    auto& values = row_uploads.emplace_back();
    for (auto id : ids)
      values.push_back(id.value());
    memory.upload(values.data(), const_cast<Index*>(destination), values.size());
  };
  for (const auto& row : patch.appended_base_out_rows)
    uploadIds(row.arc_ids, view.out_arcs + out_offsets[row.source.value()]);
  for (const auto& row : patch.appended_base_in_rows)
    uploadIds(row.arc_ids, view.in_arcs + in_offsets[row.sink.value()]);
  for (const auto& row : patch.aux_rows)
    if (!row.replacement_arc_ids.empty())
      uploadIds(row.replacement_arc_ids, view.aux_arcs + aux_offsets[aux_mapping[row.source.value()]]);
  memory.upload(packed.appends.data(), const_cast<Arc*>(view.arcs) + prior.view.arc_count, packed.appends.size());
  memory.upload(packed.children.data(), const_cast<Child*>(view.children) + prior.view.child_count, packed.children.size());
  memory.upload(packed.tables.data(), const_cast<double*>(view.table_data) + prior.table_count, packed.tables.size());
  std::vector<NodeState> nodes(patch.node_appends.size());
  std::vector<PathState> paths(checkedProduct(checkedProduct(nodes.size(), 8), view.tag_count));
  for (const auto& seed : patch.appended_start_seeds) {
    demand(seed.seed_node.value() >= prior.view.node_count && seed.seed_node.value() < view.node_count && seed.path_start_node.value() < view.node_count
               && !seed.crpr_node && !seed.crpr_transition,
           "CUDA append seed has invalid node or unsupported CRPR");
    demand(seed.availability == StartSeedAvailability::kMaterialized && (!seed.path_source || *seed.path_source == PathSource::kInput),
           "unsupported appended seed");
    const auto found = std::find(layout.clock_tags.begin(), layout.clock_tags.end(), seed.clock_tag);
    demand(found != layout.clock_tags.end(), "appended seed changes clock tag domain");
    const Index tag = found - layout.clock_tags.begin();
    auto name = std::find(layout.clock_names.begin(), layout.clock_names.end(), seed.clock_tag);
    if (name == layout.clock_names.end()) {
      layout.clock_names.push_back(seed.clock_tag);
      name = layout.clock_names.end() - 1;
    }
    const Index local = seed.seed_node.value() - prior.view.node_count;
    auto& node = nodes[local];
    node.clock_name = name - layout.clock_names.begin();
    node.arrival = 0.0;
    for (unsigned a = 0; a < 2; ++a)
      for (unsigned t = 0; t < 2; ++t) {
        demand(seed.slew[a][t] && std::isfinite(*seed.slew[a][t]), "invalid appended seed slew");
        node.slew[a * 2 + t] = {*seed.slew[a][t], true};
        if (seed.path_source) {
          demand(seed.arrival[a][t] && std::isfinite(*seed.arrival[a][t]), "invalid appended seed arrival");
          auto& path = paths[((local * 2 + a) * 2 * 2 + t) * view.tag_count + tag];
          path.present = true;
          path.arrival = *seed.arrival[a][t];
          path.slew = *seed.slew[a][t];
          path.start = seed.path_start_node.value();
        }
      }
    if (seed.path_source)
      node.arrival = *seed.arrival[0][0];
  }
  memory.upload(nodes.data(), seed_nodes + prior.view.node_count, nodes.size());
  memory.upload(paths.data(), seed_paths + prior.view.node_count * 8 * view.tag_count, paths.size());
  std::vector<Endpoint> endpoints;
  for (const auto& source : patch.appended_endpoints) {
    demand(source.node.value() >= prior.view.node_count && source.node.value() < view.node_count, "appended endpoint outside appended nodes");
    endpoints.push_back({source.node.value(), source.clock_period, {source.output_delay[0].value_or(0.0), source.output_delay[0].has_value()}});
  }
  memory.upload(endpoints.data(), const_cast<Endpoint*>(view.endpoints) + prior.view.endpoint_count, endpoints.size());
  DevicePatch device;
  auto uploaded = [&]<class T>(const std::vector<T>& values) {
    auto* pointer = memory.allocate<T>(values.size());
    memory.upload(values, pointer);
    return pointer;
  };
  device.removes = uploaded(packed.removes);
  device.remove_count = packed.removes.size();
  device.changes = uploaded(packed.changes);
  device.change_count = packed.changes.size();
  device.reverse_changes = uploaded(packed.reverse_changes);
  device.reverse_count = packed.reverse_changes.size();
  device.payloads = uploaded(packed.payloads);
  device.payload_count = packed.payloads.size();
  device.deactivations = uploaded(packed.deactivations);
  device.deactivate_count = packed.deactivations.size();
  device.expected_children = uploaded(packed.expected_children);
  device.expected_tables = uploaded(packed.expected_tables);
  device.aux_expected = uploaded(packed.aux_expected);
  device.aux_expected_count = packed.aux_expected.size();
  device.expected_aux_arcs = uploaded(packed.expected_aux_arcs);
  device.old_aux_arcs = prior.view.aux_arcs;
  memory.record(1);
  launchTargetedPatch(view, device, memory.stream());
  telemetry.cuda.kernel_launches += 3;
  memory.synchronize();
}
}  // namespace ista::csr
