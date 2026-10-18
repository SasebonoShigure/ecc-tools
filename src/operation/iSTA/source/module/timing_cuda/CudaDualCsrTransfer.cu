#include <algorithm>
#include <cmath>
#include <map>

#include "CudaDualCsrInternal.cuh"

namespace ista::csr {
using namespace cuda_detail;
void CudaDualCsrSession::Impl::bindGraph() noexcept
{
  view.out_offsets = storage->graph.out_offsets.data;
  view.in_offsets = storage->graph.in_offsets.data;
  view.roots = storage->graph.roots.data;
  view.ends = storage->graph.ends.data;
  seed_nodes = storage->graph.node_seeds.data;
  seed_paths = storage->graph.path_seeds.data;
  view.out_arcs = storage->graph.out_arcs.data;
  view.aux_rows = storage->graph.aux_rows.data;
  view.in_arcs = storage->graph.in_arcs.data;
  view.arcs = storage->graph.arcs.data;
  view.children = storage->children.data;
  view.table_data = storage->tables.data;
  view.rc_load_cells = storage->rc_load_cells.data;
  view.rc_nets = storage->graph.rc_views.data;
  view.rc_bindings = storage->graph.rc_bindings.data;
  view.rc_net_by_node = storage->graph.rc_net_by_node.data;
  view.endpoints = storage->graph.endpoints.data;
  view.indegrees = storage->indegrees.data;
  view.path_rows = storage->domain.rows.data;
  view.path_tags = storage->domain.tags.data;
}

std::uint64_t CudaDualCsrSession::Impl::bindAuxRows()
{
  if (storage->aux_bindings.empty())
    return 0;
  // This descriptor capacity is reserved first, before any graph buffer grows.
  // A failed reservation leaves every committed device Aux pointer unchanged.
  if (storage->aux_binding_upload.capacity < storage->aux_bindings.size())
    return 0;
  for (auto& binding : storage->aux_bindings) {
    if (binding.source >= storage->graph.aux_storage.size())
      continue;
    const auto& row = storage->graph.aux_storage[binding.source];
    binding.before = {row.buffer.data, row.count};
  }
  memory.upload(storage->aux_bindings, storage->aux_binding_upload.data);
  launchAuxBindings(view, storage->aux_binding_upload.data, storage->aux_bindings.size(),
                    storage->graph.aux_rows.capacity, memory.stream(), storage->kernel_threads);
  return 1;
}

void CudaDualCsrSession::Impl::allocate()
{
  auto grow = [&]<class T>(DeviceBuffer<T>& buffer, Index count, Index preserve = 0) {
    if (memory.ensure(buffer, count, preserve))
      ++telemetry.grown_buffers;
    return buffer.data;
  };
  grow(storage->aux_binding_upload, storage->aux_bindings.size());
  const auto* old = origin.get();
  grow(storage->graph.out_arcs, host->out_offsets.back(), old ? host->out_offsets[old->view.node_count] : 0);
  grow(storage->graph.in_arcs, host->in_offsets.back(), old ? host->in_offsets[old->view.node_count] : 0);
  grow(storage->graph.arcs, view.arc_count, old ? old->view.arc_count : 0);
  grow(storage->children, view.child_count, storage->uploaded_children);
  grow(storage->tables, table_count, storage->uploaded_tables);
  grow(storage->rc_load_cells, storage->models.rc_load_cells.size(), storage->uploaded_rc_load_cells);
  grow(storage->graph.endpoints, view.endpoint_count, old ? old->view.endpoint_count : 0);
  if (storage->graph.aux_storage.size() < view.node_count)
    storage->graph.aux_storage.resize(view.node_count);
  grow(storage->graph.aux_rows, view.node_count, old ? old->view.node_count : 0);
  // Only changed Aux rows reserve capacity. Other rows keep their addresses.
  for (const auto& binding : storage->aux_bindings) {
    auto& row = storage->graph.aux_storage[binding.source];
    grow(row.buffer, binding.replacement_count, row.count);
  }
  grow(storage->graph.out_offsets, host->out_offsets.size(), old ? old->view.node_count + 1 : 0);
  grow(storage->graph.in_offsets, host->in_offsets.size(), old ? old->view.node_count + 1 : 0);
  grow(storage->graph.roots, host->roots.size(), old ? old->view.root_count : 0);
  grow(storage->graph.ends, host->ends.size(), old ? old->view.end_count : 0);
  grow(storage->graph.node_seeds, host->node_seeds.size(), old ? storage->host_undo.node_seeds : 0);
  grow(storage->graph.path_seeds, host->path_seeds.size(), old ? storage->host_undo.path_seeds : 0);
  allocatePathDomain();
  bindGraph();
  const Index old_nodes = old ? old->view.node_count : 0;
  memory.zero(const_cast<AuxRow*>(view.aux_rows) + old_nodes, view.node_count - old_nodes);
  auto& result = storage->results[result_index];
  view.nodes = grow(result.nodes, view.node_count);
  view.paths = grow(result.paths, view.path_count);
  view.arc_state = grow(result.arcs, view.arc_count);
  view.child_state = grow(result.children, view.child_state_count);
  view.order = grow(result.order, view.node_count);
  view.node_levels = result.node_levels.data;
  view.level_offsets = result.level_offsets.data;
  view.status = grow(result.status, 1);
  if constexpr (incregpusta_debug_checks_enabled) {
    view.storage_visits = grow(result.storage_visits, 12);
    memory.zero(view.storage_visits, 12);
  }
  memory.zero(view.status, 1);
}

void CudaDualCsrSession::Impl::transferPatch(const Impl& prior, const CudaDualCsrPatch& patch, PackedPatch& packed)
{
  // Allocate and prepare every upload/undo source before submitting async work.
  for (const auto& row : patch.appended_base_out_rows)
    for (auto id : row.arc_ids)
      packed.appended_out.push_back(id.value());
  for (const auto& row : patch.appended_base_in_rows)
    for (auto id : row.arc_ids)
      packed.appended_in.push_back(id.value());
  auto& endpoints = packed.endpoints;
  endpoints.reserve(patch.appended_endpoints.size());
  for (const auto& source : patch.appended_endpoints)
    endpoints.push_back({source.node.value(), source.clock_period,
                         {source.output_delay[0].value_or(0.0), source.output_delay[0].has_value()}});
  auto& slots = packed.slots;
  slots.reserve(packed.removes.size() + packed.changes.size() + packed.reverse_changes.size());
  for (const auto& item : packed.removes)
    slots.push_back({const_cast<Index*>(view.out_arcs) + item.slot, 0});
  for (const auto& item : packed.changes)
    slots.push_back({const_cast<Index*>(view.out_arcs) + item.slot, 0});
  for (const auto& item : packed.reverse_changes)
    slots.push_back({const_cast<Index*>(view.in_arcs) + item.slot, 0});
  for (const auto& item : packed.aux_patches) {
    const auto& row = storage->graph.aux_storage[item.source];
    for (Index i = 0; i < std::min(row.count, item.replacement_count); ++i)
      slots.push_back({row.buffer.data + i, 0});
  }
  packed.seeds.reserve(packed.seed_loads.size());
  for (const auto& load : packed.seed_loads) packed.seeds.push_back({load.slot});
  auto& arcs = packed.arcs;
  arcs.reserve(packed.payloads.size() + packed.deactivations.size());
  for (const auto& item : packed.payloads)
    arcs.push_back({item.id, {}});
  for (const auto& item : packed.deactivations)
    arcs.push_back({item.id, {}});
  auto reserve = [&]<class T>(DeviceBuffer<T>& buffer, std::size_t count) {
    if (memory.ensure(buffer, count))
      ++telemetry.grown_buffers;
    return buffer.data;
  };
  DevicePatch device;
  device.removes = reserve(storage->removes, packed.removes.size());
  device.remove_count = packed.removes.size();
  device.changes = reserve(storage->changes, packed.changes.size());
  device.change_count = packed.changes.size();
  device.reverse_changes = reserve(storage->reverse_changes, packed.reverse_changes.size());
  device.reverse_count = packed.reverse_changes.size();
  device.payloads = reserve(storage->payloads, packed.payloads.size());
  device.payload_count = packed.payloads.size();
  device.seed_nodes = seed_nodes;
  device.seed_loads = reserve(storage->seed_loads, packed.seed_loads.size());
  device.seed_load_count = packed.seed_loads.size();
  reserve(storage->seed_undo, packed.seeds.size());
  storage->seed_undo_count = packed.seeds.size();
  device.deactivations = reserve(storage->deactivations, packed.deactivations.size());
  device.deactivate_count = packed.deactivations.size();
  device.aux_patches = reserve(storage->aux_patches, packed.aux_patches.size());
  device.aux_patch_count = packed.aux_patches.size();
  device.aux_values = reserve(storage->aux_values, packed.aux_values.size());
  reserve(storage->slot_undo, slots.size());
  reserve(storage->arc_undo, arcs.size());
  storage->slot_undo_count = slots.size();
  storage->arc_undo_count = arcs.size();
  try {
    memory.record(0);
    transferRcPatch(prior);
    const auto offset_begin = prior.view.node_count + 1;
    memory.upload(host->out_offsets.data() + offset_begin, const_cast<Index*>(view.out_offsets) + offset_begin, host->out_offsets.size() - offset_begin);
    memory.upload(host->in_offsets.data() + offset_begin, const_cast<Index*>(view.in_offsets) + offset_begin, host->in_offsets.size() - offset_begin);
    telemetry.cuda.kernel_launches += bindAuxRows();
    if (patch.roots)
      memory.upload(host->roots, view.roots);
    if (patch.ends)
      memory.upload(host->ends, view.ends);
    memory.upload(packed.appended_out.data(), const_cast<Index*>(view.out_arcs) + host->out_offsets[prior.view.node_count],
                  packed.appended_out.size());
    memory.upload(packed.appended_in.data(), const_cast<Index*>(view.in_arcs) + host->in_offsets[prior.view.node_count],
                  packed.appended_in.size());
    memory.upload(packed.appends.data(), const_cast<Arc*>(view.arcs) + prior.view.arc_count, packed.appends.size());
    const auto child_begin = storage->uploaded_children, table_begin = storage->uploaded_tables;
    if (view.child_count > child_begin)
      memory.upload(storage->models.children.data() + child_begin, const_cast<Child*>(view.children) + child_begin, view.child_count - child_begin);
    if (table_count > table_begin)
      memory.upload(storage->models.tables.data() + table_begin, const_cast<double*>(view.table_data) + table_begin, table_count - table_begin);
    const auto node_seed_begin = storage->host_undo.node_seeds, path_seed_begin = storage->host_undo.path_seeds;
    if (host->node_seeds.size() > node_seed_begin)
      memory.upload(host->node_seeds.data() + node_seed_begin, seed_nodes + node_seed_begin, host->node_seeds.size() - node_seed_begin);
    if (host->path_seeds.size() > path_seed_begin)
      memory.upload(host->path_seeds.data() + path_seed_begin, seed_paths + path_seed_begin, host->path_seeds.size() - path_seed_begin);
    transferPathDomain();
    memory.upload(endpoints.data(), const_cast<Endpoint*>(view.endpoints) + prior.view.endpoint_count, endpoints.size());
    memory.upload(packed.removes, device.removes);
    memory.upload(packed.changes, device.changes);
    memory.upload(packed.reverse_changes, device.reverse_changes);
    memory.upload(packed.payloads, device.payloads);
    memory.upload(packed.seed_loads, device.seed_loads);
    memory.upload(packed.seeds, storage->seed_undo.data);
    memory.upload(packed.deactivations, device.deactivations);
    memory.upload(packed.aux_patches, device.aux_patches);
    memory.upload(packed.aux_values, device.aux_values);
    memory.upload(slots, storage->slot_undo.data);
    memory.upload(arcs, storage->arc_undo.data);
    launchCaptureUndo(view, storage->slot_undo.data, slots.size(), storage->arc_undo.data, arcs.size(), seed_nodes, storage->seed_undo.data, packed.seeds.size(), memory.stream(), storage->kernel_threads);
    storage->undo_captured = true;
    telemetry.cuda.kernel_launches += !slots.empty() || !arcs.empty() || !packed.seeds.empty();
    memory.record(1);
    telemetry.cuda.kernel_launches += launchTargetedPatch(view, device, memory.stream(), storage->kernel_threads);
    // Upload vectors are session-owned. execute waits before publication or
    // rollback, so no local-lifetime wait is needed here.
    storage->uploaded_children = view.child_count;
    storage->uploaded_tables = table_count;
  } catch (...) {
    // Drain failed uploads before rollback can reuse the persistent staging.
    try {
      memory.synchronize();
    } catch (...) {
      storage->poisoned = true;
    }
    throw;
  }
}
}  // namespace ista::csr
