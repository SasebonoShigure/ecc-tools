#include "CudaDualCsrInternal.cuh"

namespace ista::csr {
using namespace cuda_detail;

void CudaDualCsrSession::Impl::transferRc(HostData& packed)
{
  auto& graph = storage->graph;
  auto grow = [&]<class T>(DeviceBuffer<T>& buffer, Index count) {
    if (memory.ensure(buffer, count)) ++telemetry.grown_buffers;
    return buffer.data;
  };
  // Prepare every buffer and descriptor before the first asynchronous upload.
  if (graph.rc_nets.size() < packed.rc_nets.size())
    graph.rc_nets.resize(packed.rc_nets.size());
  graph.rc_upload.resize(packed.rc_nets.size());
  grow(graph.rc_views, packed.rc_nets.size());
  grow(graph.rc_bindings, packed.rc.bindings.size());
  grow(graph.rc_net_by_node, packed.rc.net_by_node.size());
  for (std::size_t index = 0; index < packed.rc_nets.size(); ++index) {
    auto& source = packed.rc_nets[index];
    auto& target = graph.rc_nets[index];
    auto& descriptor = graph.rc_upload[index];
    packRcTree(*source.owner, source.tree);
    std::copy(source.tree.lanes.begin(), source.tree.lanes.end(), descriptor.lanes);
    grow(target.models, source.tree.models.size());
    grow(target.terminals, source.tree.terminals.size());
    descriptor.models = target.models.data;
    descriptor.terminals = target.terminals.data;
    descriptor.child_count = source.child_count;
    descriptor.rows = grow(target.rows, source.rows.size());
    descriptor.records = grow(target.records, source.record_count);
    descriptor.drivers = grow(target.drivers, 4);
    descriptor.initialize_arcs = grow(target.initialize_arcs, source.initialize_arcs.size());
    descriptor.initialize_count = source.initialize_arcs.size();
  }
  bindGraph();
  for (std::size_t index = 0; index < packed.rc_nets.size(); ++index) {
    const auto& source = packed.rc_nets[index];
    const auto& target = graph.rc_nets[index];
    memory.upload(source.tree.models, target.models.data);
    memory.upload(source.tree.terminals, target.terminals.data);
    memory.upload(source.rows, target.rows.data);
    memory.upload(source.initialize_arcs, target.initialize_arcs.data);
  }
  memory.upload(graph.rc_upload, graph.rc_views.data);
  memory.upload(packed.rc.bindings, graph.rc_bindings.data);
  memory.upload(packed.rc.net_by_node, graph.rc_net_by_node.data);
  const auto begin = storage->uploaded_rc_load_cells;
  const auto& cells = storage->models.rc_load_cells;
  if (cells.size() > begin)
    memory.upload(cells.data() + begin, storage->rc_load_cells.data + begin, cells.size() - begin);
  // All sources are invocation/session-owned and remain live until execute's
  // existing status wait. No additional host synchronization is needed here.
}
}  // namespace ista::csr
