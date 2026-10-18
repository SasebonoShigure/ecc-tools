// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"

namespace ista::csr {
namespace {
__global__ void collectReportNodes(const cuda_detail::NodeState* source,
    const cuda_detail::Index* ids, cuda_detail::Index count, value_detail::NodeValue* output)
{
  using cuda_detail::Index;
  for (Index slot = blockIdx.x * blockDim.x + threadIdx.x; slot < count; slot += blockDim.x * gridDim.x)
    output[slot] = source[ids ? ids[slot] : slot];
}
__global__ void collectReportPaths(const value_detail::PathState* source,
    const cuda_detail::Index* ids, cuda_detail::Index count, value_detail::PathState* output)
{
  using cuda_detail::Index;
  for (Index slot = blockIdx.x * blockDim.x + threadIdx.x; slot < count; slot += blockDim.x * gridDim.x)
    output[slot] = source[ids[slot]];
}
}  // namespace

void CudaDualCsrSession::Impl::enqueueReportRead(cuda_detail::ReportTransfer& transfer)
{
  using namespace cuda_detail;
  telemetry.grown_buffers += memory.ensure(storage->report_nodes, transfer.nodes.size());
  telemetry.grown_buffers += memory.ensure(storage->report_node_values, transfer.node_values.size());
  telemetry.grown_buffers += memory.ensure(storage->report_paths, transfer.paths.size());
  telemetry.grown_buffers += memory.ensure(storage->report_path_values, transfer.paths.size());
  if (!transfer.node_values.empty()) {
    if (!transfer.nodes.empty()) memory.upload(transfer.nodes, storage->report_nodes.data);
    collectReportNodes<<<kernelBlocks(transfer.node_values.size(), storage->kernel_threads), storage->kernel_threads, 0, memory.stream()>>>(
        view.nodes, transfer.nodes.empty() ? nullptr : storage->report_nodes.data,
        transfer.node_values.size(), storage->report_node_values.data);
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "collect report nodes");
  }
  if (!transfer.paths.empty()) {
    memory.upload(transfer.paths, storage->report_paths.data);
    collectReportPaths<<<kernelBlocks(transfer.paths.size(), storage->kernel_threads), storage->kernel_threads, 0, memory.stream()>>>(
        view.paths, storage->report_paths.data, transfer.paths.size(), storage->report_path_values.data);
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "collect report paths");
  }
  memory.record(6);
  if (!transfer.node_values.empty()) memory.download(storage->report_node_values.data, transfer.node_values);
  if (!transfer.path_values.empty())
    memory.download(transfer.paths.empty() ? view.paths : storage->report_path_values.data, transfer.path_values);
}

const CsrReportValues& CudaDualCsrSession::Impl::readReportValues(
    std::span<const NodeId> nodes, std::span<const CsrQueryPath> paths)
{
  using namespace cuda_detail;
  const auto& domain = storage->domain.host;
  for (const auto id : nodes)
    demand(id.value() < view.node_count, "report node is out of range");
  for (const auto path : paths) {
    demand(path.row < domain.rows.size(), "report path row is out of range");
    const auto row = domain.rows[path.row];
    demand(path.slot >= row.begin && path.slot < row.end, "report path slot is outside its current row");
  }
  if (values) {
    report_values.complete = &*values;
    return report_values;
  }
  // Allocate every cache node and transfer buffer before enqueuing asynchronous work.
  std::map<Index, CsrQueryNode> added_nodes;
  std::map<Index, value_detail::PathState> added_paths;
  for (const auto id : nodes)
    if (!report_values.nodes.contains(id.value()))
      added_nodes.try_emplace(id.value(), CsrQueryNode{{}, levelOf(id.value())});
  for (const auto path : paths)
    if (!report_values.paths.contains(path.slot))
      added_paths.try_emplace(path.slot);
  if (added_nodes.empty() && added_paths.empty()) return report_values;
  ReportTransfer transfer;
  transfer.nodes.reserve(added_nodes.size());
  transfer.paths.reserve(added_paths.size());
  for (const auto& [id, value] : added_nodes) { (void) value; transfer.nodes.push_back(id); }
  for (const auto& [id, value] : added_paths) { (void) value; transfer.paths.push_back(id); }
  transfer.node_values.resize(transfer.nodes.size());
  transfer.path_values.resize(transfer.paths.size());
  memory.bindTelemetry(telemetry.cuda);
  try {
    const auto prior_readback = telemetry.cuda.d2h_bytes;
    const auto prior_wait = telemetry.cuda.stream_wait_ms;
    enqueueReportRead(transfer);
    memory.record(7);
    memory.synchronize();
    telemetry.cuda.delivery_d2h_ms += memory.milliseconds(6, 7);
    telemetry.cuda.result_d2h_bytes += telemetry.cuda.d2h_bytes - prior_readback;
    telemetry.cuda.result_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
    std::size_t slot = 0;
    for (auto& [id, value] : added_nodes) { (void) id; value.timing = transfer.node_values[slot++]; }
    slot = 0;
    for (auto& [id, value] : added_paths) { (void) id; value = transfer.path_values[slot++]; }
    report_values.nodes.merge(added_nodes);
    report_values.paths.merge(added_paths);
    return report_values;
  } catch (const CudaAllocationError&) {
    throw;
  } catch (const CudaError&) {
    try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
    poisoned = true;
    storage->poisoned = true;
    throw;
  }
}
const CsrReportValues& CudaDualCsrSession::reportValues(
    std::span<const NodeId> nodes, std::span<const CsrQueryPath> paths) const
{
  requireCommitted();
  return _impl->readReportValues(nodes, paths);
}
}  // namespace ista::csr
