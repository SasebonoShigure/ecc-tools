#include <algorithm>
#include <chrono>

#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"
#include "MixedLevelReconstructor.hpp"
#include "MixedTopologyInternal.hpp"

namespace ista::csr {
using namespace cuda_detail;
namespace mixed = timing_incremental_levelization;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct OldLevelCopy
{
  Index begin, count, destination;
};
__device__ void fail(DeviceView view)
{
  recordFailure(view.status, Status::invariant);
}
__global__ void insertNewLevelNodes(DeviceView view, const NewNodePlacement* nodes, Index count)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count; i += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto value = nodes[i];
    if (value.node >= view.node_count || value.position >= view.node_count) {
      fail(view);
      continue;
    }
    view.order[value.position] = value.node;
  }
}
__global__ void reconstructNodeToLevel(DeviceView view, Index offset_count)
{
  if (readStatus(view.status) != Status::success)
    return;
  if (blockIdx.x == 0 && threadIdx.x == 0)
    *view.level_offset_count = offset_count;
  if (!offset_count || offset_count > view.node_count + 1 || view.level_offsets[0] != 0 || view.level_offsets[offset_count - 1] != view.node_count) {
    fail(view);
    return;
  }
  for (Index position = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; position < view.node_count;
       position += static_cast<Index>(gridDim.x) * blockDim.x) {
    const Index node = view.order[position];
    Index low = 0, high = offset_count - 1;
    while (low + 1 < high) {
      const Index middle = low + (high - low) / 2;
      if (view.level_offsets[middle] <= position)
        low = middle;
      else
        high = middle;
    }
    if (node >= view.node_count || low + 1 >= offset_count || view.level_offsets[low] > position || view.level_offsets[low + 1] <= position) {
      fail(view);
      continue;
    }
    // Inverse construction also detects duplicate nodes before propagation can
    // consume an incomplete permutation. No independent replay is needed.
    if (atomicCAS(reinterpret_cast<unsigned long long*>(view.node_levels + node), 0ULL, static_cast<unsigned long long>(low + 1)) != 0ULL)
      fail(view);
  }
}
unsigned blocks(Index count)
{
  return static_cast<unsigned>(std::max<Index>(1, std::min<Index>(65535, count / 256 + (count % 256 != 0))));
}
}  // namespace

std::optional<CsrFifoLevelization> CudaDualCsrSession::Impl::reconstructMixed()
{
  demand(origin && origin->levels.complete, "mixed reconstruction requires a successful origin");
  Status patch_status;
  try {
    const auto prior_readback = telemetry.cuda.d2h_bytes;
    const auto prior_wait = telemetry.cuda.stream_wait_ms;
    memory.download(view.status, &patch_status, 1);
    memory.synchronize();
    telemetry.cuda.status_d2h_bytes += telemetry.cuda.d2h_bytes - prior_readback;
    telemetry.cuda.status_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
  } catch (...) {
    try {
      memory.synchronize();
    } catch (...) {
      storage->poisoned = true;
    }
    throw;
  }
  if (patch_status != Status::success) {
    code = CsrComputationCode::kInvariantViolation;
    return std::nullopt;
  }
  const auto& cached = origin->values;
  const mixed::detail::PreviousLevels previous = cached
      ? mixed::detail::PreviousLevels(cached->level_offsets, cached->level_nodes, cached->node_to_level, true)
      : mixed::detail::PreviousLevels(origin->levels);
  const mixed::MixedLevelizationRequest request{origin->view.node_count, origin->generation.value(), generation.value()};
  std::vector<std::uint8_t> root_state(view.node_count, 0);
  const mixed::detail::TopologyTraversal topology{static_cast<std::size_t>(view.node_count), [&](const mixed::detail::TopologyVisitor& visitor) {
                                                    for (const auto& arc : host->bindings) {
                                                      if (!arc.active)
                                                        continue;
                                                      if (arc.source >= view.node_count || arc.sink >= view.node_count)
                                                        return EdgeVisitResult::fail(CsrViewError::kNodeOutOfRange);
                                                      if (arc.levelization)
                                                        root_state[arc.sink] = 1;
                                                      const auto result = visitor({NodeId{arc.source}, NodeId{arc.sink}, arc.levelization, arc.storage_kind});
                                                      if (result.control != EdgeVisitControl::kContinue)
                                                        return result;
                                                    }
                                                    return EdgeVisitResult::continueVisit();
                                                  }};
  const auto build_start = Clock::now();
  auto built = mixed::detail::buildOwnedMixedGraph(topology, previous, request);
  const auto build_ms = elapsed(build_start);
  auto reject = [&](mixed::MixedStageFailureCode failure) {
    telemetry.mixed_failure = failure;
    code = failure == mixed::MixedStageFailureCode::kActualCycle ? CsrComputationCode::kCycle : CsrComputationCode::kInvariantViolation;
  };
  auto classify = [&] {
    const auto actual = mixed::detail::classifyActualDag(topology);
    if constexpr (incregpusta_debug_checks_enabled) {
      ++telemetry.mixed.actual_cycle_classifier_runs;
      telemetry.mixed.actual_cycle_classifier_arc_visits += actual.eligible_arc_visits;
    }
    switch (actual.code) {
      case mixed::ActualDagClassificationCode::kDag:
        reject(mixed::MixedStageFailureCode::kOldLevelOrderConflict);
        break;
      case mixed::ActualDagClassificationCode::kCycle:
        reject(mixed::MixedStageFailureCode::kActualCycle);
        break;
      case mixed::ActualDagClassificationCode::kViewError:
        reject(mixed::MixedStageFailureCode::kViewError);
        break;
      case mixed::ActualDagClassificationCode::kInvariantFailure:
        reject(mixed::MixedStageFailureCode::kInvariantFailure);
        break;
    }
  };
  if (const auto* failure = std::get_if<mixed::MixedGraphBuildFailure>(&built)) {
    telemetry.mixed = failure->telemetry;
    telemetry.mixed.mixed_build_ms = build_ms;
    switch (failure->code) {
      case mixed::MixedGraphBuildFailureCode::kOldOldOrderConflict:
        classify();
        break;
      case mixed::MixedGraphBuildFailureCode::kViewError:
        reject(mixed::MixedStageFailureCode::kViewError);
        break;
      case mixed::MixedGraphBuildFailureCode::kMalformedPreviousLevels:
        reject(mixed::MixedStageFailureCode::kMalformedPreviousLevels);
        break;
      case mixed::MixedGraphBuildFailureCode::kMappingOverflow:
        reject(mixed::MixedStageFailureCode::kMappingOverflow);
        break;
      case mixed::MixedGraphBuildFailureCode::kInvariantFailure:
        reject(mixed::MixedStageFailureCode::kInvariantFailure);
        break;
    }
    return std::nullopt;
  }
  const auto& graph = std::get<mixed::MixedPrecedenceGraph>(built);
  telemetry.mixed = graph.telemetry;
  telemetry.mixed.mixed_build_ms = build_ms;
  const auto kahn_start = Clock::now();
  auto kahn = mixed::detail::levelizeBuiltMixedGraph(graph);
  telemetry.mixed.mixed_kahn_ms = elapsed(kahn_start);
  if (const auto* failure = std::get_if<mixed::MixedKahnFailure>(&kahn)) {
    if (failure->code == mixed::MixedKahnFailureCode::kMixedCycle)
      classify();
    else
      reject(mixed::MixedStageFailureCode::kInvariantFailure);
    return std::nullopt;
  }
  // Roots are external patch input. Check their actual eligible incidences
  // gathered during the required topology traversal, not with a second scan.
  for (Index root : host->roots) {
    if (root >= view.node_count || root_state[root] != 0) {
      reject(mixed::MixedStageFailureCode::kInvariantFailure);
      return std::nullopt;
    }
    root_state[root] = 2;
  }
  if (std::find(root_state.begin(), root_state.end(), 0) != root_state.end()) {
    reject(mixed::MixedStageFailureCode::kInvariantFailure);
    return std::nullopt;
  }
  const auto& levels = std::get<mixed::MixedKahnLevelization>(kahn);
  const auto prepare_start = Clock::now();
  std::vector<Index> offsets{0};
  std::vector<NewNodePlacement> new_nodes;
  std::vector<OldLevelCopy> copies;
  new_nodes.reserve(graph.domain.n_new_nodes);
  copies.reserve(graph.domain.n_old_levels);
  Index destination = 0;
  for (Index level = 0; level + 1 < levels.level_offsets.size(); ++level) {
    bool has_old = false;
    for (Index position = levels.level_offsets[level]; position < levels.level_offsets[level + 1]; ++position) {
      const auto vertex = levels.level_vertices[position];
      if (mixed::isVirtualOldLevel(vertex, graph.domain)) {
        demand(!has_old, "mixed frontier has multiple old levels");
        has_old = true;
        const Index begin = previous.level_offsets[vertex.value()], end = previous.level_offsets[vertex.value() + 1];
        demand(begin < end && end <= origin->view.node_count, "old level D2D range invalid");
        copies.push_back({begin, end - begin, destination});
        destination = checkedAdd(destination, end - begin);
      } else {
        const auto decoded = mixed::newNodeForMixedVertex(vertex, graph.domain);
        const auto* node = std::get_if<mixed::NewActualNode>(&decoded);
        demand(node != nullptr, "mixed new-node mapping failed");
        new_nodes.push_back({node->value().value(), destination});
        destination = checkedAdd(destination, 1);
      }
      demand(destination <= view.node_count, "mixed reconstruction exceeds node capacity");
    }
    demand(destination > offsets.back(), "mixed reconstruction produced an empty level");
    offsets.push_back(destination);
  }
  demand(destination == view.node_count && new_nodes.size() == graph.domain.n_new_nodes && copies.size() == graph.domain.n_old_levels,
         "mixed reconstruction does not cover current nodes");
  telemetry.mixed_prepare_ms = elapsed(prepare_start);
  if constexpr (incregpusta_debug_checks_enabled) {
    telemetry.mixed.kahn_processed_count = levels.processed_count;
    telemetry.mixed.max_frontier = levels.max_frontier;
    telemetry.mixed.reconstructed_level_count = offsets.size() - 1;
  }
  std::optional<CsrFifoLevelization> oracle;
  if constexpr (incregpusta_debug_checks_enabled) {
    const CsrFifoLevelization oracle_previous(true, previous.level_offsets, previous.level_nodes, previous.node_to_level);
    auto result = mixed::reconstructMixedLevelization(graph.domain, oracle_previous, levels);
    demand(std::holds_alternative<CsrFifoLevelization>(result), "CPU reconstruction oracle rejected mixed levels");
    oracle = std::get<CsrFifoLevelization>(std::move(result));
  }
  if (memory.ensure(storage->placements, new_nodes.size()))
    ++telemetry.grown_buffers;
  auto* device_new_nodes = storage->placements.data;
  try {
    memory.record(7);
    for (const auto& copy : copies)
      memory.copy(origin->view.order + copy.begin, view.order + copy.destination, copy.count);
    memory.upload(new_nodes, device_new_nodes);
    memory.upload(offsets, view.level_offsets);
    memory.zero(view.node_levels, view.node_count);
    if (!new_nodes.empty()) {
      insertNewLevelNodes<<<blocks(new_nodes.size()), 256, 0, memory.stream()>>>(view, device_new_nodes, new_nodes.size());
      ++telemetry.cuda.kernel_launches;
      check(cudaGetLastError(), "CUDA insert mixed new nodes launch");
    }
    reconstructNodeToLevel<<<blocks(view.node_count), 256, 0, memory.stream()>>>(view, offsets.size());
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "CUDA mixed node2level launch");
    // Host upload arrays remain alive until this stream finishes consuming them.
    memory.synchronize();
  } catch (...) {
    try {
      memory.synchronize();
    } catch (...) {
      storage->poisoned = true;
    }
    throw;
  }
  return oracle;
}
}  // namespace ista::csr
