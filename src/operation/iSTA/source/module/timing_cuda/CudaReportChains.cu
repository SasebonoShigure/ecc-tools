// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <cub/device/device_scan.cuh>
#include <tuple>
#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"
#include "CsrCudaSequentialPropagation.cuh"

namespace ista::csr {
using namespace cuda_detail;
namespace {
// Count and fill walk the same immutable generation with one thread per root.
__device__ Index walkReportChain(DeviceView view, ReportChain chain, ReportChainStep* output, Status* status)
{
  Index count = 0;
  while (chain.node != absent) {
    if (chain.node >= view.node_count || count == view.node_count) {
      recordFailure(status, Status::invariant);
      break;
    }
    ReportChainStep step{chain.node, 0, absent, chain.tag};
    Index predecessor = absent;
    unsigned transition = 2;
    if (chain.transition < 2) {
      if (chain.clock) {
        const auto& node = view.nodes[chain.node];
        const auto& clock = node.physical_clock[chain.analysis * 2 + chain.transition];
        if (node.propagated_clock) {
          predecessor = clock.predecessor;
          transition = clock.predecessor_transition;
        }
      } else {
        step.row = pathRow(chain.node, chain.analysis, chain.source, chain.transition);
        if (const auto* path = lookupPath(view, step.row, chain.tag)) {
          step.slot = static_cast<Index>(path - view.paths);
          if (path->present) {
            predecessor = path->predecessor_node;
            transition = path->predecessor_transition;
          }
        }
      }
    }
    if (output) output[count] = step;
    ++count;
    chain.node = predecessor;
    chain.transition = transition;
  }
  return count;
}
__global__ void countReportChains(DeviceView view, const ReportChain* chains, Index count, Index* offsets, Status* status)
{
  for (Index i = blockIdx.x * blockDim.x + threadIdx.x; i <= count; i += blockDim.x * gridDim.x)
    offsets[i] = i == count ? 0 : walkReportChain(view, chains[i], nullptr, status);
}
__global__ void fillReportChains(DeviceView view, const ReportChain* chains, Index count, const Index* offsets,
                                 ReportChainStep* output, Status* status)
{
  for (Index i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += blockDim.x * gridDim.x)
    static_cast<void>(walkReportChain(view, chains[i], output + offsets[i], status));
}

bool cachedReportChain(const CsrReportValues& cache, const PathDomain& domain, ReportChain chain,
                       std::vector<NodeId>& nodes, std::vector<CsrQueryPath>& paths)
{
  while (chain.node != absent) {
    const auto found = cache.nodes.find(chain.node);
    if (found == cache.nodes.end()) return false;
    nodes.push_back(NodeId{chain.node});
    if (chain.transition >= 2) return true;
    if (chain.clock) {
      const auto& node = found->second.timing;
      if (!node.propagated_clock) return true;
      const auto& lane = node.physical_clock[chain.analysis * 2 + chain.transition];
      chain.node = lane.predecessor;
      chain.transition = lane.predecessor_transition;
    } else {
      const Index row_id = ((chain.node * 2 + chain.analysis) * 2 + chain.source) * 2 + chain.transition;
      const auto row = domain.rows[row_id];
      const auto end = domain.tags.begin() + row.end;
      const auto tag = std::lower_bound(domain.tags.begin() + row.begin, end, chain.tag);
      if (tag == end || *tag != chain.tag) return true;
      const Index slot = tag - domain.tags.begin();
      const auto value = cache.paths.find(slot);
      if (value == cache.paths.end()) return false;
      paths.push_back({row_id, slot, chain.tag});
      if (!value->second.present) return true;
      chain.node = value->second.predecessor_node;
      chain.transition = value->second.predecessor_transition;
    }
  }
  return true;
}
}  // namespace

std::vector<ReportChainStep> CudaDualCsrSession::Impl::readReportChains(std::span<const ReportChain> chains)
{
  if (chains.empty()) return {};
  if (chains.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::length_error("report chain scan capacity exceeded");
  const auto threads = storage->kernel_threads;
  std::vector<ReportChainStep> steps;
  Index count = 0;
  Status status = Status::success;
  memory.bindTelemetry(telemetry.cuda);
  try {
    const auto ensure = [&]<class T>(DeviceBuffer<T>& buffer, std::size_t size) {
      telemetry.grown_buffers += memory.ensure(buffer, size);
      return buffer.data;
    };
    auto* roots = ensure(storage->report_chains, chains.size());
    auto* offsets = ensure(storage->report_chain_offsets, chains.size() + 1);
    auto* device_status = ensure(storage->report_chain_status, 1);
    std::size_t bytes = 0;
    check(cub::DeviceScan::ExclusiveSum(nullptr, bytes, offsets, offsets, static_cast<int>(chains.size() + 1), memory.stream()),
          "report chain scan scratch");
    auto* scratch = ensure(storage->algorithm_scratch, bytes);
    memory.upload(chains.data(), roots, chains.size());
    memory.zero(device_status, 1);
    countReportChains<<<kernelBlocks(chains.size() + 1, threads), threads, 0, memory.stream()>>>(
        view, roots, chains.size(), offsets, device_status);
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "count report chains");
    check(cub::DeviceScan::ExclusiveSum(scratch, bytes, offsets, offsets, static_cast<int>(chains.size() + 1), memory.stream()),
          "report chain scan");
    ++telemetry.cuda.device_algorithm_calls;
    auto prior_bytes = telemetry.cuda.d2h_bytes;
    auto prior_wait = telemetry.cuda.stream_wait_ms;
    memory.download(offsets + chains.size(), &count, 1);
    memory.download(device_status, &status, 1);
    memory.synchronize();
    telemetry.cuda.result_d2h_bytes += telemetry.cuda.d2h_bytes - prior_bytes;
    telemetry.cuda.result_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
    if (status != Status::success) throw CudaError("report predecessor chain is cyclic or invalid");
    // The maximum is checked before allocating or accepting a device-sized output.
    if (count > checkedProduct(view.node_count, chains.size())) throw CudaError("report chain count exceeds its domain");
    steps.resize(count);
    auto* output = ensure(storage->report_chain_steps, count);
    fillReportChains<<<kernelBlocks(chains.size(), threads), threads, 0, memory.stream()>>>(
        view, roots, chains.size(), offsets, output, device_status);
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "fill report chains");
    prior_bytes = telemetry.cuda.d2h_bytes;
    prior_wait = telemetry.cuda.stream_wait_ms;
    memory.record(6);
    memory.download(output, steps);
    memory.record(7);
    memory.synchronize();
    telemetry.cuda.delivery_d2h_ms += memory.milliseconds(6, 7);
    telemetry.cuda.result_d2h_bytes += telemetry.cuda.d2h_bytes - prior_bytes;
    telemetry.cuda.result_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
    return steps;
  } catch (...) {
    // Stack-backed transfers and uploaded request spans must retire before unwinding.
    try { memory.synchronize(); } catch (...) { poisoned = storage->poisoned = true; }
    try { throw; }
    catch (const CudaAllocationError&) { throw; }
    catch (const CudaError&) { poisoned = storage->poisoned = true; throw; }
    catch (...) { throw; }
  }
}

std::vector<CsrQueryPath> CudaDualCsrSession::Impl::prepareReportRead(const CsrReportRequest& request)
{
  if (values) { report_values.complete = &*values; return {}; }
  const auto& domain = storage->domain.host;
  std::vector<NodeId> nodes(request.nodes.begin(), request.nodes.end());
  std::vector<CsrQueryPath> paths;
  for (const auto node : request.all_path_nodes) {
    demand(node.value() < view.node_count, "report endpoint is out of range");
    nodes.push_back(node);
    for (Index row = node.value() * 8; row < (node.value() + 1) * 8; ++row)
      for (Index slot = domain.rows[row].begin; slot < domain.rows[row].end; ++slot)
        paths.push_back({row, slot, domain.tags[slot]});
  }
  std::vector<ReportChain> chains;
  for (const auto& root : request.clock_chains)
    chains.push_back({root.node.value(), 0, static_cast<unsigned>(root.analysis), 0, static_cast<unsigned>(root.transition), true});
  for (const auto& root : request.path_chains)
    chains.push_back({root.endpoint.value(), root.tag.value(), static_cast<unsigned>(root.analysis),
                      static_cast<unsigned>(root.source), static_cast<unsigned>(root.transition), false});
  const auto key = [](const ReportChain& root) {
    return std::tuple{root.node, root.tag, root.analysis, root.source, root.transition, root.clock};
  };
  std::ranges::sort(chains, {}, key);
  chains.erase(std::unique(chains.begin(), chains.end(), [&](const auto& a, const auto& b) { return key(a) == key(b); }), chains.end());
  std::erase_if(chains, [&](const ReportChain& root) {
    demand(root.node < view.node_count && root.analysis < 2 && root.source < 2 && root.transition <= 2,
           "report chain root is invalid");
    return cachedReportChain(report_values, domain, root, nodes, paths);
  });
  const auto steps = readReportChains(chains);
  for (const auto& step : steps) {
    nodes.push_back(NodeId{step.node});
    if (step.slot != absent) paths.push_back({step.row, step.slot, step.tag});
  }
  static_cast<void>(readReportValues(nodes, paths));
  return paths;
}

std::vector<CsrQueryPath> CudaDualCsrSession::prepareReport(const CsrReportRequest& request) const
{
  requireCommitted();
  return _impl->prepareReportRead(request);
}
std::span<const std::string> CudaDualCsrSession::reportClockNames() const
{
  requireCommitted();
  return _impl->host->layout.clock_names;
}
}  // namespace ista::csr
