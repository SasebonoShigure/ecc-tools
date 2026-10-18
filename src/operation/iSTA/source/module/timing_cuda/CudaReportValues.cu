// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <chrono>
#include <cstring>
#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"

namespace ista::csr {
namespace {
// D2H transfers whole records, including padding initialized in the source.
template <class T> __global__ void collectReportNodes(const cuda_detail::NodeState* source,
    const cuda_detail::Index* ids, cuda_detail::Index count, T* output)
{
  using cuda_detail::Index;
  for (Index slot = blockIdx.x * blockDim.x + threadIdx.x; slot < count; slot += blockDim.x * gridDim.x)
    memcpy(&output[slot], &static_cast<const T&>(source[ids ? ids[slot] : slot]), sizeof(T));
}
__global__ void collectReportPaths(const value_detail::PathState* source,
    const cuda_detail::Index* ids, cuda_detail::Index count, value_detail::PathState* output)
{
  using cuda_detail::Index;
  for (Index slot = blockIdx.x * blockDim.x + threadIdx.x; slot < count; slot += blockDim.x * gridDim.x)
    memcpy(&output[slot], &source[ids[slot]], sizeof(value_detail::PathState));
}
}  // namespace

void CudaDualCsrSession::Impl::prepareReportNodes(cuda_detail::ReportTransfer& transfer, std::size_t count) const
{
  if (timing_scalars) transfer.node_details.resize(count);
  else transfer.node_values.resize(count);
}
value_detail::NodeValue CudaDualCsrSession::Impl::reportNode(const cuda_detail::ReportTransfer& transfer, std::size_t slot) const
{
  if (!timing_scalars) return transfer.node_values[slot];
  const auto id = transfer.nodes.empty() ? slot : transfer.nodes[slot];
  return {(*timing_scalars)[id], transfer.node_details[slot]};
}
void CudaDualCsrSession::Impl::enqueueReportRead(cuda_detail::ReportTransfer& transfer)
{
  using namespace cuda_detail;
  telemetry.grown_buffers += memory.ensure(storage->report_nodes, transfer.nodes.size());
  telemetry.grown_buffers += memory.ensure(storage->report_node_data,
      std::max(checkedProduct(transfer.node_values.size(), sizeof(value_detail::NodeValue)),
               checkedProduct(transfer.node_details.size(), sizeof(value_detail::NodeDetails))));
  telemetry.grown_buffers += memory.ensure(storage->report_paths, transfer.paths.size());
  telemetry.grown_buffers += memory.ensure(storage->report_path_values, transfer.paths.size());
  // cudaMalloc alignment supports each concrete projection; only one uses this scratch per query.
  auto* nodes = reinterpret_cast<value_detail::NodeValue*>(storage->report_node_data.data);
  auto* details = reinterpret_cast<value_detail::NodeDetails*>(storage->report_node_data.data);
  if (!transfer.nodes.empty()) memory.upload(transfer.nodes, storage->report_nodes.data);
  const auto collect = [&](auto* output, std::size_t count) {
    if (count == 0) return;
    collectReportNodes<<<kernelBlocks(count, storage->kernel_threads), storage->kernel_threads, 0, memory.stream()>>>(
        view.nodes, transfer.nodes.empty() ? nullptr : storage->report_nodes.data, count, output);
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "collect report nodes");
  };
  collect(nodes, transfer.node_values.size());
  collect(details, transfer.node_details.size());
  if (!transfer.paths.empty()) {
    memory.upload(transfer.paths, storage->report_paths.data);
    collectReportPaths<<<kernelBlocks(transfer.paths.size(), storage->kernel_threads), storage->kernel_threads, 0, memory.stream()>>>(
        view.paths, storage->report_paths.data, transfer.paths.size(), storage->report_path_values.data);
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "collect report paths");
  }
  memory.record(6);
  memory.download(nodes, transfer.node_values);
  memory.download(details, transfer.node_details);
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
  prepareReportNodes(transfer, transfer.nodes.size());
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
    for (auto& [id, value] : added_nodes) { (void) id; value.timing = reportNode(transfer, slot++); }
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
const std::vector<value_detail::NodeTimingScalars>& CudaDualCsrSession::Impl::readTimingScalars()
{
  using namespace cuda_detail;
  using value_detail::NodeTimingScalars;
  if (timing_scalars) return *timing_scalars;
  const auto start = std::chrono::steady_clock::now();
  std::vector<NodeTimingScalars> result(view.node_count);
  if (values) {
    const auto& nodes = values->nodes;
    for (Index node = 0; node < view.node_count; ++node)
      result[node] = {nodes.arrivals.values[node], nodes.requireds.values[node], nodes.slacks.values[node]};
  } else {
    std::vector<Index> missing;
    if (!report_values.nodes.empty()) {
      missing.reserve(view.node_count - report_values.nodes.size());
      auto cached = report_values.nodes.begin();
      for (Index node = 0; node < view.node_count; ++node) {
        if (cached != report_values.nodes.end() && cached->first == node) {
          result[node] = static_cast<const NodeTimingScalars&>(cached->second.timing);
          ++cached;
        } else missing.push_back(node);
      }
    }
    std::vector<NodeTimingScalars> added(missing.size());
    const auto count = report_values.nodes.empty() ? result.size() : missing.size();
    if (count) {
      memory.bindTelemetry(telemetry.cuda);
      try {
        telemetry.grown_buffers += memory.ensure(storage->report_nodes, missing.size());
        telemetry.grown_buffers += memory.ensure(storage->report_node_data, checkedProduct(count, sizeof(NodeTimingScalars)));
        auto* output = reinterpret_cast<NodeTimingScalars*>(storage->report_node_data.data);
        if (!missing.empty()) memory.upload(missing, storage->report_nodes.data);
        collectReportNodes<<<kernelBlocks(count, storage->kernel_threads), storage->kernel_threads, 0, memory.stream()>>>(
            view.nodes, missing.empty() ? nullptr : storage->report_nodes.data, count, output);
        ++telemetry.cuda.kernel_launches;
        check(cudaGetLastError(), "collect timing scalars");
        const auto prior_readback = telemetry.cuda.d2h_bytes;
        const auto prior_wait = telemetry.cuda.stream_wait_ms;
        memory.record(6);
        memory.download(output, missing.empty() ? result.data() : added.data(), count);
        memory.record(7);
        memory.synchronize();
        telemetry.cuda.delivery_d2h_ms += memory.milliseconds(6, 7);
        telemetry.cuda.result_d2h_bytes += telemetry.cuda.d2h_bytes - prior_readback;
        telemetry.cuda.result_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
      } catch (const CudaAllocationError&) {
        throw;
      } catch (const CudaError&) {
        try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
        poisoned = true;
        storage->poisoned = true;
        throw;
      }
      for (std::size_t slot = 0; slot < missing.size(); ++slot) result[missing[slot]] = added[slot];
    }
  }
  timing_scalars.emplace(std::move(result));
  telemetry.materialize_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return *timing_scalars;
}
const std::vector<value_detail::NodeTimingScalars>& CudaDualCsrSession::timingScalars() const
{
  requireCommitted();
  return _impl->readTimingScalars();
}

const CsrReportValues& CudaDualCsrSession::reportValues(
    std::span<const NodeId> nodes, std::span<const CsrQueryPath> paths) const
{
  requireCommitted();
  return _impl->readReportValues(nodes, paths);
}
}  // namespace ista::csr
