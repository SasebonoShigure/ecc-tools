#include "CudaDualCsrInternal.cuh"

namespace ista::csr {
using namespace cuda_detail;
namespace {
__global__ void updateRc(DeviceView view, const RcNetUpdate* nets, Index net_count,
                         const RcBindingUpdate* bindings, Index binding_count)
{
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x;
       i < net_count || i < binding_count; i += static_cast<Index>(gridDim.x) * blockDim.x) {
    if (i < net_count) view.rc_nets[nets[i].index] = nets[i].value;
    if (i < binding_count) const_cast<RcBinding*>(view.rc_bindings)[bindings[i].index] = bindings[i].value;
  }
}
void uploadUpdates(Runtime& memory, RcPatchStorage& patch, DeviceView view,
                   const std::vector<RcNetUpdate>& nets, const std::vector<RcBindingUpdate>& bindings,
                   unsigned threads, CsrCudaTelemetry& telemetry)
{
  memory.upload(nets, patch.view_upload.data);
  memory.upload(bindings, patch.binding_upload.data);
  const Index count = std::max(nets.size(), bindings.size());
  if (count) {
    const auto blocks = static_cast<unsigned>(std::min<Index>((count - 1) / threads + 1, 65535));
    updateRc<<<blocks, threads, 0, memory.stream()>>>(view, patch.view_upload.data, nets.size(),
                                                    patch.binding_upload.data, bindings.size());
    check(cudaGetLastError(), "CUDA RC descriptor update");
    ++telemetry.kernel_launches;
  }
}
}

void CudaDualCsrSession::Impl::transferRcPatch(const Impl& prior)
{
  if (!view.rc_net_count) return;
  auto& patch = storage->rc_patch;
  auto& graph = storage->graph;
  auto grow = [&]<class T>(DeviceBuffer<T>& buffer, Index count, Index preserve = 0) {
    if (memory.ensure(buffer, count, preserve)) ++telemetry.grown_buffers;
    return buffer.data;
  };
  grow(graph.rc_views, view.rc_net_count, prior.view.rc_net_count);
  grow(graph.rc_bindings, host->rc.bindings.size(), storage->host_undo.rc_binding_count);
  grow(graph.rc_net_by_node, view.node_count, prior.view.node_count);
  if (graph.rc_nets.size() < view.rc_net_count) graph.rc_nets.resize(view.rc_net_count);
  if (patch.work_nets.size() < view.rc_net_count) patch.work_nets.resize(view.rc_net_count);
  graph.rc_upload.resize(view.rc_net_count);
  if (patch.work_views.size() < view.rc_net_count) patch.work_views.resize(view.rc_net_count);
  patch.views.reserve(patch.nets.size());
  patch.undo_views.reserve(patch.nets.size());
  grow(patch.view_upload, patch.nets.size());
  grow(patch.binding_upload, patch.bindings.size());
  for (const auto& [index, source] : patch.nets) {
    auto& target = patch.work_nets[index];
    target.owner.reset();
    RcNet descriptor;
    std::copy(source.tree.lanes.begin(), source.tree.lanes.end(), descriptor.lanes);
    descriptor.models = grow(target.models, source.tree.models.size());
    descriptor.terminals = grow(target.terminals, source.tree.terminals.size());
    descriptor.child_count = source.child_count;
    descriptor.rows = grow(target.rows, source.rows.size());
    descriptor.records = grow(target.records, source.record_count);
    descriptor.drivers = grow(target.drivers, 4);
    descriptor.initialize_arcs = grow(target.initialize_arcs, source.initialize_arcs.size());
    descriptor.initialize_count = source.initialize_arcs.size();
    patch.work_views[index] = descriptor;
    target.owner = source.owner;
    patch.views.push_back({index, descriptor});
    if (index < prior.view.rc_net_count) patch.undo_views.push_back({index, graph.rc_upload[index]});
  }
  // Allocate before swapping; descriptors must follow their buffer owner.
  patch.buffers_swapped = true;
  for (const auto& [index, source] : patch.nets) {
    static_cast<void>(source);
    std::swap(graph.rc_nets[index], patch.work_nets[index]);
    std::swap(graph.rc_upload[index], patch.work_views[index]);
  }
  bindGraph();
  for (const auto& [index, source] : patch.nets) {
    const auto& target = graph.rc_nets[index];
    memory.upload(source.tree.models, target.models.data);
    memory.upload(source.tree.terminals, target.terminals.data);
    memory.upload(source.rows, target.rows.data);
    memory.upload(source.initialize_arcs, target.initialize_arcs.data);
  }
  // Every new node gets a defined absent entry, including buffer inputs and
  // non-RC edits in a graph that also contains RC nets.
  memory.upload(host->rc.net_by_node.data() + prior.view.node_count,
                const_cast<Index*>(view.rc_net_by_node) + prior.view.node_count, view.node_count - prior.view.node_count);
  const auto begin = storage->uploaded_rc_load_cells;
  const auto& cells = storage->models.rc_load_cells;
  if (cells.size() > begin)
    memory.upload(cells.data() + begin, storage->rc_load_cells.data + begin, cells.size() - begin);
  patch.uploaded = true;
  uploadUpdates(memory, patch, view, patch.views, patch.bindings, storage->kernel_threads, telemetry.cuda);
  storage->uploaded_rc_load_cells = cells.size();
}

void CudaDualCsrSession::Impl::restoreRcPatch(CsrCudaTelemetry& cleanup)
{
  auto& patch = storage->rc_patch;
  if (patch.buffers_swapped) {
    for (const auto& [index, source] : patch.nets) {
      static_cast<void>(source);
      std::swap(storage->graph.rc_nets[index], storage->rc_patch.work_nets[index]);
      std::swap(storage->graph.rc_upload[index], storage->rc_patch.work_views[index]);
    }
    storage->graph.rc_upload.resize(patch.old_net_count);
    patch.buffers_swapped = false;
  }
  bindGraph();
  if (patch.uploaded)
    uploadUpdates(memory, patch, view, patch.undo_views, storage->host_undo.rc_bindings, storage->kernel_threads, cleanup);
}
void CudaDualCsrSession::Impl::clearRcPatch() noexcept
{
  for (const auto& [index, source] : storage->rc_patch.nets) {
    static_cast<void>(source);
    if (index < storage->rc_patch.work_nets.size()) storage->rc_patch.work_nets[index].owner.reset();
  }
  storage->rc_patch.clear();
}
}  // namespace ista::csr
